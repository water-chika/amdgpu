# GFX12/RDNA4 ray tracing

This document describes the ray-tracing mechanisms publicly visible for AMD
GFX12/RDNA4. It is based on open Mesa and Linux sources rather than private
hardware documentation. For a standalone Vulkan workload that verifies these
mechanisms on RX 9070 XT/GFX1201, see the
[Phase 1/2 RADV ray-query sample](gfx12-ray-query-sample.md).

The central conclusion is:

> GFX12 ray tracing is shader-driven traversal assisted by fixed-function BVH
> intersection, compressed-node fetch/decode, instance transformation, and LDS
> traversal-stack instructions.

Public sources do not expose a complete standalone block formally named an “RT
core.” Instead, ray tracing appears as cooperation between SQ, TA/TD, TCP,
LDS/DS, ordinary shader execution, and compute-based acceleration-structure
build shaders.

## Evidence versus inference

This document uses two evidence levels:

- **Confirmed**: directly represented by public opcodes, compiler lowering,
  acceleration-structure layouts, driver code, or generated register names.
- **Architectural inference**: a likely interpretation of those names and data
  flows, but not a published physical block diagram.

Do not use this document to infer unsupported claims such as RT units per CU,
box tests per cycle, triangle throughput, or exact cache topology.

## 1. High-level architecture

A useful public-source model is:

```text
SPIR-V ray query or ray-tracing shader
               │
               ▼
RADV NIR lowering builds the traversal loop
               │
               ▼
ACO selects GFX12 instructions
               │
               ├── image_bvh8_intersect_ray
               ├── ds_bvh_stack_push8_pop1_rtn_b32
               ├── ds_bvh_stack_push8_pop2_rtn_b64
               └── s_wait_bvhcnt
               │
               ▼
SQ issues shader and BVH operations
               │
               ├── TA/TD intersection-side machinery
               │      box/triangle tests and result generation
               │
               ├── TCP BVH fetch/compression handling
               │
               └── LDS/DS traversal-stack operations
```

The exact physical request path is not public. A route such as
`SQ → TA/TD → TCP → lower caches` is a reasonable interpretation of register
and performance-counter names, not a confirmed block diagram.

## 2. Division between hardware and shader code

GFX12 does not publicly appear to execute an entire Vulkan ray-tracing pipeline
inside an autonomous traversal processor.

### Fixed-function assistance

Public evidence supports hardware assistance for:

- fetching BVH nodes through a specialized MIMG instruction,
- decoding GFX12 compressed BVH nodes,
- testing rays against up to eight child boxes,
- testing rays against triangles,
- returning candidate child/hit information,
- child ordering or sorting behavior,
- instance-node traversal assistance,
- transforming ray origin and direction when entering an instance,
- traversal-stack push/pop operations in LDS,
- independent tracking of outstanding BVH operations.

### Shader responsibility

Compiler-generated shader code still owns:

- the outer traversal loop,
- interpretation of returned node types,
- selecting the next node,
- traversal termination,
- ray flags and culling policy,
- closest-hit and any-hit decisions,
- procedural/AABB intersection callbacks,
- payload handling,
- ray-query state,
- ray-tracing pipeline control flow.

A concise mental model is:

```text
shader = traversal policy and RT control flow
hardware BVH instructions = wide node/triangle intersection primitives
DS instructions = traversal-stack assistance
```

## 3. Hardware-facing blocks

## 3.1 SQ: issue and dependency tracking

The shader sequencer issues GFX12 BVH instructions as shader ISA operations.
Mesa ACO exposes:

```text
image_bvh_dual_intersect_ray
image_bvh8_intersect_ray
```

Linux generated register definitions expose:

```text
SQ_WAVE_IB_STS.BVH_CNT
```

This establishes that BVH operations are associated with shader waves and can
remain outstanding asynchronously.

### Dedicated BVH wait counter

GFX12 adds:

```asm
s_wait_bvhcnt N
```

Mesa distinguishes this from earlier generations:

```text
GFX12:   s_wait_bvhcnt
GFX6-11: vmcnt accounts for BVH operations
```

Conceptually:

```asm
image_bvh8_intersect_ray ...
s_wait_bvhcnt 0
; returned BVH result VGPRs are now ready
```

