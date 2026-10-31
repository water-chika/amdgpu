# Recreating the GFX1201 raw triangle

This tutorial explains how `src/amdgpu_raw_triangle_gfx1201.c` draws a
256×256 triangle on Navi48/GFX1201 without Vulkan. It is deliberately a
reconstruction guide, not merely a capture log: starting with libdrm_amdgpu and
the two shader word streams, the steps below reproduce the program's BOs, GPU
virtual addresses, PM4, submission, SDMA readback, and validation.

The verified device is an RX 9070 XT reporting ASIC ID `0x7550`, four shader
engines (SEs), and GFX1201. The known-good draw submits 433 PM4 dwords. Its
summary validator observes `0xff0000ff` at the first nonzero and center pixels,
18,432 nonzero pixels, and bounding box `32,33..223,223`. It does not compare
every covered pixel against a reference image.

> **Safety and scope.** Raw PM4 is generation-specific. A wrong register,
> address, packet count, or ring size can reset or hang the GPU. Use a render
> node on a disposable session, retain the five-second fence timeouts, and do
> not run this stream on a device other than Navi48 ASIC `0x7550`.

## 1. Architecture

The command path is:

```text
CPU/libdrm                         GFX queue                         DMA queue
-----------                        ---------                         ---------
open render node                   program GE rings
create VM + context                program captured draw state
allocate/map BOs        --IB-->    run merged NGG ES/GS + PS
BO list establishes residency      write linear RGBA8 VRAM
wait kernel GFX fence                                      --copy--> GTT readback
                                                         wait DMA fence
CPU scans mapped GTT
```

GFX12 uses NGG rather than the Renoir/GFX9 VS-only setup. The merged ES/GS
shader generates positions and primitive connectivity. Geometry exchange also
requires application-owned attribute, position, and primitive rings. The pixel
shader emits a packed FP16 MRT0 export which the captured CB state converts to
RGBA8 red. The render target is requested in the VRAM domain; a separate SDMA submission copies
it to CPU-visible GTT.

The implementation has three checkpoints:

* `--stage info`: initialize libdrm, query the GPU, and reject non-`0x7550`.
* `--stage rings`: allocate/program the GE rings and submit only that state.
* `--stage draw`: add shaders, raster/CB state, synchronization, draw, SDMA
  readback, and summary validation of color, coverage count, and bounds.

## 2. Prerequisites and build

Requirements are a recent Linux amdgpu kernel driver, libdrm_amdgpu, Clang/LLVM
with the `gfx1201` target, and GNU Make. LLVM 18 or newer is recommended; the
verified local compiler was Clang 23. The repository helper installs the common
packages:

`tools/build_dependecies.sh` uses Bash syntax and has no shebang, so invoke it
explicitly. On Arch, its `vulkan-devel` package group may prompt for members.

```sh
bash ./tools/build_dependecies.sh
make gfx1201
```

To install the executable and its two required sibling shader binaries together:

```sh
make install-gfx1201 PREFIX=/usr/local
# Use DESTDIR=/tmp/stage for a staged install.
```

The opt-in target builds:

```text
build/amdgpu-raw-triangle-gfx1201
build/gfx12-raw-triangle-ngg.bin
build/gfx12-raw-triangle-ps.bin
```

`tools/extract-text.sh` assembles each `.s` file for `gfx1201`, then extracts
only `.text`. The executable finds the binaries beside itself through
`/proc/self/exe`; keep all three files in the same directory.

Run in increasing order of risk:

```sh
build/amdgpu-raw-triangle-gfx1201 --stage info
build/amdgpu-raw-triangle-gfx1201 --stage rings
build/amdgpu-raw-triangle-gfx1201 --stage draw
build/amdgpu-raw-triangle-gfx1201 --stage draw --dump-pm4
```

Select another render node with `--device /dev/dri/renderD129`.

## 3. Obtaining a clean RADV reference

`RADV_DEBUG=dumpibs` is preferable to live UMR capture because it needs neither
root nor debugfs. Force the sample to remove three common sources of noise:
linear tiling, a procedural vertex path, and no render-pass clear.

On a machine with more than one AMD GPU, force both the RADV ICD and the RX
9070 XT. Adjust the ICD JSON path for the distribution:

```sh
VK_DRIVER_FILES=/usr/share/vulkan/icd.d/radeon_icd.x86_64.json \
MESA_VK_DEVICE_SELECT=1002:7550! \
RADV_TRIANGLE_LINEAR=1 RADV_TRIANGLE_PROCEDURAL=1 \
RADV_TRIANGLE_NO_CLEAR=1 RADV_DEBUG=dumpibs,shaders \
  build/radv-triangle > /tmp/radv-triangle-clean-dumpibs.txt 2>&1
```

Confirm the output names `AMD Radeon RX 9070 XT (RADV GFX1201)` before using it.
The example otherwise selects the first AMD physical device, which may be the
Renoir GPU on a multi-GPU host. `VK_DRIVER_FILES` prevents AMDVLK from satisfying
the same AMD vendor check.

Caveats:

