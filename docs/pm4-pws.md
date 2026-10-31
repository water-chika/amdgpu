# GFX12 Pipeline Wait Sync (PWS)

Pipeline Wait Sync (PWS), often called **Pixel Wait Sync** in AMD material, is a
GFX11+ internal counter-based synchronization mechanism. It lets a GPU pipeline
event signal progress and lets later GPU work wait for that event without first
writing a normal fence value to memory and polling it.

At a high level:

```text
producer work
    ↓
RELEASE_MEM with PWS_ENABLE
    ↓ event completion advances an internal counter
ACQUIRE_MEM with PWS wait fields
    ↓ selected consumer stage waits for that counter
consumer work
```

PWS answers the question:

> Has a selected pipeline event completed far enough for a selected later stage
> to proceed?

PWS does not, by itself, answer every cache-coherency question. Pipeline
completion, cache visibility, frontend ordering, submission completion, and CPU
visibility are related but distinct responsibilities.

## 1. Why PWS exists

A traditional GPU synchronization sequence can use memory:

1. Insert an end-of-pipe event.
2. Write a fence value to a GPU memory address.
3. Poll that memory address from the command processor.
4. Continue after the expected value appears.

PWS provides an internal alternative:

1. Insert a pipeline event through `RELEASE_MEM`.
2. Ask the event to advance an internal PWS counter.
3. Wait on that counter through `ACQUIRE_MEM`.

No application fence address is required for that internal producer/consumer
relationship.

Common uses include:

- waiting for graphics idle before changing GFX12 GE rings,
- waiting for pixel-shader completion,
- waiting for compute-shader completion,
- ordering graphics-to-compute transitions,
- pairing pipeline completion with cache writeback or invalidation.

## 2. The producer: `RELEASE_MEM`

A PWS producer is a `PKT3_RELEASE_MEM` packet whose event word has
`PWS_ENABLE=1`.

Conceptually:

```text
RELEASE_MEM
  EVENT_TYPE  = BOTTOM_OF_PIPE_TS, PS_DONE, CS_DONE, or another supported event
  EVENT_INDEX = index appropriate to that event
  PWS_ENABLE  = 1
  GCR fields  = optional cache actions
  address/data payloads = 0 when no memory fence is written
```

The packet inserts an event into the pipeline. It does not synchronously stop at
packet parsing time. The counter is signaled only after the selected event
reaches its completion point.

Mesa's helper is:

```c
ac_emit_cp_release_mem_pws(cs, gfx_level, ip_type, event_type, gcr_cntl);
```

It chooses the event index, repacks relevant GCR fields into the GFX12
`RELEASE_MEM` encoding, sets `PWS_ENABLE`, and emits seven payload dwords.

### Example: bottom-of-pipe signal

Before changing GE ring registers, Mesa uses a bottom-of-pipe event:

```text
RELEASE_MEM
  EVENT_TYPE = BOTTOM_OF_PIPE_TS
  PWS_ENABLE = 1
```

Meaning:

> After all earlier graphics work reaches the bottom of the pipeline, advance
> the timestamp-event PWS counter.

## 3. The consumer: PWS `ACQUIRE_MEM`

A PWS consumer is the GFX11+ PWS form of `PKT3_ACQUIRE_MEM`.

Packet shape:

```text
header: ACQUIRE_MEM, count=6
payload 0: PWS_STAGE_SEL, PWS_COUNTER_SEL, PWS_ENA2, PWS_COUNT
payload 1: GCR_SIZE_LO = 0xffffffff
payload 2: GCR_SIZE_HI = 0x01ffffff
payload 3: GCR_BASE_LO = 0
payload 4: GCR_BASE_HI = 0
payload 5: PWS_ENA = 1, encoded as bit 31
payload 6: GCR_CNTL
```

Because PM4 type-3 count is payload count minus one, `count=6` means seven
payload dwords.

Mesa's helper is:

```c
ac_emit_cp_acquire_mem_pws(cs, gfx_level, ip_type,
                           event_type, stage_sel, count, gcr_cntl);
```

It derives the counter family from `event_type`, selects the consumer stage,
enables PWS in both required fields, and emits the wait.

## 4. PWS wait fields

