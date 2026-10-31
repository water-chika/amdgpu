/*
 * GFX1201 constant-color pixel shader.
 * Words are copied exactly from the captured RADV disassembly. The packed
 * FP16 export is logical RGBA (1, 0, 0, 1); with the captured CB state it
 * produces RGBA8 red, stored as little-endian dword 0xff0000ff.
 */
.text
.globl start
.p2align 8
start:
  .long 0x7e0002ff, 0x00003c00
  .long 0x7e0202ff, 0x3c000000
  .long 0xf8000803, 0x80800100
  .long 0xbfb00000