1. `RADV_TRIANGLE_NO_CLEAR=1` removes RADV's internal `RECTLIST` clear and
   `RADV_TRIANGLE_PROCEDURAL=1` removes the vertex-buffer descriptor path. The
   reference still calls `vkCmdCopyImageToBuffer`, so the dump still contains a
   GFX12 compute readback dispatch after the real `TRILIST`. Isolate the triangle
   state and `DRAW_INDEX_AUTO(3)`, then stop before compute setup.
2. GPU addresses in a dump name RADV-owned BOs. They become invalid when RADV
   exits. Copy register values and packet ordering, but substitute addresses of
   BOs allocated by this process.
3. Optimal-tiled and linear images have different CB state. The implementation
   uses the captured linear layout: 262,144 bytes, 256-byte alignment,
   1,024-byte row pitch, `COLOR_SW_MODE=0`, and
   `CB_COLOR0_ATTRIB3=0x01000000`.
4. `dumpibs` is a reference, not a stable ABI. State may move or defaults may
   change between Mesa versions. Preserve the device, Mesa commit, image
   modifiers, and shader disassembly with a capture used for comparison.
5. UMR ring decoding is useful for a live second opinion but requires UMR,
   root/debugfs access, and prompt capture before unrelated queue traffic
   overwrites the ring.

## 4. Encodings used by the program

### 4.1 Type-3 packet headers

The helper encodes:

```c
PKT3(op, n) = (3 << 30) | ((n & 0x3fff) << 16) | (op << 8)
```

Here `n` is **payload dwords minus one**, not the payload count. Thus
`PKT3(0x58, 6)` has seven payload dwords. The packets used are:

| Opcode | Packet | Payload in this program |
| ---: | --- | --- |
| `0x28` | `CONTEXT_CONTROL` | two dwords, both `0x80000000` |
| `0x2d` | `DRAW_INDEX_AUTO` | index count `3`, initiator `2` |
| `0x2f` | `NUM_INSTANCES` | one instance |
| `0x49` | `RELEASE_MEM` | seven GFX12 payload dwords |
| `0x58` | `ACQUIRE_MEM` | seven GFX12 payload dwords |
| `0x69` | `SET_CONTEXT_REG` | register offset, value |
| `0x76` | `SET_SH_REG` | register offset, value |
| `0x79` | `SET_UCONFIG_REG` | register offset, value |
| `0x7a` | `SET_UCONFIG_REG_INDEX` | indexed offset, value |

For the two `0x80000000` payloads and the distinction between context masks and
actual graphics state, see [`PM4 CONTEXT_CONTROL`](pm4-context-control.md).

Register packet offsets are relative to distinct spaces:

```text
CONTEXT absolute byte address = 0x28000 + 4 * offset
SH      absolute byte address = 0x0b000 + 4 * offset
UCONFIG absolute byte address = 0x30000 + 4 * offset
```

`ucfg_idx(0x10000242, 4)` intentionally retains index/control bits in the first
payload while its low register offset identifies `VGT_PRIMITIVE_TYPE`.

### 4.2 Address fields

The stream never writes a full 64-bit address into one register. Apply exactly
the shift and split expected by each block:

| Consumer | Encoding |
| --- | --- |
| shader `PGM_LO` | bits `[39:8]`: `(uint32_t)(va >> 8)` |
| shader `PGM_HI` | bits `[47:40]`: `(va >> 40) & 0xff` |
| `CB_COLOR0_BASE` | `(uint32_t)(va >> 8)` |
| `CB_COLOR0_BASE_EXT` | `(va >> 40) & 0xff` |
| attribute/position/primitive ring base | `(uint32_t)(va >> 16)` |
| SDMA source/destination | low and high 32-bit dwords, unshifted |
| GFX/SDMA IB address | full 64-bit `ib_mc_address` supplied to libdrm |

Shader and color BOs therefore need at least 256-byte alignment; ring bases need
64 KiB alignment. `AMDGPU_VA_RANGE_HIGH | AMDGPU_VA_RANGE_32_BIT` puts BOs into
the driver's high 32-bit-compatible VA window. The common high component is
reported by `amdgpu_sw_info_address32_hi`.

## 5. BO, VA, and residency lifecycle

For every BO, reproduce this order. `preferred_heap` requests VRAM or GTT but
is a placement preference rather than an absolute residency guarantee:

1. Round the requested byte count up to the physical alignment.
2. `amdgpu_bo_alloc` in VRAM or GTT. Add
   `AMDGPU_GEM_CREATE_CPU_ACCESS_REQUIRED` only when CPU mapping is required.
3. `amdgpu_va_range_alloc` with the same size/alignment and the high/32-bit
   flags.
4. `amdgpu_bo_va_op(..., AMDGPU_VA_OP_MAP)` with readable, writable, and
   executable VM permissions. Executable is needed by shader BOs; the tutorial
   keeps one common mapping policy for all BOs.
5. If CPU-accessible, `amdgpu_bo_cpu_map`, zero the whole aligned BO, then copy
   code or PM4.
6. Put the IB and every referenced BO in an `amdgpu_bo_list_create` list. The
   list is the submission's residency declaration; a valid VA alone is not
   enough.