### `PWS_COUNTER_SEL`

This selects the internal event-counter family. Mesa derives it from the event
used by the release:

| Release event | Counter selection |
| --- | --- |
| Timestamp/EOP event such as `BOTTOM_OF_PIPE_TS` | `TS_SELECT` |
| `PS_DONE` | `PS_SELECT` |
| `CS_DONE` | `CS_SELECT` |

The release and acquire must use matching event/counter semantics.

### `PWS_STAGE_SEL`

This selects the point that later work is prevented from passing until the wait
is satisfied.

Examples include command-processor stages such as:

- CP ME,
- CP PFP,

and graphics pipeline points such as pre-depth or pre-shader stages.

The selected stage matters. Waiting at ME does not automatically prove that PFP
has stopped running ahead of ME.

### `PWS_COUNT`

This is a six-bit counter field used by the hardware PWS protocol. Mesa normally
passes zero for a direct release/wait pair. Do not invent count values without a
same-generation hardware or driver reference.

### `PWS_ENA2` and `PWS_ENA`

The GFX12 PWS acquire uses two enable fields:

```text
payload 0 bit 17: PWS_ENA2
payload 5 bit 31: PWS_ENA
```

Both are set by Mesa's PWS helper. A packet with only the PWS-shaped size fields
but no matching enable/control fields is not a complete PWS wait.

## 5. Decoding `0x00022000`

The raw-triangle experiment uses this control word in one PWS-shaped acquire:

```text
0x00022000
```

Relevant GFX12 fields are:

```text
bits 13:11  PWS_STAGE_SEL
bits 15:14  PWS_COUNTER_SEL
bit  17     PWS_ENA2
bits 23:18  PWS_COUNT
```

Decode:

```text
PWS_STAGE_SEL   = CP_PFP
PWS_COUNTER_SEL = TS_SELECT
PWS_ENA2        = 1
PWS_COUNT       = 0
```

Payload 5 contains:

```text
0x80000000 -> PWS_ENA = 1
```

Together they request a timestamp-counter wait associated with the CP PFP side.

## 6. GE ring update example

Mesa requires graphics idle before updating the GFX12 attribute, position, and
primitive ring registers. Its production sequence is:

```text
earlier graphics work

RELEASE_MEM
  EVENT_TYPE = BOTTOM_OF_PIPE_TS
  PWS_ENABLE = 1

ACQUIRE_MEM, PWS form
  PWS_COUNTER_SEL = TS_SELECT
  PWS_STAGE_SEL   = CP_ME
  PWS_COUNT       = 0
  PWS_ENA2        = 1
  PWS_ENA         = 1

program SPI_ATTRIBUTE_RING_BASE/SIZE
program GE_POS_RING_BASE/SIZE
program GE_PRIM_RING_BASE/SIZE
```

The release establishes the producer event. The acquire waits for that event.
An acquire by itself cannot prove earlier graphics work reached bottom of pipe.

The raw GFX1201 tutorial describes ring sizing and programming in
[`raw-gfx-triangle.md`](raw-gfx-triangle.md).

## 7. Why `PFP_SYNC_ME` may be required

On GFX10+, `ACQUIRE_MEM` is executed through ME. If the intended consumer is
PFP, PFP must also be prevented from running ahead of ME.

Mesa models a PFP acquire as:

```text
ME executes ACQUIRE_MEM
PFP_SYNC_ME
```

A complete CP-PFP sequence is therefore:

```text
RELEASE_MEM with PWS signal
ACQUIRE_MEM with CP_PFP PWS wait
PFP_SYNC_ME
PFP-consumed commands
```

`PFP_SYNC_ME` is not part of shader prefetch. It specifically synchronizes PFP
with ME after the acquire.

Without it, PFP can process later frontend work before ME has completed the PWS
wait. This matters when later PFP work consumes state, draw arguments, or memory
produced by earlier GPU/ME work.

## 8. PWS and cache visibility

PWS and GCR cache controls answer different questions:

```text
PWS event/wait:
  Has the selected pipeline producer completed?

GCR_CNTL and release cache fields:
  Have required caches been written back or invalidated so another consumer can
  observe the results?
```