This reduces interference between ordinary vector-memory dependencies and BVH
intersection dependencies.

## 3.2 TA/TD: intersection-side machinery

Public names associate ray-intersection work with TA/TD. Examples include:

```text
TD_CNTL.FORCE_RT_BVH4_ARBITER_TO_PING_PONG
TA_PERF_SEL_bvh_total_cycles
TA_PERF_SEL_ray_tracing_bvh4_tri_node
TA_PERF_SEL_ray_tracing_bvh4_fp16_box_node
TA_PERF_SEL_ray_tracing_bvh4_fp32_box_node
```

Some named performance events are from older public SoC headers and therefore do
not prove an identical GFX12 microarchitecture. They do establish the public
historical association of the texture address/data path with BVH box and
triangle work.

It is reasonable—but still architectural inference—to describe TA/TD as
containing intersection and result-ordering logic used by the BVH MIMG
instruction.

## 3.3 TCP: BVH fetch and compressed-format handling

GFX12 generated registers expose BVH-specific fields in
`TCP_COMPRESSION_CNTL`:

```text
BVH_BYPASS_COMPRESSION
BVH_SPECULATIVE_DATA_READ
BVH_MAX_COMP_BLOCK_SIZE
BVH_MAX_UNCOMP_BLOCK_SIZE
```

This is direct evidence that the TCP path understands BVH-specific compressed
and speculative accesses.

Public source does not fully define:

- where decompression physically occurs,
- which cache levels allocate BVH requests,
- exact GL1/GL2 behavior,
- default firmware programming of these controls,
- whether every node type follows an identical cache route.

## 3.4 LDS/DS: traversal-stack operations

GFX12 has dedicated eight-wide DS operations:

```text
ds_bvh_stack_push8_pop1_rtn_b32
ds_bvh_stack_push8_pop2_rtn_b64
```

It also supports the earlier four-wide operation:

```text
ds_bvh_stack_push4_pop1_rtn_b32
```

These instructions operate through the LDS/DS path. Mesa lowers a NIR
`bvh_stack_rtn_amd` intrinsic to the appropriate instruction.

The stack protocol includes special values for:

```text
skip children 0–3
skip children 4–7
skip children 0–7
```

and stack-pointer state such as:

```text
HAS_BLAS
OVERFLOW
TLAS_POP
```

The dedicated stack operations avoid implementing all traversal stack updates
as ordinary sequences of LDS atomics and address arithmetic.

## 4. `image_bvh8_intersect_ray`

GFX12 traversal in RADV uses the NIR intrinsic:

```text
bvh8_intersect_ray_amd
```

ACO lowers it to:

```asm
image_bvh8_intersect_ray
```

Inputs include:

- a BVH resource descriptor,
- BVH base address,
- current node ID,
- ray origin,
- ray direction,
- maximum ray distance `tmax`,
- cull mask.

The GFX12 physical instruction result is represented by 16 logical components:

```text
10 general node/intersection result VGPRs
3 transformed-origin VGPRs
3 transformed-direction VGPRs
```

The exact meaning of the first ten components depends on the node type. The raw
GFX1201 probe records all ten but semantically validates only box slots 0-7 for
its captured one-child source node; components 8-9 are not yet asserted.

## 4.1 Box-node result

For a BVH8 internal box node, the instruction can return up to eight child node
IDs. Shader code then:

1. determines which children intersected,
2. chooses the next node,
3. pushes remaining candidates using a BVH stack instruction,
4. continues the traversal loop.

The fixed-function instruction accelerates the wide box tests and result
production. Shader code still controls the traversal policy.

In the source-defined reproduction of one captured RADV BLAS root, the raw probe
observed child ID zero in slot 0 and zero in the other seven slots, identically
across 50/50 runs. Those words describe that captured one-child result. They are
not evidence that a zero child ID generally means null or no child.

## 4.2 Primitive-node result

For primitive nodes, returned data can encode two triangle candidates, including
information such as:

- intersection distance `t`,
- barycentric coordinates `u` and `v`,
- primitive indices,
- geometry indices,
- front/back-face state,
- opaque/procedural flags,
- navigation information.