7. Submit, wait for the kernel fence, and destroy the temporary BO list.
8. At teardown: CPU-unmap, VM-unmap, free VA range, then free BO.

BO inventory:

| BO | Heap | Alignment | CPU map | Purpose |
| --- | --- | ---: | --- | --- |
| GE rings | VRAM | 64 KiB | no | attribute + position + primitive rings |
| NGG shader | VRAM | 256 B | yes | upload raw merged ES/GS words |
| PS shader | VRAM | 256 B | yes | upload raw pixel shader words |
| color | VRAM | 256 B | yes | 256×256×4 linear render target |
| readback | GTT | 4 KiB | yes | SDMA destination inspected by CPU |
| GFX IB | GTT | 4 KiB | yes | emitted PM4 |
| SDMA IB | GTT | 4 KiB | yes | seven-dword linear copy packet |

The color BO is CPU-mapped and zeroed so uncovered pixels start at zero; this
relies on host writes to visible VRAM becoming available before GFX execution.
Validation does not CPU-read that VRAM mapping: it exercises an explicit SDMA
copy to GTT and then reads the GTT destination.

## 6. Deriving the four-SE GE ring allocation

Mesa's Navi48 defaults are 1,400 KiB attribute storage per SE, 16,384 position
exports per SE, and 16,368 primitive exports per SE. Attribute storage is first
rounded to 64 KiB, becoming 1,408 KiB per SE. Position and primitive capacities
are converted to bytes, aligned as Mesa specifies, and their total allocation
uses `SE²`:

```text
attribute_per_SE = align(1400 KiB, 64 KiB) = 1408 KiB = 1,441,792 B
position_per_SE  = align(16,384 * 16 B, 32 B) = 262,144 B
primitive_per_SE = align(16,368 * 4 B, 32 B)  = 65,472 B

for SE = 4:
attribute_total = 1,441,792 * 4                   = 5,767,168 B = 0x580000
position_total  = align(262,144 * 4², 64 KiB)    = 4,194,304 B = 0x400000
primitive_total = align(65,472 * 4², 64 KiB)     = 1,048,576 B = 0x100000
total                                                    = 11,010,048 B = 0xa80000

position_base_offset  = 0x580000
primitive_base_offset = 0x580000 + 0x400000 = 0x980000
```

Lay them contiguously:

```text
attribute = rings.va
position  = rings.va + attribute_total
primitive = position + position_total
```

Programming units differ: all three bases are in 64 KiB units; attribute size
is `(bytes_per_SE >> 16) - 1`, while position and primitive sizes are in 32-byte
units. GFX12 requires all four `GE_POS/PRIM_RING_{BASE,SIZE}` registers to be
updated together. `GE_PRIM_RING_SIZE=0x0c6e0000 | (primitive_per_SE >> 5)` also
carries captured scope, temporal, speculative-read, force-SE-scope, and no-fill
policy fields. Do not replace the queried SE count silently when porting; the
current fallback of four exists only for the verified target.

## 7. Shader word streams and disassembly

The source files contain `.long` words because assembler syntax support varied
when the stream was captured. The comments identify them as captured RADV shader
words, not as data loaded from a repository-external `/tmp` file.

### 7.1 Merged NGG ES/GS

| Byte | Raw word(s) | Disassembly | Role |
| ---: | --- | --- | --- |
| `00` | `bfb50003` | `s_setprio 3` | Raise shader priority. |
| `04` | `99000380` | `s_pack_ll_b32_b16 s0, 0, s3` | Pack invocation mask source. |
| `08` | `bf870009` | `s_delay_alu instid0(SALU_CYCLE_1)` | Satisfy scalar dependency. |
| `0c` | `947e00c1` | `s_bfe_u64 exec, -1, s0` | Form active-lane mask. |
| `10` | `bfa50011` | `s_cbranch_execz 17` | Skip position work for no lanes. |
| `14` | `4a060608` | `v_add_nc_u32_e32 v3, s8, v3` | Compute procedural vertex ID. |
| `18` | `7e040280` | `v_mov_b32_e32 v2, 0` | Position Z = 0. |
| `1c` | `7e0802f2` | `v_mov_b32_e32 v4, 1.0` | Position W = 1. |
| `20` | `bf870003` | `s_delay_alu instid0(VALU_DEP_3)` | Wait for vertex-ID dependency. |
| `24` | `7c840681` | `v_cmp_eq_i32_e32 vcc_lo, 1, v3` | Select vertex 1. |
| `28` | `d5010001 41a9feff bf400000` | `v_cndmask_b32_e64 v1, 0xbf400000, neg(0xbf400000), vcc_lo` | Choose X = ±0.75. |
| `34` | `7c840680` | `v_cmp_eq_i32_e32 vcc_lo, 0, v3` | Select vertex 0. |
| `38` | `bf88fffd` | `s_wait_alu 0xfffd` | Resolve pending ALU dependencies. |
| `3c` | `d5010001 01a90101` | `v_cndmask_b32_e64 v1, v1, 0, vcc_lo` | Vertex 0 X = 0. |
| `44` | `d5010003 41a9feff 3f400000` | `v_cndmask_b32_e64 v3, 0x3f400000, neg(0x3f400000), vcc_lo` | Choose Y = ±0.75. |
| `50` | `f80008cf 04020301` | `export pos0, v1, v3, v2, v4 done` | Export clip position. |
| `58` | `84038803` | `s_lshl_b32 s3, s3, 8` | Prepare primitive lane mask. |
| `5c` | `bfc40000` | `s_wait_expcnt 0` | Wait for position export. |
| `60` | `bf870009` | `s_delay_alu instid0(SALU_CYCLE_1)` | Scalar dependency delay. |
| `64` | `947e03c1` | `s_bfe_u64 exec, -1, s3` | Activate primitive-export lane. |
| `68` | `bfa50002` | `s_cbranch_execz 2` | Skip absent primitive. |
| `6c` | `f8000941 80808000` | `export prim, v0, off, off, off done` | Export primitive connectivity. |
| `74` | `bfb00000` | `s_endpgm` | End NGG wave. |