A PWS wait with insufficient cache writeback can still expose stale memory.
Cache operations issued before the producer has completed can also be too early.
A correct transition frequently needs both:

```text
producer completion event
+ cache writeback/invalidation
+ consumer-stage wait
```

For example, before SDMA reads a rendered color target, the graphics stream must
ensure color writes reach memory visible to SDMA. The local reference used a
GFX12 release event carrying GLV invalidation and GL2 invalidate/writeback bits.
The CPU then waited for the GFX submission fence before issuing the SDMA copy.

## 9. PWS versus other synchronization mechanisms

### PWS versus a memory fence

| PWS | Memory fence |
| --- | --- |
| Uses internal GPU counters. | Writes a value to GPU memory. |
| Does not require an application fence address. | Consumer polls or waits on memory. |
| Efficient for pipeline-local ordering. | Useful for broader or externally visible synchronization. |
| Counter is not CPU-readable. | Fence memory can be CPU-visible. |

### PWS versus the kernel submission fence

The libdrm/kernel fence queried through:

```c
amdgpu_cs_query_fence_status(...)
```

answers:

> Has this submitted job, including the kernel's queue epilogue, completed?

PWS answers:

> Has a selected in-pipeline event occurred so a selected later GPU stage may
> continue?

Typical division of responsibility:

```text
inside GFX IB:
  PWS and cache controls order GPU pipeline work

between submissions:
  CPU waits for the GFX kernel fence before submitting dependent SDMA work

inside/after SDMA submission:
  CPU waits for the SDMA kernel fence before reading GTT
```

A kernel fence does not replace every required in-IB cache or pipeline
transition. Likewise, a PWS wait does not notify the CPU that a submission has
completed.

### PWS versus `PFP_SYNC_ME`

```text
PWS:          order against an internal pipeline event counter
PFP_SYNC_ME:  stop PFP from outrunning ME
```

A CP-PFP PWS acquire commonly needs both the acquire and `PFP_SYNC_ME`.

## 10. PWS counter lifetime and interference

PWS counters belong to the queue timeline. They are not local variables owned by
a C function or one IB.

Mesa notes that a PWS fence generated by a previous IB can be waited on, but an
intervening IB can generate another PWS event and advance the same counter. A
later wait may then observe the newer event.

Practical rules:

- Keep a release/acquire pair in one controlled command sequence when possible.
- Do not assume the counter is unchanged across unrelated submissions.
- Do not split a delicate PWS protocol around command buffers from other
  processes.
- Use the same event/counter family on both sides.
- Treat `PWS_COUNT` as hardware protocol state, not a general application
  semaphore value.

## 11. Plain `ACQUIRE_MEM` is different

GFX12 also has an ordinary cache-acquire form that does not wait for PWS.
Mesa's plain GFX acquire is structurally:

```text
ACQUIRE_MEM, count=6
payload 0: engine selection
payload 1: size_lo = 0xffffffff
payload 2: size_hi = 0x00ffffff
payload 3: base_lo = 0
payload 4: base_hi = 0
payload 5: poll interval = 0x0000000a
payload 6: GCR_CNTL
```

The PWS form instead uses:

```text
payload 0: PWS stage/counter/enable/count
payload 2: size_hi = 0x01ffffff
payload 5: PWS_ENA = 0x80000000
```

Do not mix the layouts.

Use plain `ACQUIRE_MEM` for cache operations that do not wait on a PWS event.
Use the PWS form only as part of a matching event-counter protocol.

## 12. Conceptual pseudocode

A bottom-of-pipe producer and ME consumer can be represented as:

```c
emit_release_mem_pws(
    EVENT_BOTTOM_OF_PIPE_TS,
    required_release_cache_actions);

emit_acquire_mem_pws(
    EVENT_BOTTOM_OF_PIPE_TS,
    STAGE_CP_ME,
    0,
    required_acquire_cache_actions);

emit_consumer_work();
```

For PFP consumption:

```c
emit_release_mem_pws(EVENT_BOTTOM_OF_PIPE_TS, release_cache_actions);
emit_acquire_mem_pws(EVENT_BOTTOM_OF_PIPE_TS, STAGE_CP_PFP, 0,
                     acquire_cache_actions);
emit_pfp_sync_me();
emit_pfp_consumer_work();
```

