# PM4 UCONFIG writes and indexed UCONFIG writes

AMD PM4 provides two closely related packets for writing registers in the
UCONFIG register space:

- `PKT3_SET_UCONFIG_REG` (`0x79`)
- `PKT3_SET_UCONFIG_REG_INDEX` (`0x7a`)

Both write UCONFIG registers. The indexed form adds a 4-bit command-processor
selector in bits 31:28 of its first payload. That selector is special write
metadata; it is not part of the register address or the value stored in the
register.

This document explains the difference and decodes the raw GFX1201 triangle's
write to `VGT_PRIMITIVE_TYPE`.

## 1. The UCONFIG register space

UCONFIG registers have a byte-address base of:

```text
0x00030000
```

PM4 SET-register packets do not carry the absolute byte address. They carry a
dword-relative offset:

```text
offset = (absolute_register_address - 0x30000) / 4

absolute_register_address = 0x30000 + 4 * offset
```

For example, GFX1201 `VGT_PRIMITIVE_TYPE` is at absolute address `0x030908`:

```text
offset = (0x030908 - 0x030000) / 4
       = 0x908 / 4
       = 0x242
```

The same `0x242` register offset is used by both ordinary and indexed UCONFIG
packet formats. The indexed packet carries additional non-address metadata in
the upper bits of that payload.

## 2. Ordinary `SET_UCONFIG_REG`

Opcode:

```text
PKT3_SET_UCONFIG_REG = 0x79
```

For one register value, the packet is:

```text
header:  PKT3(0x79, count=1)
payload 0:
  bits 15:0   REG_OFFSET
  bits 31:16  reserved, zero
payload 1:
  bits 31:0   REG_DATA
```

The type-3 count is payload length minus one. Two payload dwords therefore use
`count=1`.

A helper is:

```c
static void set_uconfig(pm4 *p, u32 offset, u32 value)
{
    emit(p, PKT3(0x79, 1));
    emit(p, offset);
    emit(p, value);
}
```

The raw triangle uses an ordinary write for `VGT_GS_OUT_PRIM_TYPE`:

```c
set_uconfig(p, 0x266, 2);
```

Decode the target:

```text
absolute address = 0x30000 + 4 * 0x266
                 = 0x30998
register         = VGT_GS_OUT_PRIM_TYPE
value            = 2, TRISTRIP
```

No special selector accompanies this write.

## 3. Indexed `SET_UCONFIG_REG_INDEX`

Opcode:

```text
PKT3_SET_UCONFIG_REG_INDEX = 0x7a
```

For one register value, the packet is:

```text
header:  PKT3(0x7a, count=1)
payload 0:
  bits 15:0   REG_OFFSET
  bits 27:16  reserved, zero
  bits 31:28  INDEX
payload 1:
  bits 31:0   REG_DATA
```

Construct the first payload as:

```c
packed_offset = offset | (index << 28);
```

A less error-prone helper keeps the fields separate:

```c
static void set_uconfig_index(pm4 *p, u32 offset, u32 index, u32 value)
{
    if (offset > 0xffff)
        dief("UCONFIG offset is too large: 0x%x", offset);
    if (!index || index > 0xf)
        dief("invalid UCONFIG index: %u", index);

    emit(p, PKT3(0x7a, 1));
    emit(p, offset | (index << 28));
    emit(p, value);
}
```

Mesa's helper also requires a nonzero index. On older hardware or firmware that
does not support `SET_UCONFIG_REG_INDEX`, Mesa can fall back to the ordinary
packet; the raw GFX1201 path does not need that compatibility fallback.

## 4. Decoding `0x10000242`

The raw triangle currently emits:

```c
ucfg_idx(p, 0x10000242, 4);
```

Its helper accepts the already-packed first payload. Split it into fields:

```text
packed payload = 0x10000242

INDEX      = (0x10000242 >> 28) & 0xf
           = 1

REG_OFFSET = 0x10000242 & 0xffff
           = 0x242

REG_DATA   = 4
```

The register address is derived only from `REG_OFFSET`:

```text
0x30000 + 4 * 0x242 = 0x30908 = VGT_PRIMITIVE_TYPE
```

The complete operation is therefore:

```text
SET_UCONFIG_REG_INDEX
  register = VGT_PRIMITIVE_TYPE
  index    = 1
  value    = 4, DI_PT_TRILIST
```

The `0x10000000` portion is not an address extension. It is the index selector.

## 5. What the index is—and is not

The INDEX field chooses command-processor handling associated with a particular
kind of UCONFIG write.

It is not:

- an array element number,
- a shader-engine selector,
- a register-bank number,
- part of the absolute MMIO address,
- a draw index,
- or the data written to the register.

The GFX12 generated packet definitions name some selector values:

| Index | Generated name |
| ---: | --- |
| `0` | `DEFAULT` |
| `2` | `INDEX_TYPE` |
| `3` | `NUM_INSTANCES` |

Drivers also use selector values defined by the hardware programming model for
specific frontend registers. Examples in RADV include:

```text
VGT_PRIMITIVE_TYPE  -> index 1
VGT_INDEX_TYPE      -> index 2
IA_MULTI_VGT_PARAM  -> index 4
```

Do not infer that index 1 has a universal meaning that can be used for arbitrary
UCONFIG registers. Choose the selector from a matching hardware definition or a
known-good Mesa/PAL implementation for the same GPU generation.

## 6. Side-by-side packet example

### Ordinary write

```c
emit(p, PKT3(0x79, 1));
emit(p, 0x00000266);
emit(p, 0x00000002);
```