The generated vertices are `(0,-0.75)`, `(0.75,0.75)`, and
`(-0.75,0.75)` in clip space. With a 256×256 viewport they explain the expected
screen bounds near 32 and 224.

### 7.2 Pixel shader

| Byte | Raw word(s) | Disassembly | Role |
| ---: | --- | --- | --- |
| `00` | `7e0002ff 00003c00` | `v_mov_b32_e32 v0, 0x3c00` | Pack FP16 R=1, G=0 into `v0`. |
| `08` | `7e0202ff 3c000000` | `v_mov_b32_e32 v1, 0x3c000000` | Pack FP16 B=0, A=1 into `v1`. |
| `10` | `f8000803 80800100` | `export mrt0, v0, v1, off, off done` | Export packed FP16 RGBA. |
| `18` | `bfb00000` | `s_endpgm` | End PS wave. |

`0x3c00` is FP16 `1.0`. With the little-endian packed halves and captured
`SPI_SHADER_COL_FORMAT`/CB format, the logical output is red with alpha and the
RGBA8 memory dword is `0xff0000ff`. Interpreting the immediate bytes directly
as color channels is incorrect.

## 8. Register state, grouped and classified

All offsets below are packet-relative dword offsets. Provenance classes are:

* **Capture** — copied from the local GFX1201 RADV procedural/linear experiment.
  The generated capture is not stored in the repository; reproduce it with the
  command in section 3 before auditing or changing these values.
* **Mesa-derived** — calculated according to pinned Mesa ring logic.
* **Address** — captured register choice with this process's BO address inserted.
* **Structural** — zero/default repetition required to reproduce the captured
  state; its individual field meaning has not been independently minimized.

### 8.1 GE rings (`UCONFIG`)

| Offset | Register | Value | Provenance |
| ---: | --- | ---: | --- |
| `444..447` | `SPI_GS_THROTTLE_CNTL1/2`, `SPI_ATTRIBUTE_RING_BASE/SIZE` | `12355123`, `0001544d`, `attr>>16`, `00020000|((1408KiB>>16)-1)` | Mesa-derived + Capture |
| `268..26b` | `GE_POS_RING_BASE/SIZE`, `GE_PRIM_RING_BASE/SIZE` | `pos>>16`, `262144>>5`, `prim>>16`, `0c6e0000|(65472>>5)` | Mesa-derived + Capture |

### 8.2 Color target, masks, and blending (`CONTEXT`)

| Offset | Register | Value | Provenance |
| ---: | --- | ---: | --- |
| `318..31c` | `CB_COLOR0_BASE`, `VIEW`, `VIEW2`, `ATTRIB`, `FDCC_CONTROL` | `color>>8`, `0`, `0`, `0`, `10000004` | Address + Capture |
| `31e`, `31f`, `390` | `CB_COLOR0_ATTRIB2`, `ATTRIB3`, `BASE_EXT` | `00ff00ff`, `01000000`, `(color>>40)&ff` | Capture + Address |
| `3b0..3b7` | `CB_COLOR0_INFO` and the seven following color-slot INFO registers | `0002800a`, then seven zeros | Capture + Structural |
| `214`, `215`, `216` | `CB_TARGET_MASK`, `CB_SHADER_MASK`, `CB_COLOR_CONTROL` | `f`, `f`, `00cc0010` | Capture |
| `1d5..1d7` | `SX_PS_DOWNCONVERT`, `SX_BLEND_OPT_EPSILON`, `SX_BLEND_OPT_CONTROL` | `5`, `6`, `0` | Capture |
| `1d8..1df` | `SX_MRT0_BLEND_OPT` through MRT7 | eight × `06000600` | Capture + Structural |
| `1e0..1e7` | `CB_BLEND0_CONTROL` through blend7 | eight × `0` | Capture + Structural |

The target is exactly `256 * 256 * 4 = 262144` bytes. Linear swizzle mode zero,
base in 256-byte units, and `ATTRIB3=0x01000000` are a single matched layout;
do not transplant only one of them.