RADV interprets these fields and applies Vulkan ray flags, hit acceptance, and
shader invocation rules.

## 4.3 Instance-node result

For instance nodes, the GFX12 result can include:

- BLAS address,
- instance/user data,
- child node IDs,
- transformed ray origin,
- transformed ray direction.

This reduces shader work when changing coordinate systems between TLAS and BLAS
traversal.

On the older RADV BVH4 path, shader code loads a world-to-object matrix and
performs the ray transformation explicitly. GFX12 consumes the transformed
origin and direction returned by the BVH8 instruction.

## 5. BVH resource descriptor

The intersection operation is an MIMG instruction using a four-dword resource
descriptor. RADV builds a descriptor that describes the BVH address space and
intersection behavior.

Public Mesa code configures properties including:

- a large address range for node addressing,
- box sorting,
- triangle IJ return mode,
- pointer-flag support,
- compressed-format enable for BVH8,
- wide-sort enable,
- instance enable.

The descriptor is therefore part of the contract between:

```text
BVH memory encoding
↕
image_bvh8_intersect_ray interpretation
↕
compiler-generated traversal code
```

Changing the node format without changing the descriptor and traversal ABI is
not valid.

## 6. GFX12 BVH8 node format

Mesa explicitly documents that GFX12 introduces a new BVH encoding for:

```text
image_bvh_dual_intersect_ray
image_bvh8_intersect_ray
```

The principal hardware-facing node size is 128 bytes.

## 6.1 BVH8 box node

A `radv_gfx12_box_node` contains:

- internal-child base ID,
- primitive-child base ID,
- parent ID,
- three-component common origin,
- shared X/Y/Z exponents,
- child count,
- optional oriented-box matrix index,
- eight compressed child records.

Each child record is 96 bits and includes:

- quantized minimum X/Y/Z,
- quantized maximum X/Y/Z,
- cull flags,
- eight-bit cull mask,
- node type,
- node-size increment in 128-byte units.

Bounds are represented relative to a common origin and common power-of-two
extent. The encoder floors minima and conservatively rounds maxima so
quantization does not shrink the represented box.

Compared with four complete FP32 boxes, this improves branching factor and
reduces node bandwidth.

## 6.2 Primitive node

A GFX12 primitive node is 128 bytes and can encode multiple triangle pairs.
Compression can share:

- common prefixes of vertex components,
- component trailing-zero counts,
- base geometry index,
- base primitive index,
- deduplicated vertex references.

A node can contain up to eight triangle-pair descriptors when the compressed
representation fits.

The format also carries metadata such as:

- opaque state,
- double-sided state,
- procedural state,
- geometry and primitive deltas,
- vertex references.

## 6.3 Instance node

The GFX12 instance representation has two 128-byte portions.

The first is hardware-facing and includes:

- world-to-object transform,
- BLAS address and pointer flags,
- parent ID,
- cull mask and user data,
- common origin/exponents,
- quantized child records.

The second is driver data and includes:

- object-to-world transform,
- application custom instance/index values,
- BVH and leaf-offset metadata.

## 7. Traversal algorithm in RADV

The GFX12 traversal builder is:

```text
radv_build_ray_traversal_gfx12
```

Its logical loop is approximately:

```text
node = root

while node is valid:
    result = image_bvh8_intersect_ray(descriptor, base, ray, node)
    wait for BVH result

    switch node type:
      box:
        choose first intersected child
        push remaining child candidates
        node = chosen child

      instance:
        save TLAS continuation
        update BVH base
        replace ray with returned object-space origin/direction
        node = returned BLAS child

      triangle:
        evaluate returned hit candidates
        apply flags and hit policy
        invoke/report hit as required
        node = pop traversal stack

      procedural:
        run intersection callback logic
        node = pop traversal stack
```

This illustrates the split: the instruction handles intersection-heavy work;
shader code owns traversal and Vulkan semantics.

## 8. Compiler flow

## 8.1 Ray queries

For ray queries, RADV:

1. receives SPIR-V ray-query operations,
2. optimizes the query state,
3. lowers query operations to a NIR traversal loop,
4. selects the GFX12 traversal path when BVH8 is enabled,
5. emits NIR BVH8 and stack intrinsics,
6. lets ACO select GFX12 machine instructions.

