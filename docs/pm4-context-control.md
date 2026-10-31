# PM4 `CONTEXT_CONTROL`

`PKT3_CONTEXT_CONTROL` configures which register groups the AMD command processor
may automatically **load** from context-state memory and which groups it should
**shadow** back to context-state memory.

It does not write ordinary graphics state, clear registers, flush caches, wait
for idle, switch processes, or launch work. Those jobs belong to packets such as
`SET_*_REG`, `CLEAR_STATE`, `ACQUIRE_MEM`, and draw/dispatch packets.

This document describes the GFX12 encoding used by
`src/amdgpu_raw_triangle_gfx1201.c` and explains why its two payload words are
both `0x80000000`.

## Packet shape

The opcode is `0x28`. The packet contains one header and two payload dwords:

```text
PKT3_CONTEXT_CONTROL, count = 1
load-control word
shadow-control word
```

The PM4 type-3 count field is **payload dwords minus one**, so `count = 1`
means two payload dwords:

```c
emit(&p, PKT3(0x28, 1));
emit(&p, load_control);
emit(&p, shadow_control);
```

The packet is accepted on the graphics command processor. It is not a MEC
compute-queue packet.

## Load-control word

The first payload controls automatic register restoration:

| Bit | GFX12 field | Meaning when set |
| ---: | --- | --- |
| 0 | `LOAD_GLOBAL_CONFIG` | Enable loading global configuration state. |
| 1 | `LOAD_PER_CONTEXT_STATE` | Enable loading per-context state. |
| 15 | `LOAD_GLOBAL_UCONFIG` | Enable loading global UCONFIG registers. |
| 16 | `LOAD_GFX_SH_REGS` | Enable loading graphics shader registers. |
| 24 | `LOAD_CS_SH_REGS` | Enable loading compute shader registers. |
| 31 | `UPDATE_LOAD_ENABLES` | Replace CP's current load-enable mask with this word. |

Bits not listed above are reserved and must be zero.

The important distinction is between the mask and its update strobe:

```text
0x00000000: UPDATE_LOAD_ENABLES = 0
            no request to replace the existing mask

0x80000000: UPDATE_LOAD_ENABLES = 1
            replacement mask has every LOAD_* group disabled
```

Thus an all-zero payload is not a reliable way to disable loading. Bit 31 must
be set to tell CP to adopt the new all-disabled mask.

## Shadow-control word

The second payload controls automatic register shadowing:

| Bit | GFX12 field | Meaning when set |
| ---: | --- | --- |
| 0 | `SHADOW_GLOBAL_CONFIG` | Shadow global configuration writes. |
| 1 | `SHADOW_PER_CONTEXT_STATE` | Shadow per-context register writes. |
| 15 | `SHADOW_GLOBAL_UCONFIG` | Shadow global UCONFIG writes. |
| 16 | `SHADOW_GFX_SH_REGS` | Shadow graphics shader-register writes. |
| 24 | `SHADOW_CS_SH_REGS` | Shadow compute shader-register writes. |
| 31 | `UPDATE_SHADOW_ENABLES` | Replace CP's current shadow-enable mask with this word. |

“Shadowing” means maintaining register values in driver-managed context-state
storage so they can be restored after operations such as preemption or context
switching. Merely enabling these bits does not allocate or configure that
storage; a complete shadow-register scheme also needs valid backing memory and
the corresponding load setup.

As with the load word:

```text
0x80000000 = update the mask and disable every SHADOW_* group
```

## The raw-triangle encoding

The GFX1201 raw triangle emits:

```c
emit(&p, PKT3(0x28, 1));
emit(&p, 0x80000000);
emit(&p, 0x80000000);
```

Decoded:

```text
load-control word   = 0x80000000
  UPDATE_LOAD_ENABLES = 1
  every LOAD_* bit    = 0

shadow-control word = 0x80000000
  UPDATE_SHADOW_ENABLES = 1
  every SHADOW_* bit    = 0
```

The command means:

> Replace both masks, disabling automatic context loads and automatic register
> shadowing.

The following explicit PM4 register writes are therefore the state source for
the experiment. CP is not asked to restore hidden register groups from a
context image or to shadow the subsequent writes.

Mesa RADV emits the same pair when the device is not using shadow registers:

```c
PKT3(PKT3_CONTEXT_CONTROL, 1, 0)
S_281_UPDATE_LOAD_ENABLES(1)
S_282_UPDATE_SHADOW_ENABLES(1)
```

Both field macros encode bit 31, producing the two `0x80000000` payloads.

## What this packet does not initialize

A common mistake is to read the two zero masks as “clear all graphics state.”
That is not what happens.

After this packet, registers may still contain reset values, kernel/queue
preamble values, or earlier context values. The raw command stream must still
program every register required by the draw.

Compare the responsibilities:

| Packet or mechanism | Responsibility |
| --- | --- |
| `CONTEXT_CONTROL` | Select automatic context load and shadow groups. |
| `CLEAR_STATE` | Apply hardware-defined default graphics state where supported. |
| Graphics preamble | Establish queue- and generation-specific baseline state. |
| `ACQUIRE_MEM` | Perform cache visibility operations and/or pipeline waits. |
| `SET_CONTEXT_REG` | Explicitly write context registers. |
| `SET_SH_REG` | Explicitly write shader registers. |
| `SET_UCONFIG_REG` | Explicitly write UCONFIG registers. |
| `DRAW_INDEX_AUTO` | Launch a draw. |

For this reason, the raw triangle follows `CONTEXT_CONTROL` with GE-ring setup,
cache/pipeline synchronization, shader state, color-buffer state, viewport and
raster state, and finally the draw packet.

## Example: enabling selected load groups

Suppose a driver has configured valid context-state backing storage and wants CP
to restore per-context state, global UCONFIG, and graphics shader registers. Its
load word would conceptually be:

```c
load_control =
    (1u << 31) |  /* UPDATE_LOAD_ENABLES */
    (1u << 1)  |  /* LOAD_PER_CONTEXT_STATE */
    (1u << 15) |  /* LOAD_GLOBAL_UCONFIG */
    (1u << 16);   /* LOAD_GFX_SH_REGS */
```

This is:

```text
0x80018002
```

That does not itself perform an arbitrary memory load. It enables those groups
for the surrounding context load/restore mechanism. Copying this value into a
standalone raw stream without valid context-state setup would be incorrect.

## Relationship to shadow-register operation

When shadow registers are fully configured, Mesa uses a broader packet. Its
load word enables the register groups that should be restored, while its shadow
word enables the groups whose writes should be tracked. Conceptually:

```text
load word:
  UPDATE_LOAD_ENABLES
  LOAD_PER_CONTEXT_STATE
  LOAD_GLOBAL_UCONFIG
  LOAD_GFX_SH_REGS
  LOAD_CS_SH_REGS

shadow word:
  UPDATE_SHADOW_ENABLES
  SHADOW_GLOBAL_CONFIG
  SHADOW_PER_CONTEXT_STATE
  SHADOW_GLOBAL_UCONFIG
  SHADOW_GFX_SH_REGS
  SHADOW_CS_SH_REGS
```

That form belongs to a complete context save/restore design. The raw triangle
intentionally chooses the simpler all-disabled masks because it explicitly
programs its state and does not implement shadow backing storage.

## Debugging checklist

When inspecting a decoded IB, verify:

1. The opcode is `CONTEXT_CONTROL` (`0x28`).
2. The type-3 count is `1`, giving exactly two payload dwords.
3. Reserved bits in both payloads are zero.
4. Bit 31 is set if the intent is to replace either enable mask.
5. Enabled load/shadow groups correspond to actually configured context-state
   backing and restore behavior.
6. Do not attribute later cache, register, or draw failures to this packet until
   checking the graphics preamble and explicit `SET_*_REG` state.

For the raw triangle, the expected decode is:

```text
CONTEXT_CONTROL
  LOAD_GLOBAL_CONFIG       = 0
  LOAD_PER_CONTEXT_STATE   = 0
  LOAD_GLOBAL_UCONFIG      = 0
  LOAD_GFX_SH_REGS         = 0
  LOAD_CS_SH_REGS          = 0
  UPDATE_LOAD_ENABLES      = 1

  SHADOW_GLOBAL_CONFIG     = 0
  SHADOW_PER_CONTEXT_STATE = 0
  SHADOW_GLOBAL_UCONFIG    = 0
  SHADOW_GFX_SH_REGS       = 0
  SHADOW_CS_SH_REGS        = 0
  UPDATE_SHADOW_ENABLES    = 1
```

## Source references

The field definitions and examples used here are available in the pinned Mesa
revision `eda37f052369d3f1579e932abe940fc9b4026855`:

- [`amd_cp_packets_gfx12.h`](https://gitlab.freedesktop.org/mesa/mesa/-/blob/eda37f052369d3f1579e932abe940fc9b4026855/src/amd/common/amd_cp_packets_gfx12.h) — generated `S_281_*` and `S_282_*` field definitions. In a Mesa build tree, the generated file is under `build/src/amd/common/`.
- [`src/amd/packets/cp_pm4_table_data_gfx12.json`](https://gitlab.freedesktop.org/mesa/mesa/-/blob/eda37f052369d3f1579e932abe940fc9b4026855/src/amd/packets/cp_pm4_table_data_gfx12.json) — authoritative GFX12 packet-field database.
- [`src/amd/vulkan/radv_queue.c`](https://gitlab.freedesktop.org/mesa/mesa/-/blob/eda37f052369d3f1579e932abe940fc9b4026855/src/amd/vulkan/radv_queue.c) — RADV's `0x80000000`, `0x80000000` emission when shadow registers are disabled.
- [`src/amd/common/ac_shadowed_regs.c`](https://gitlab.freedesktop.org/mesa/mesa/-/blob/eda37f052369d3f1579e932abe940fc9b4026855/src/amd/common/ac_shadowed_regs.c) — broader load/shadow masks used by Mesa's shadow-register setup.

See [the raw GFX1201 triangle tutorial](raw-gfx-triangle.md) for the packets and
explicit register state that follow `CONTEXT_CONTROL` in the working draw.