### 8.3 Viewport, scissor, raster, and samples (`CONTEXT`)

| Offset | Register/group | Value(s) | Provenance |
| ---: | --- | --- | --- |
| `10b..10e` | `PA_CL_GB_{VERT,HORZ}_{CLIP,DISC}_ADJ` | `43800000,3f800000,43800000,3f800000` | Capture |
| `10f..116` | viewport X/Y/Z scale/offset and Z min/max | `43000000` ×4, `3f800000,0,0,3f800000` | Capture |
| `81`, `82` | window scissor TL/BR | `0`, `00ff00ff` | Capture |
| `61` | screen scissor BR | `01000100` | Capture |
| `94`, `95` | viewport scissor 0 TL/BR | `0`, `00ff00ff` | Capture |
| `83`, `8d` | cliprect rule, hardware screen offset | `ffff`, `00080008` | Capture |
| `190`, `192..198` | PS input/control and shader export formats | `0,1,4,0,4,0,1,1` | Capture |
| `204`, `206`, `207` | clip control, VS-out control, raster mode | `01080000`, `0`, `240` | Capture |
| `282`, `283`, `291`, `2f7` | line width/stipple/reset/control | `8`, `00ff0000`, `2`, `0` | Capture |
| `292`, `293` | `PA_SC_MODE_CNTL_0/1` | `22`, `060201bc` | Capture |
| `2f8`, `30e`, `30f`, `317` | AA config/masks and sample properties | `0`, `ffffffff`, `ffffffff`, `0` | Capture |
| `2fc..2fe`, `302`, `306`, `30a` | centroid priorities and sample locations | all `0` | Structural |
| `311`, `315` | binner and conservative-raster controls | `19fc01b0`, `00100000` | Capture |

The viewport values include `128.0` (`0x43000000`) and `1.0`
(`0x3f800000`). Together with the captured clip-adjustment constants and
scissors they implement the clip-to-window behavior used by the validation
math.

### 8.4 Depth, stages, NGG, and PS interface (`CONTEXT/UCONFIG`)

| Offset | Register/group | Value(s) | Provenance |
| ---: | --- | --- | --- |
| `03,04,06,07` | `DB_RENDER_OVERRIDE`, `DB_RENDER_OVERRIDE2`, `DB_Z_INFO`, `DB_STENCIL_INFO` | `1000,08000000,0,20000000` | Capture |
| `18`, `19`, `1b`, `1c`, `1e`, `1f` | DB count/viewport/shader/depth/EQAA/alpha-to-mask controls | `0,0,10,00700770,00110000,00018700` | Capture |
| `1ff` | `GE_MAX_OUTPUT_PER_SUBGROUP` | `80` | Capture |
| `2a6`, `2a8` | `VGT_SHADER_STAGES_EN`, `VGT_DRAW_PAYLOAD_CNTL` | `04000000`, `40` | Capture |
| `2cf`, `2d3` | `VGT_GS_INSTANCE_CNT`, `GE_NGG_SUBGRP_CNTL` | `4`, `1` | Capture |
| `2e5`, `2ef` | `PA_SC_HIZ_INFO`, `PA_SC_HISZ_CONTROL` | `0`, `2` | Capture |
| `212` CONTEXT | `PA_CL_VRS_CNTL` | `0` | Capture |
| `263` UCONFIG | `GE_VRS_RATE` | `0` | Capture |
| `25b`, `262`, `266` UCONFIG | `GE_CNTL`, `VGT_PRIMITIVEID_EN`, `VGT_GS_OUT_PRIM_TYPE` | `a0010080`, `0`, `2` | Capture |
| `242` indexed UCONFIG | `VGT_PRIMITIVE_TYPE` | `4` (`TRILIST`) | Capture |
| `196`, `197`, `198` | `SPI_BARYC_CNTL`, `SPI_PS_INPUT_ENA`, `SPI_PS_INPUT_ADDR` | `0`, `1`, `1` | Capture |
| `2f7`, `f4` | `PA_SC_LINE_CNTL`, `PA_SC_VRS_OVERRIDE_CNTL` | `0`, `51` | Capture |

Several raw captured values above combine fields whose public names vary with
generated-header revision. Their byte offsets and values reproduce the verified
local experiment, but the generated clean capture is not stored in the
repository and every bit has not been independently minimized. Regenerate the
section-3 capture before changing or auditing them.

### 8.5 Shader programs (`SH`)

| Offset | Register | Value | Provenance |
| ---: | --- | ---: | --- |
| `89`, `86` | `SPI_SHADER_PGM_LO_ES`, `SPI_SHADER_PGM_HI_ES` | `ngg>>8`, `(ngg>>40)&ff` | Address |
| `8a`, `8b`, `88`, `8d` | `SPI_SHADER_PGM_RSRC1_GS`, `RSRC2_GS`, `RSRC4_GS`, `USER_DATA_GS_1` | `000c0002,4,01ff0bff,0` | Capture |
| `08`, `09` | `SPI_SHADER_PGM_LO_PS`, `SPI_SHADER_PGM_HI_PS` | `ps>>8`, `(ps>>40)&ff` | Address |
| `0a`, `0b`, `07`, `31` | `SPI_SHADER_PGM_RSRC1_PS`, `RSRC2_PS`, `RSRC4_PS`, `SPI_SHADER_GS_OUT_CONFIG_PS` | `000c0002,4,000207ff,400` | Capture |
| `8c` | `SPI_SHADER_USER_DATA_GS_0` | `0` immediately before draw | Capture |