## 8.2 Ray-tracing pipelines

For full Vulkan ray-tracing pipelines, RADV performs:

- RT control-flow lowering,
- payload and shader-record ABI lowering,
- traversal-shader generation,
- hit/miss/callable stage integration,
- NIR optimization,
- ACO instruction selection.

RADV supports multiple compiler organization strategies, including monolithic,
continuation-passing, and function-call-oriented lowering modes. These are
software organization strategies layered above the same BVH hardware
instructions.

## 8.3 NIR to ACO mapping

Key mappings are:

```text
nir_bvh8_intersect_ray_amd
    -> image_bvh8_intersect_ray

nir_bvh_stack_rtn_amd
    -> ds_bvh_stack_push8_pop1_rtn_b32
       or another matching DS stack operation

BVH result dependency
    -> s_wait_bvhcnt
```

This mapping is experimentally verified, not only inferred from source. On
Mesa 26.2.6-arch1.1 and RX 9070 XT/GFX1201, the standalone ray-query sample's
application compute shader contained one static `image_bvh8_intersect_ray`,
three `s_wait_bvhcnt 0` sites, and one
`ds_bvh_stack_push8_pop1_rtn_b32` site. It returned the exact expected hit,
distance, barycentric, instance, primitive, and geometry records for two hits
and one miss.

A separate opt-in raw probe now verifies a smaller layer below RADV traversal:
`make raw-ray-query-gfx1201` builds source-defined host and assembly files, and
its `info`, `shader`, and `box` stages culminate in one directly submitted
`image_bvh8_intersect_ray` against a reproduction of the captured one-child BLAS
root. The exact output repeated 50/50 times. That result validates this box-node
instruction/descriptor/ISA ABI only; it does not validate primitive decoding,
traversal, or a full ray query. See the
[sample and raw-probe record](gfx12-ray-query-sample.md) for source provenance,
commands, exact words, dispatch state, and evidence limitations.

## 9. Acceleration-structure build path

The public RADV path builds acceleration structures with compute shaders. No
separately exposed fixed-function BVH-builder packet or hardware unit is used by
this implementation.

The high-level build is:

```text
application geometry
       │
       ▼
common intermediate BVH construction
       │
       ▼
GFX12 compute encoders
       ├── collapse nodes into up to eight children
       ├── quantize child bounds
       ├── compress triangle batches
       ├── write 128-byte hardware nodes
       └── initialize headers and metadata
```

## 9.1 Internal-node encoding

`encode_gfx12.comp`:

- processes clusters of eight invocations,
- collapses intermediate binary structure into up to eight hardware children,
- computes common origins and exponents,
- quantizes child bounds,
- writes child types, cull masks, base IDs, and parent information,
- optionally pairs sibling triangles.

## 9.2 Triangle compression

`encode_triangles_gfx12.comp`:

- processes up to 16 source triangles per cluster,
- pairs triangles,
- deduplicates vertices,
- finds shared prefixes and trailing zeros,
- chooses groups according to compressed size,
- writes up to eight pair descriptors per primitive node,
- retries inefficient batches that would otherwise produce poor packing.

## 9.3 Update and refit

`update_gfx12.comp`:

- rebuilds modified triangle/AABB leaves,
- walks internal nodes bottom-up,
- coordinates with ready counters and device-scope atomics,
- recomputes total bounds,
- requantizes all eight children,
- supports in-place and source-to-destination updates.

## 10. Differences from GFX10.3 and GFX11

| Area | GFX10.3 | GFX11 | GFX12/RDNA4 |
| --- | --- | --- | --- |
| Traversal width | BVH4 | BVH4 | BVH8 |
| Main intersection ISA | `image_bvh64_intersect_ray` | BVH64 form with newer opcode/layout | `image_bvh8_intersect_ray` |
| Returned box children | Up to four | Up to four | Up to eight |
| Box format | FP16/FP32 BVH4 boxes | Similar BVH4 family | Quantized 128-byte BVH8 nodes |
| Primitive format | Mostly fixed triangle leaves | Similar older format | Compressed multi-triangle primitive nodes |
| Stack | Software-oriented traversal stack | Dedicated four-wide DS stack support | Eight-wide DS stack operations |
| Instance ray transform | Performed substantially in shader code | BVH4/pointer assistance | Returned transformed origin/direction |
| BVH waits | `vmcnt` | `vmcnt` | Dedicated `bvhcnt` and `s_wait_bvhcnt` |
| Build encoding | BVH4 | BVH4 | BVH8 collapse, quantization, triangle compression |
| Update | BVH4 refit | Similar | Eight-wide requantization and compressed-leaf handling |
| Cache-visible controls | No equivalent found in inspected GC10 headers | Existing RT-associated TA/TD names | Explicit TCP BVH compression/speculative-read controls |