Decode:

```text
packet   = SET_UCONFIG_REG
offset   = 0x266
register = VGT_GS_OUT_PRIM_TYPE
value    = 2, TRISTRIP
```

### Indexed write

```c
emit(p, PKT3(0x7a, 1));
emit(p, 0x10000242);
emit(p, 0x00000004);
```

Decode:

```text
packet   = SET_UCONFIG_REG_INDEX
offset   = 0x242
index    = 1
register = VGT_PRIMITIVE_TYPE
value    = 4, TRILIST
```

Both target the UCONFIG space. Only the second carries the selector.

## 7. Why the triangle uses both primitive registers

The two writes describe different sides of the NGG pipeline:

```text
VGT_PRIMITIVE_TYPE = TRILIST
```

This is the input topology. `DRAW_INDEX_AUTO` generates vertex IDs 0, 1, and 2,
and the frontend groups those three inputs as one triangle.

```text
VGT_GS_OUT_PRIM_TYPE = TRISTRIP
```

This describes the topology exported from the merged NGG ES/GS stage toward
rasterization.

For the working stream:

```c
set_uconfig_index(p, 0x242, 1, 4); /* input: DI_PT_TRILIST */
set_uconfig(p,       0x266,    2); /* output: TRISTRIP */
```

These settings are complementary rather than contradictory.

## 8. When to use each packet

Use `SET_UCONFIG_REG` when:

- the register accepts an ordinary UCONFIG write,
- no generation-specific selector is required,
- and a matching hardware or driver implementation uses the ordinary form.

Use `SET_UCONFIG_REG_INDEX` when:

- the hardware definition specifies indexed semantics for the register,
- a known-good same-generation driver uses a nonzero selector,
- or the selector identifies a special frontend register role such as index type
  or primitive type.

Do not mechanically replace one packet with the other. Even though their offset
fields address the same UCONFIG range, the command processor can apply different
handling based on the packet and INDEX field.

## 9. Common mistakes

### Treating the packed payload as an offset

Wrong:

```text
absolute = 0x30000 + 4 * 0x10000242
```

The upper index bits must be removed first:

```text
offset   = 0x10000242 & 0xffff = 0x242
absolute = 0x30000 + 4 * 0x242 = 0x30908
```

### Shifting the index into the wrong position

The GFX12 SET packet places INDEX in bits 31:28:

```c
packed = offset | (index << 28);
```

Not bits 16:19 or 24:27.

### Using an index with the ordinary opcode

`SET_UCONFIG_REG` reserves the upper offset-word bits. This is malformed:

```c
emit(p, PKT3(0x79, 1));
emit(p, 0x10000242); /* index bits are not valid in the ordinary packet */
```

### Copying a selector to an unrelated register

The selector is register- and generation-sensitive. Preserve the pair:

```text
(register, selector)
```

rather than reusing a selector because two registers happen to be in UCONFIG
space.

### Confusing packet index with draw index

`SET_UCONFIG_REG_INDEX.INDEX` controls register-write semantics.
`DRAW_INDEX_AUTO` generates vertex IDs. These uses of “index” are unrelated.

## 10. Decoder checklist

When decoding an indexed UCONFIG write:

1. Verify opcode `0x7a`.
2. Verify the packet payload count.
3. Extract `REG_OFFSET = payload & 0xffff`.
4. Extract `INDEX = (payload >> 28) & 0xf`.
5. Verify reserved bits 27:16 are zero.
6. Compute `absolute = 0x30000 + 4 * REG_OFFSET`.
7. Resolve the symbolic register for the target GPU generation.
8. Confirm the `(register, index)` pair against a hardware definition or a
   matching driver.
9. Decode the following payload as register data, not as part of the address.

For the raw triangle, the expected decode is:

```text
PKT3_SET_UCONFIG_REG_INDEX
  REG_OFFSET = 0x242
  INDEX      = 1
  REG_DATA   = 4
  REGISTER   = VGT_PRIMITIVE_TYPE
  VALUE      = DI_PT_TRILIST
```

## Source references

The field definitions and driver examples below are pinned to Mesa revision
`eda37f052369d3f1579e932abe940fc9b4026855`:

- [`src/amd/packets/cp_pm4_table_data_gfx12.json`](https://gitlab.freedesktop.org/mesa/mesa/-/blob/eda37f052369d3f1579e932abe940fc9b4026855/src/amd/packets/cp_pm4_table_data_gfx12.json) — GFX12 packet layouts.
- [`src/amd/common/ac_cmdbuf.h`](https://gitlab.freedesktop.org/mesa/mesa/-/blob/eda37f052369d3f1579e932abe940fc9b4026855/src/amd/common/ac_cmdbuf.h) — ordinary and indexed UCONFIG helper construction.
- [`src/amd/vulkan/radv_cmd_buffer.c`](https://gitlab.freedesktop.org/mesa/mesa/-/blob/eda37f052369d3f1579e932abe940fc9b4026855/src/amd/vulkan/radv_cmd_buffer.c) — same-generation uses for primitive type, index type, and `IA_MULTI_VGT_PARAM`.
- [`src/amd/registers/gfx12.json`](https://gitlab.freedesktop.org/mesa/mesa/-/blob/eda37f052369d3f1579e932abe940fc9b4026855/src/amd/registers/gfx12.json) — symbolic register names and addresses.

See also:

- [`PM4 CONTEXT_CONTROL`](pm4-context-control.md)
- [Raw GFX1201 triangle tutorial](raw-gfx-triangle.md)