## 9. Synchronization, fences, and SDMA

GFX12 `ACQUIRE_MEM` and `RELEASE_MEM` use a header count of six and seven
payload dwords. Do not confuse the two different `ACQUIRE_MEM` layouts:

```text
plain cache acquire (Mesa ac_emit_cp_acquire_mem):
  control/engine, ffffffff, 00ffffff, 0, 0, poll=0000000a, GCR_CNTL

PWS wait (Mesa ac_emit_cp_acquire_mem_pws):
  PWS control, ffffffff, 01ffffff, 0, 0, PWS_ENA=80000000, GCR_CNTL
```

The current prototype's initial `acquire(control=0, GCR=0x0000c181)` reuses the
PWS-shaped helper even though it does not enable a PWS wait. That stream passed
on the tested device, but a new implementation should emit the plain form for a
plain cache operation rather than copying this hybrid.

### 9.1 Safe GE-ring update ordering

Mesa's `radv_emit_ge_rings` says the queue **must be idle** before changing the
attribute/position/primitive ring registers. The production ordering is:

1. `RELEASE_MEM(BOTTOM_OF_PIPE_TS, PWS_ENABLE=1)`.
2. PWS `ACQUIRE_MEM` waiting at CP ME on the same event/counter.
3. Program all attribute, position, and primitive ring registers.
4. On GFX12 with PFP firmware >= 2680, emit
   `UPDATE_DB_SUMMARIZER_TIMEOUT` (`PKT3 0xef`) with the timeout selected by the
   partial-HiZ workaround. The observed non-workaround value is zero, but query
   firmware/capability before emitting a firmware-gated packet.

The current small prototype programs the rings after only an initial cache
acquire. It works on its fresh one-shot context, but that shortcut is not the
Mesa queue-update contract and should not be copied into a general implementation.

### 9.2 PWS and frontend ordering

A PWS release/acquire pair is not complete for a CP-PFP consumer until PFP also
waits. Mesa implements a PFP acquire as an ME `ACQUIRE_MEM` followed by:

```text
PKT3_PFP_SYNC_ME (opcode 0x42), payload 0
```

The local prototype omits this after its pre-draw CP-PFP PWS acquire. Its direct,
CPU-initialized `DRAW_INDEX_AUTO` passed repeatedly, but add `PFP_SYNC_ME` when
reconstructing the full barrier—especially before PFP consumes data produced by
ME/GPU work.

For the draw-to-SDMA transition, the local optimal capture used release event
word `0x80700514`, carrying GLV invalidation plus GL2 invalidate/writeback. The
prototype currently emits `0x80404514`; the later kernel-appended GFX fence also
performs completion/writeback on the tested kernel, which is why readback passed.
A self-contained stream should preserve the explicit captured GL2 visibility
bits instead of relying on that kernel epilogue detail.

PWS counters are global to the queue timeline. An intervening IB can increment
the counter, so keep each release/acquire relationship in one controlled
submission.

### 9.3 Kernel fences and SDMA

The libdrm GFX fence is a **kernel submission fence**, distinct from PWS:

```text
amdgpu_cs_submit(GFX)
amdgpu_cs_query_fence_status(..., 5 seconds)
```

Only after it signals does the program submit SDMA. CPU ordering between the two
submissions establishes the cross-engine execution dependency; the in-IB
release establishes memory visibility. The SDMA linear copy packet is seven
dwords:

```text
[0] 1                         COPY opcode/header
[1] COLOR_BYTES - 1           byte count minus one
[2] 0                         copy control
[3] src VA low                color
[4] src VA high
[5] dst VA low                readback
[6] dst VA high
```

The SDMA BO list contains its IB, source color BO, and destination GTT BO. Wait
on an `AMDGPU_HW_IP_DMA` kernel fence for five seconds before reading mapped
GTT. Kernel fences prove queue completion and BO lifetime; PWS orders work and
cache effects inside the GFX command stream. They are complementary.

## 10. Exact implementation order

A clean-room recreation should follow this sequence exactly:

1. Parse `--device`, `--stage`, and `--dump-pm4`.
2. Open the render node, call `amdgpu_device_initialize`, query GPU info and
   `address32_hi`, then create one context.
3. Print identity and reject ASIC IDs other than `0x7550`; return for `info`.
4. Use queried `num_shader_engines` (the kernel maximum-SE topology; verified
   value four), derive the three ring sizes, and request one 64-KiB-aligned,
   high-VA, device-only VRAM BO.
5. For a production-safe implementation, emit bottom-of-pipe PWS release/wait
   before updating ring registers, then program throttle/attribute and all four
   position/primitive registers together; include the firmware-gated DB
   summarizer timeout workaround described in section 9. The current prototype's
   shorter `CONTEXT_CONTROL` + initial acquire sequence is verified only for its
   fresh one-shot context.