## 11. `image_bvh_dual_intersect_ray`

GFX12 publicly defines:

```text
image_bvh_dual_intersect_ray
```

but the inspected RADV traversal selects `image_bvh8_intersect_ray`. No active
Mesa use was found for the dual instruction in this revision.

Therefore public source confirms the opcode exists but does not establish:

- its intended production use,
- its input/output ABI,
- its performance advantage,
- whether another driver/compiler path uses it.

Avoid treating it as the normal GFX12 traversal instruction without additional
evidence.

## 12. Portability and implementation cautions

### BVH format and shader ISA are coupled

A GFX12 BVH8 cannot simply be traversed with the older BVH4 instruction. The
following must match:

```text
node layout
resource descriptor
intersection instruction
result decoding
traversal stack protocol
compiler-generated control flow
```

### Building is not traversal

Acceleration-structure build/refit runs as compute work. Traversal happens when
ray/query shaders execute BVH instructions. Synchronization between build and
traversal must make written AS memory visible to later shader reads.

### Stack operations are generation-sensitive

GFX11 and GFX12 differ in stack address encoding, traversal width, pointer flags,
and available DS operations. Do not transplant raw stack instruction sequences
between generations.

### Public names do not prove physical topology

Register prefixes such as SQ, TA, TD, TCP, and LDS identify participating
interfaces and controls. They do not prove precise floorplan placement or a
specific count of “RT cores.”

## 13. What remains unknown publicly

The inspected open sources do not establish:

- ray accelerators per CU or WGP,
- ray-box or ray-triangle tests per cycle,
- instruction latency,
- exact TA/TD internal pipeline,
- exact SQ-to-TCP request routing,
- GL1/GL2 allocation and bypass policy for every BVH access,
- hidden firmware configuration,
- detailed power or clock partitioning,
- production use of `image_bvh_dual_intersect_ray`.

These require hardware documentation, measured microbenchmarks, or additional
public disclosures.

## 14. Source map

The references below are pinned to Mesa revision
`eda37f052369d3f1579e932abe940fc9b4026855`.

### Instruction and compiler support

