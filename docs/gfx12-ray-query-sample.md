# GFX12 RADV ray-query sample

This note records the completed Phase 1/2 experiment behind
`examples/radv_ray_query.c` and `examples/radv_ray_query.comp`. It is a normal
Vulkan/RADV application, not a raw-PM4 ray-tracing probe. The verified target
was an RX 9070 XT (`1002:7550`, RADV GFX1201) with Mesa 26.2.6-arch1.1.

The captures are intentionally external to this source-only repository. The
local evidence paths quoted below are reproducibility references, not repository
inputs:

```text
/home/water/experiments/gfx12-ray-query/logs/shaders.log
/home/water/experiments/gfx12-ray-query/logs/dumpibs.log
/home/water/experiments/gfx12-ray-query/logs/metashaders.log
```

## 1. Standalone architecture

The sample has two source files:

- `radv_ray_query.c` creates the Vulkan objects, builds one BLAS and one TLAS,
  dispatches the query shader, reads back 96 bytes, and checks every word.
- `radv_ray_query.comp` launches one invocation per ray. Each invocation creates
  an inline `rayQueryEXT`, proceeds it to completion, and writes an eight-word
  record.

The executable deliberately selects AMD PCI device `1002:7550` and requires
`driverName == "radv"`. This keeps a successful run tied to the GFX1201 path
whose ISA and PM4 were captured, rather than silently running on another Vulkan
device.

The Vulkan object flow is:

```text
host-visible triangle vertices
        -> BLAS build (one opaque triangle)
        -> TLAS build (one identity instance)
        -> compute pipeline with TLAS + result SSBO descriptors
        -> vkCmdDispatch(3, 1, 1)
        -> host-visible result comparison
```

BLAS, TLAS, and ray-query command buffers are submitted separately and each is
completed with a fence before the next phase. This is intentionally simple and
makes the phases visible in a RADV IB dump.

## 2. Build and run

Build only the sample and its SPIR-V module:

```sh
make radv-ray-query
```

The shader rule uses `glslangValidator` when available and otherwise `glslc`,
targeting Vulkan 1.2. Run with:

```sh
build/radv-ray-query
```

An alternate SPIR-V file may be supplied as the sole argument:

```sh
build/radv-ray-query /path/to/query.comp.spv
```

The verified run identified:

```text
device: AMD Radeon RX 9070 XT (RADV GFX1201) (1002:7550)
driver: radv - Mesa 26.2.6-arch1.1
api: 1.4.354
```

## 3. Exact scene, rays, and results

The BLAS contains one non-indexed, opaque triangle at `z = 0`:

```text
v0 = (-1, -1, 0)
v1 = ( 1, -1, 0)
v2 = ( 0,  1, 0)
```

The TLAS contains one identity-transformed instance with mask `0xff`, custom
index 7, and triangle-facing culling disabled. All rays use opaque flags, cull
mask `0xff`, `tmin = 0`, direction `(0, 0, -1)`, and `tmax = 10`.

| Row | Origin | Expected result |
| --- | --- | --- |
| 0 | `(0, 0, 1)` | hit at `t=1`, barycentrics `(0.25, 0.5)` |
| 1 | `(-0.5, -0.5, 2)` | hit at `t=2`, barycentrics `(0.125, 0.25)` |
| 2 | `(2, 2, 1)` | miss |

Each output record is `{hit, t, bary.x, bary.y, custom_index,
primitive_index, geometry_index, instance_id}`. Floating-point values are
stored as raw IEEE-754 words. The exact verified output was:

```text
row 0: 00000001 3f800000 3e800000 3f000000 00000007 00000000 00000000 00000000
row 1: 00000001 40000000 3e000000 3e800000 00000007 00000000 00000000 00000000
row 2: 00000000 ffffffff ffffffff ffffffff ffffffff ffffffff ffffffff ffffffff
PASS
```

A miss writes `0xffffffff` to all seven payload words, making an incomplete or
partially executed dispatch distinguishable from valid zero-valued hit fields.
The buffer is initially poisoned with `0xcdcdcdcd` as an additional guard.