6. For `rings`, put the ring BO and IB in a GFX BO list, submit, fence-wait,
   print VA/size/dword count, and tear down.
7. Resolve shader paths beside the executable; read nonempty, dword-aligned raw
   files.
8. Allocate 256-byte-aligned VRAM BOs for NGG, PS, and color; allocate a
   4-KiB-aligned GTT readback BO. Zero mappings, then upload shaders.
9. Emit the grouped captured state above, substituting only shader/color BO
   addresses.
10. Emit pre-draw release/acquire PWS synchronization and `PFP_SYNC_ME` when
    acquiring at CP PFP. Treat the prototype's omitted sync packet as a tested
    shortcut, not the general sequence.
11. Emit `NUM_INSTANCES(1)`, `SPI_SHADER_USER_DATA_GS_0=0`, then
    `DRAW_INDEX_AUTO(3, 2)`.
12. Emit the final release with explicit GLV/GL2 visibility fields (the captured
    event word was `0x80700514`). Optionally dump every PM4 dword.
13. Make a GFX BO list containing IB, rings, both shaders, and color; submit and
    wait for its five-second fence.
14. Build the SDMA copy IB, list IB/color/readback, submit on DMA, and wait for
    its five-second fence.
15. Scan the readback and require the exact center, first nonzero, count, and
    bounding box.
16. Destroy in reverse order: IBs/readback/color/PS/NGG/rings, PM4 vector,
    context, device, file descriptor.

## 11. Validation math

The clip-space vertices span X from -0.75 to +0.75 and Y from -0.75 to +0.75.
For a 256-pixel viewport, the nominal transform is:

```text
window_x = clip_x * 128 + 128  => 32, 128, 224
window_y = clip_y * 128 + 128  => 32, 224
```

Rasterization's top-left fill convention yields integer covered bounds
`x=32..223`, `y=33..223`, not inclusive 224. The triangle base and height are
both 192 pixels, so its geometric area is:

```text
192 * 192 / 2 = 18,432 pixels
```

The summary scanner therefore requires:

```text
center = 0xff0000ff
first  = 0xff0000ff
nonzero = 18432
bbox = 32,33..223,223
```

Count and bounds catch errors that a center-pixel check misses: stale clear
data, wrong winding/fill convention, a broken viewport transform, an incorrect
row pitch, or an SDMA copy with the wrong byte count. This is not a full image
comparison: other covered pixels are required to be nonzero, but their colors
are not individually checked.

## 12. Staged debugging

| Stage/symptom | Likely cause | First checks |
| --- | --- | --- |
| `info` cannot open node | wrong render node or permissions | `--device`, render group access |
| wrong ASIC rejection | selected Renoir/another GPU | use RX 9070 XT render node; never bypass guard |
| ring-stage VM fault | ring VA/alignment/size encoding | 64 KiB bases, `SE²` position/primitive totals, BO list residency |
| ring fence timeout/reset | malformed packet count or illegal ring state | `PKT3(...,6)` has seven payloads; update all four GE registers |
| shader file open/size error | binaries not beside executable or stale build | `make gfx1201`; dword-aligned nonempty `.bin` files |
| draw VM fault at shader fetch | bad `PGM_LO/HI` split or missing residency | `>>8`, `>>40`, 256-byte alignment, both shader BOs in list |
| draw completes, all zeros | PS/CB disabled, wrong target base, or missing sync | CB masks/info, `BASE/BASE_EXT`, release/acquire ordering |
| one corner/tiny output | viewport/scissor state wrong | scales/offsets and screen/window/vport scissors |
| right shape, wrong color | mistaken packed-FP16 interpretation or CB format | PS words, shader color format, `CB_COLOR0_INFO` |
| GFX succeeds, SDMA times out | malformed copy packet or DMA queue unavailable | count-minus-one, full VAs, DMA BO list and IP type |
| SDMA succeeds, zeros/stale data | omitted GFX fence/cache release or reversed src/dst | wait GFX first; source=color, destination=GTT |
| count correct, bbox shifted by one | raster fill convention/state differs | compare sample/raster state and clean capture |
| center red but exact check fails | stale pixels, pitch/format mismatch, or changed shader | first value, bbox/count, zero initialization, linear state |

Use `--dump-pm4` only after `rings` is stable. Compare packet boundaries first,
then register offsets, then values; a shifted packet makes every subsequent
“register difference” meaningless.

## 13. Portability boundaries

This is not a general GFX12 renderer.

* ASIC `0x7550`, four-SE sizing, GFX1201 shader words, register values, and
  packet encodings are a matched set.
* Different SE counts require recomputing allocation offsets and sizes. The
  libdrm `num_shader_engines` field used here is the kernel maximum-SE topology
  that Mesa maps to `max_se`; this experiment is nevertheless hardware-validated
  only for the reported value four.
* Another shader needs matching `SPI_SHADER_PGM_RSRC*`, stage/export, user-data,
  PS-input, and color-export state. Raw words alone are insufficient.