- [`src/amd/compiler/aco_opcodes.py`](https://gitlab.freedesktop.org/mesa/mesa/-/blob/eda37f052369d3f1579e932abe940fc9b4026855/src/amd/compiler/aco_opcodes.py) — BVH intersection, DS stack, and `s_wait_bvhcnt` opcodes.
- [`src/amd/compiler/instruction_selection/aco_select_nir_intrinsics.cpp`](https://gitlab.freedesktop.org/mesa/mesa/-/blob/eda37f052369d3f1579e932abe940fc9b4026855/src/amd/compiler/instruction_selection/aco_select_nir_intrinsics.cpp) — NIR-to-GFX12 BVH and stack instruction lowering.
- [`src/compiler/nir/nir_intrinsics.py`](https://gitlab.freedesktop.org/mesa/mesa/-/blob/eda37f052369d3f1579e932abe940fc9b4026855/src/compiler/nir/nir_intrinsics.py) — AMD BVH NIR intrinsic definitions.

### Traversal and RT pipelines

- [`src/amd/vulkan/nir/radv_nir_rt_common.c`](https://gitlab.freedesktop.org/mesa/mesa/-/blob/eda37f052369d3f1579e932abe940fc9b4026855/src/amd/vulkan/nir/radv_nir_rt_common.c) — GFX12 BVH8 traversal builder and descriptor setup.
- [`src/amd/vulkan/nir/radv_nir_lower_ray_queries.c`](https://gitlab.freedesktop.org/mesa/mesa/-/blob/eda37f052369d3f1579e932abe940fc9b4026855/src/amd/vulkan/nir/radv_nir_lower_ray_queries.c) — ray-query lowering and BVH8 selection.
- [`src/amd/vulkan/nir/radv_nir_rt_traversal_shader.c`](https://gitlab.freedesktop.org/mesa/mesa/-/blob/eda37f052369d3f1579e932abe940fc9b4026855/src/amd/vulkan/nir/radv_nir_rt_traversal_shader.c) — traversal-shader generation.
- [`src/amd/vulkan/radv_pipeline_rt.c`](https://gitlab.freedesktop.org/mesa/mesa/-/blob/eda37f052369d3f1579e932abe940fc9b4026855/src/amd/vulkan/radv_pipeline_rt.c) — Vulkan RT pipeline lowering and compilation.

### BVH format, build, and update

- [`src/amd/vulkan/bvh/README.md`](https://gitlab.freedesktop.org/mesa/mesa/-/blob/eda37f052369d3f1579e932abe940fc9b4026855/src/amd/vulkan/bvh/README.md) — GFX12 node formats and instruction return layouts.
- [`src/amd/vulkan/bvh/bvh.h`](https://gitlab.freedesktop.org/mesa/mesa/-/blob/eda37f052369d3f1579e932abe940fc9b4026855/src/amd/vulkan/bvh/bvh.h) — canonical public node structures and stack constants.
- [`src/amd/vulkan/bvh/encode_gfx12.comp`](https://gitlab.freedesktop.org/mesa/mesa/-/blob/eda37f052369d3f1579e932abe940fc9b4026855/src/amd/vulkan/bvh/encode_gfx12.comp) — BVH8 internal-node encoder.
- [`src/amd/vulkan/bvh/encode_triangles_gfx12.comp`](https://gitlab.freedesktop.org/mesa/mesa/-/blob/eda37f052369d3f1579e932abe940fc9b4026855/src/amd/vulkan/bvh/encode_triangles_gfx12.comp) — GFX12 triangle compression.
- [`src/amd/vulkan/bvh/update_gfx12.comp`](https://gitlab.freedesktop.org/mesa/mesa/-/blob/eda37f052369d3f1579e932abe940fc9b4026855/src/amd/vulkan/bvh/update_gfx12.comp) — GFX12 update/refit.
- [`src/amd/vulkan/radv_acceleration_structure.c`](https://gitlab.freedesktop.org/mesa/mesa/-/blob/eda37f052369d3f1579e932abe940fc9b4026855/src/amd/vulkan/radv_acceleration_structure.c) — build layout, pipeline sequencing, scratch, and update dispatch.

### Linux-generated hardware names

Linux evidence is pinned to revision
`181301eec0cf51ba90bb8773b108a4fcc19b312c`:

- [`gc_12_0_0_sh_mask.h`](https://github.com/torvalds/linux/blob/181301eec0cf51ba90bb8773b108a4fcc19b312c/drivers/gpu/drm/amd/include/asic_reg/gc/gc_12_0_0_sh_mask.h) — SQ BVH count, TD RT arbiter, TCP BVH compression controls, and LDS BVH queue controls.
- [`gc_12_1_0_sh_mask.h`](https://github.com/torvalds/linux/blob/181301eec0cf51ba90bb8773b108a4fcc19b312c/drivers/gpu/drm/amd/include/asic_reg/gc/gc_12_1_0_sh_mask.h) — corresponding GC12.1 fields.

## 15. Concise mental model

```text
GFX12 traversal remains shader-controlled.

image_bvh8_intersect_ray:
  fetches/decodes compressed BVH8 nodes,
  performs wide box or triangle tests,
  returns children/hit/instance results.

DS BVH stack instructions:
  maintain eight-wide traversal candidates in LDS.

s_wait_bvhcnt:
  waits for asynchronous BVH instruction results.

RADV compute shaders:
  build, compress, update, and refit the BVH8 data structure.
```

That is the publicly supportable view of GFX12 ray tracing: **wider BVH8
fixed-function intersection and compressed-node assistance integrated into the
shader, texture/cache, and LDS paths, while traversal policy and Vulkan RT
semantics remain compiler-generated shader code.**