## 4. Vulkan contract and buffers

The device enables these extensions:

```text
VK_KHR_acceleration_structure
VK_KHR_ray_query
VK_KHR_deferred_host_operations
```

and explicitly queries and enables:

```text
VkPhysicalDeviceBufferDeviceAddressFeatures::bufferDeviceAddress
VkPhysicalDeviceAccelerationStructureFeaturesKHR::accelerationStructure
VkPhysicalDeviceRayQueryFeaturesKHR::rayQuery
```

The relevant buffers are:

| Buffer | Memory | Principal usage |
| --- | --- | --- |
| vertices | host-visible, coherent | AS build input + shader device address |
| BLAS storage | device-local | AS storage + shader device address |
| BLAS scratch | device-local, scratch-aligned | storage + shader device address |
| instance | host-visible, coherent | AS build input + shader device address |
| TLAS storage | device-local | AS storage + shader device address |
| TLAS scratch | device-local, scratch-aligned | storage + shader device address |
| result (96 bytes) | host-visible, coherent | storage buffer |

Scratch device addresses are aligned to
`minAccelerationStructureScratchOffsetAlignment`. Device-address allocations
use `VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT`.

The compute descriptor set has only two bindings: binding 0 is the TLAS as
`VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR`; binding 1 is the result SSBO.

## 5. BLAS, TLAS, query submissions, and barriers

The BLAS is built first with one `VK_GEOMETRY_TYPE_TRIANGLES_KHR` geometry,
`VK_GEOMETRY_OPAQUE_BIT_KHR`, and
`VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR`. Its device address
is then placed in a single `VkAccelerationStructureInstanceKHR` used to build
the TLAS.

The TLAS command buffer starts with this dependency before its build:

```text
source stage/access: VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR
                     VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR
destination:         VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR
                     VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR
```

The query command buffer similarly places an AS-build-to-compute dependency
before binding and dispatching:

```text
source stage/access: VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR
                     VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR
destination:         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT
                     VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR
```

The fences between the three submissions provide deliberately conservative
execution completion. The explicit barriers document and exercise the intended
AS memory dependencies rather than relying only on queue idleness.

## 6. RADV capture commands

Keep generated captures outside the checkout:

```sh
mkdir -p /home/water/experiments/gfx12-ray-query/logs

RADV_DEBUG=shaders build/radv-ray-query \
  > /home/water/experiments/gfx12-ray-query/logs/shaders.log 2>&1

RADV_DEBUG=dumpibs build/radv-ray-query \
  > /home/water/experiments/gfx12-ray-query/logs/dumpibs.log 2>&1

RADV_DEBUG=metashaders build/radv-ray-query \
  > /home/water/experiments/gfx12-ray-query/logs/metashaders.log 2>&1
```

If a non-system RADV is under test, select it with the Vulkan loader environment
(for example `VK_DRIVER_FILES=/path/to/radeon_icd.x86_64.json`) while retaining
the same redirects. Captures can contain process-specific virtual addresses;
they are observations, not replayable source artifacts.

## 7. Observed application ISA

`RADV_DEBUG=shaders` exposed the lowered application compute shader and directly
verified the public-source architecture described in
[`gfx12-ray-tracing.md`](gfx12-ray-tracing.md). Static occurrences in this one
compiled application shader were:

| Instruction | Count | Observed encoding words |
| --- | ---: | --- |
| `image_bvh8_intersect_ray` | 1 | `d3e04010 0e00181c 0300181a` |
| `s_wait_bvhcnt 0` | 3 | `bfc30000` |
| `ds_bvh_stack_push8_pop1_rtn_b32 ... offset:16` | 1 | `db840010 0f1c0f07` |

The intersection destination was `v[28:37]`, with transformed origin/direction
returned separately by ACO before register allocation. The three wait sites are
control-flow placements in one static shader; they are not a claim that every
ray dynamically executes exactly three waits. Likewise, the single static MIMG
instruction is revisited by the compiler-generated traversal loop as needed.