* Another format, pitch, extent, or tiling modifier needs new CB state and new
  validation. Optimal tiling cannot use this linear SDMA/image interpretation.
* Another Mesa or firmware version may require state not seen in this capture.
* Renoir/GFX9 uses legacy VS/PS programming and no GFX12 GE position/primitive
  rings or seven-payload GFX12 PWS sequence. Use the existing
  `src/amdgpu_raw_triangle.c` path instead.

Treat captured opaque values as hypotheses when porting: retain the staged
bring-up, change one group at a time, and recapture a known-good RADV draw on the
new target.

## 14. Commit-pinned references

The implementation and terminology can be audited against these immutable
upstream revisions:

### Mesa `eda37f052369d3f1579e932abe940fc9b4026855`

* [`src/amd/common/ac_gpu_info.c`](https://gitlab.freedesktop.org/mesa/mesa/-/blob/eda37f052369d3f1579e932abe940fc9b4026855/src/amd/common/ac_gpu_info.c) — Navi48 attribute size, export counts, alignments, `SE²` totals, and offsets.
* [`src/amd/common/ac_cmdbuf_cp.c`](https://gitlab.freedesktop.org/mesa/mesa/-/blob/eda37f052369d3f1579e932abe940fc9b4026855/src/amd/common/ac_cmdbuf_cp.c) — `ac_emit_cp_gfx11_ge_rings`, GFX12 GE units/policies, PWS acquire/release, and seven-payload packets.
* [`src/amd/registers/gfx12.json`](https://gitlab.freedesktop.org/mesa/mesa/-/blob/eda37f052369d3f1579e932abe940fc9b4026855/src/amd/registers/gfx12.json) — register offset/name and field database.
* [`src/amd/packets/cp_pm4_table_data_gfx12.json`](https://gitlab.freedesktop.org/mesa/mesa/-/blob/eda37f052369d3f1579e932abe940fc9b4026855/src/amd/packets/cp_pm4_table_data_gfx12.json) — GFX12 PM4 packet layouts.

### libdrm `f9816a42b0d6138851cbe5eb7a33ee1a2f2a15ca`

* [`amdgpu/amdgpu_bo.c`](https://gitlab.freedesktop.org/mesa/drm/-/blob/f9816a42b0d6138851cbe5eb7a33ee1a2f2a15ca/amdgpu/amdgpu_bo.c) — BO allocation, CPU map, VA operations, and BO lists.
* [`amdgpu/amdgpu_vamgr.c`](https://gitlab.freedesktop.org/mesa/drm/-/blob/f9816a42b0d6138851cbe5eb7a33ee1a2f2a15ca/amdgpu/amdgpu_vamgr.c) — VA range allocation/free.
* [`amdgpu/amdgpu_cs.c`](https://gitlab.freedesktop.org/mesa/drm/-/blob/f9816a42b0d6138851cbe5eb7a33ee1a2f2a15ca/amdgpu/amdgpu_cs.c) — command submission and fence query lifecycle.
* [`amdgpu/amdgpu.h`](https://gitlab.freedesktop.org/mesa/drm/-/blob/f9816a42b0d6138851cbe5eb7a33ee1a2f2a15ca/amdgpu/amdgpu.h) — public API contracts and structures.

The register values reproduce the verified local RADV-derived experiment; the
generated clean capture is not stored in the repository. Upstream references
explain encodings and lifecycle but do not prove that every captured state bit
is minimal. This document groups writes by function for learning; exact global
write order remains the order in `captured_draw_state()`. For bit-identical
reconstruction, transcribe that function or regenerate the section-3 dump rather
than concatenating the grouped tables in section order.

## Appendix A. Compact Renoir/GFX9 contrast

`build/amdgpu-raw-triangle` is the separate Renoir/GFX9 experiment. Build and
run it with:

```sh
AMDGPU_MCPU=gfx90c make build/gfx-vs.bin build/gfx-ps-store.bin \
  build/gfx-ps-store-only.bin build/amdgpu-raw-triangle
build/amdgpu-raw-triangle
build/amdgpu-raw-triangle --ps-store-only
build/amdgpu-raw-triangle --color-target
```

Its checkpoints are VS memory store `0x76543210`, isolated PS memory store
`0xfeed1234`, and an opt-in linear color-target experiment (the executable help
still labels that path experimental/not stable). The crucial viewport fix is
`PA_CL_VTE_CNTL=0x0000003f`. Its working linear CB path uses generation-specific
state and 64-KiB color alignment; an observed Renoir linear
`CB_COLOR0_ATTRIB=0xd0000000` (`COLOR_SW_MODE=0`) differs from the optimal-tiled
`0xdc680000` (`COLOR_SW_MODE=26`). An attempted Renoir GFX `DMA_DATA` readback
returned zeros while direct color-BO mapping worked; do not infer that the
GFX1201 SDMA path automatically fixes the separate GFX9 experiment. None of
those values supersedes the GFX1201 layout, and the GFX1201 NGG rings/PWS stream
must not be backported verbatim.