The exact event, stage, and GCR fields are generation- and transition-specific.
Use a pinned same-generation Mesa/PAL implementation rather than treating this
pseudocode as universal packet values.

## 13. Common mistakes

### Emitting an acquire without a producer

A PWS acquire waits on a counter advanced by an earlier matching release event.
An acquire alone does not force arbitrary earlier graphics work to finish.

### Mixing plain and PWS layouts

Using `size_hi=0x01ffffff` and `PWS_ENA=1` with an ordinary engine-control word
creates a hybrid packet that is neither the normal plain acquire nor a complete
PWS wait.

### Omitting `PFP_SYNC_ME`

If CP PFP is the intended consumer, ME completing `ACQUIRE_MEM` is not enough to
stop PFP from advancing. Emit `PFP_SYNC_ME` when the transition requires PFP
ordering.

### Treating PWS as cache coherency

Pipeline completion does not automatically imply that every relevant cache is
visible to another engine. Select the needed release/acquire cache actions.

### Treating the kernel fence as PWS

The kernel fence is a CPU-visible job-completion mechanism. It does not encode a
stage-specific in-IB pipeline wait.

### Reusing counters across uncontrolled IBs

An unrelated IB can advance a shared PWS counter. Keep paired synchronization in
a controlled queue sequence.

## 14. Debugging checklist

When decoding or constructing a PWS transition:

1. Identify the producer work.
2. Choose an event that completes after that producer.
3. Verify `RELEASE_MEM.PWS_ENABLE=1`.
4. Decode any release-side GLV/GL2 or other cache actions.
5. Match `PWS_COUNTER_SEL` to the release event family.
6. Choose the consumer `PWS_STAGE_SEL` deliberately.
7. Verify both `PWS_ENA2` and `PWS_ENA` are set.
8. Verify `PWS_COUNT` matches the intended counter protocol.
9. Add `PFP_SYNC_ME` when PFP is the consumer.
10. Verify required cache visibility independently of event completion.
11. Keep the pair in a controlled queue timeline.
12. Use a kernel fence separately when the CPU must know submission completion.

A timeout or hang can result from:

- waiting on a counter that was never signaled,
- selecting the wrong counter family,
- selecting an illegal stage,
- omitting frontend synchronization,
- mixing plain and PWS packet fields,
- or using cache actions inappropriate for the transition.

## 15. Source references

The behavior and packet fields here are pinned to Mesa revision
`eda37f052369d3f1579e932abe940fc9b4026855`:

- [`src/amd/common/ac_cmdbuf_cp.c`](https://gitlab.freedesktop.org/mesa/mesa/-/blob/eda37f052369d3f1579e932abe940fc9b4026855/src/amd/common/ac_cmdbuf_cp.c) — `ac_emit_cp_acquire_mem_pws`, `ac_emit_cp_release_mem_pws`, plain `ac_emit_cp_acquire_mem`, and `PFP_SYNC_ME` emission.
- [`src/amd/vulkan/radv_queue.c`](https://gitlab.freedesktop.org/mesa/mesa/-/blob/eda37f052369d3f1579e932abe940fc9b4026855/src/amd/vulkan/radv_queue.c) — required bottom-of-pipe PWS wait before changing GE rings.
- [`src/amd/vulkan/radv_cs.c`](https://gitlab.freedesktop.org/mesa/mesa/-/blob/eda37f052369d3f1579e932abe940fc9b4026855/src/amd/vulkan/radv_cs.c) — RADV cache-transition policy and frontend synchronization.
- [`src/amd/packets/cp_pm4_table_data_gfx12.json`](https://gitlab.freedesktop.org/mesa/mesa/-/blob/eda37f052369d3f1579e932abe940fc9b4026855/src/amd/packets/cp_pm4_table_data_gfx12.json) — GFX12 `ACQUIRE_MEM` and `RELEASE_MEM` packet fields.

Related repository documentation:

- [`PM4 CONTEXT_CONTROL`](pm4-context-control.md)
- [`SET_UCONFIG_REG` versus `SET_UCONFIG_REG_INDEX`](pm4-uconfig-index.md)
- [Raw GFX1201 triangle tutorial](raw-gfx-triangle.md)