The shader dump also reports `workgroup_size: 1,1,1`, 2048 bytes of shared
storage, API subgroup size 64, and min/max compiled subgroup size 32. The LDS is
used by the compiler-generated ray-query traversal stack; it is not declared by
the GLSL source.

## 8. Observed application compute state

The final application dispatch is identifiable in the IB capture by its
one-thread workgroup and three X workgroups. The observed state was:

```text
COMPUTE_PGM_LO          = 0x000003b4
COMPUTE_PGM_RSRC1       = 0x000c0005
COMPUTE_PGM_RSRC2       = 0x00020082
COMPUTE_PGM_RSRC3       = 0x000000f0
COMPUTE_RESOURCE_LIMITS = 0x01000000
COMPUTE_NUM_THREAD_X/Y/Z = 1 / 1 / 1
DISPATCH_DIRECT DIM_X/Y/Z = 3 / 1 / 1
COMPUTE_DISPATCH_INITIATOR = 0x0000a045
```

RADV's decoder displayed `USER_SGPR=1`, `LDS_SIZE=4`, `CU_GROUP_COUNT=1`,
`CS_W32_EN=1`, `COMPUTE_SHADER_EN=1`, and `FORCE_START_AT_000=1`. The
`COMPUTE_PGM_LO` value is capture-address-dependent. RSRC fields are encoded
register values (for example, the decoder's `VGPRS=5` is an allocation field),
not literal source-level register counts.

The earlier dispatches in the same dump are RADV acceleration-structure build
and encode work. Their numerous program addresses, resource settings, and
dimensions demonstrate why the final `3,1,1` plus `1,1,1` signature is useful
for identifying the application query dispatch.

## 9. Attempted metashader capture

The `RADV_DEBUG=metashaders` attempt produced only the normal warning, device
identification, exact three result rows, and `PASS`. It emitted no metashader
IR or ISA. Therefore this experiment establishes **absence in the attempted
flag's output**, not that RADV uses no internal AS build shaders. `dumpibs`
clearly contains many pre-query compute dispatches, and public Mesa source says
AS construction is compute-shader based; the attempted flag simply did not
expose them in this Mesa build/path.

## 10. Raw GFX1201 box-instruction probe

Phase 1 remains the deterministic Vulkan ray query above, and Phase 2 remains
the RADV ISA/PM4 capture. Phase 3 is now complete only in the narrow sense of a
raw **box-instruction probe**: it directly submits one
`image_bvh8_intersect_ray` through `libdrm_amdgpu`, but does not implement a ray
query, traversal loop, or acceleration-structure build.

The target is opt-in so the default build remains portable:

```sh
make raw-ray-query-gfx1201
build/amdgpu-raw-ray-query-gfx1201 --stage info
build/amdgpu-raw-ray-query-gfx1201 --stage shader
build/amdgpu-raw-ray-query-gfx1201 --stage box
```

LLVM 18 or newer with the `gfx1201` assembler target is required. The stages are
incremental safety checks:

1. `info` opens the selected DRM node, prints GPU information, and rejects any
   ASIC other than Navi48 PCI device `0x7550` before allocating/submitting work.
2. `shader` dispatches the same raw shader with a zero BVH address. That branch
   writes `0x3a5a3a5a` and completion magic `0xc0decafe`, then the program checks
   SDMA readback and output canaries.
3. `box` uploads the source-defined 128-byte node, executes the BVH8 instruction,
   waits with `s_wait_bvhcnt 0`, reads back through SDMA, and validates the
   captured one-child result, completion magic, and canaries.

### Source and capture provenance

The raw probe is deliberately reconstructible from source rather than from a
checked-in binary or log:

- `src/amdgpu_raw_ray_query_gfx1201.c` owns the `libdrm_amdgpu` BO/VA setup,
  source-defined `gfx12_box_node`, compute PM4, staged checks, SDMA readback, and
  validation.
- `shaders/gfx12/raw-ray-query-box.s` owns the descriptor construction, ray and
  node-ID inputs, exact BVH8 instruction, dedicated wait, and result stores.
- `tools/extract-text.sh` assembles that shader for `gfx1201`; the generated
  `build/gfx12-raw-ray-query-box.bin` is build output, not source.

The 128-byte node values were transcribed from the one-child BLAS root produced
by the Vulkan sample's RADV build. The source expresses those values explicitly:
base IDs `0`, parent `0xffffffff`, origin `(-1,-1,0)`, packed child-count and
exponents `0x70008080`, OBB index `0x7f`, and eight 96-bit child records
`{0x00000000, 0xfffff000, 0x15ffffff}`. This is a source-defined reproduction of
that captured root, not a replay of its process-specific GPU address and not a
general-purpose GFX12 AS encoder. Capture logs and any repeated-run transcript
remain under `/home/water/experiments/gfx12-ray-query/`, outside this source-only
repository, under the same external-logs policy as the RADV captures above.

### Descriptor, shader, and ISA ABI

The compute user-data ABI is intentionally small:

```text
s[0:1] = output BO virtual address
s[2:3] = BVH BO virtual address (zero in the shader stage)
```

`COMPUTE_PGM_RSRC2.USER_SGPR` exposes four SGPRs. The verified raw compute state
uses `RSRC1=0x000c0003`, `RSRC2=0x00000008`, `RSRC3=0x000000f0`, one thread, and
one workgroup. The shader builds the four-dword BVH descriptor in `s[4:7]` as:

```text
s4 = low 32 bits of BVH VA
s5 = (high 16 bits of BVH VA) | 0x80100000
s6 = 0xffffffff
s7 = 0x81e803ff
```

The last word selects the captured Mesa GFX12 contract: 42-bit range, triangle
IJ result mode, pointer flags, compressed BVH8, wide sorting, instances, and a
valid descriptor. Inputs occupy `v[0:10]`: `tmin=0`, `tmax=10`, cull mask
`0xff`, origin `(0,0,0)`, direction `(1,1,1)`, and root box node ID `5`. Results
occupy `v[16:25]`; the source stores all ten general result components, followed
by the six origin/direction components used as instruction inputs. The exact
assembled instruction words are:

```text
d3e04010 0a000810 07040200
```

### Exact observed result and repeatability

The verified `box` run printed:

```text
stage info: DRM=3.64 asic=0x7550 family=152 chip_rev=1 external_rev=81 CU=64
stage shader: code=256 bytes rsrc1=000c0003 rsrc2=00000008 rsrc3=000000f0
result: 00000000 00000000 00000000 00000000 00000000 00000000 00000000 00000000 00000000 00000000 00000000 00000000 00000000 3f800000 3f800000 3f800000 c0decafe
returned[10..15]: 00000000 00000000 00000000 3f800000 3f800000 3f800000 (observational only)
PASS: captured one-child box result, completion magic, and canaries
```

This exact result and PASS repeated in 50 out of 50 runs. For this captured
one-child BLAS root, hardware returned child ID `0` in slot 0 and zero in the
other seven box-result slots; all eight had been initialized to `0xcdcdcdcd`, so
the instruction demonstrably overwrote them. This observation must not be
generalized into “zero child IDs are null”: zero is the captured node's actual
one-child result here. Components 8-9 are captured and printed as zero but are
not semantically asserted. Components 10-15 are the returned origin/direction
record and are observational only.

### Current limitations

This is only a controlled probe of the box form of one hardware instruction. It
does not yet provide:

- a primitive-node/triangle-instruction probe or semantic hit decoding;
- a traversal loop, LDS BVH stack operations, or node selection;
- TLAS/instance handling or coordinate-system transitions;
- a complete BLAS/TLAS build, portable AS import, or general node encoder;
- a Vulkan ray-query implementation or full ray-tracing pipeline.

The Vulkan sample remains the semantic reference for two triangle hits and one
miss. The raw probe establishes a narrower ABI and execution fact: the captured
one-child box node can be submitted safely to the GFX1201 BVH8 instruction and
produces the exact repeatable words above.
