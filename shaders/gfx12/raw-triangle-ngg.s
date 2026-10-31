/*
 * GFX1201 NGG procedural triangle shader.
 * Words are copied exactly from the captured RADV "Vertex Shader as ESGS"
 * disassembly. Keeping raw words avoids relying on the installed assembler's
 * GFX12 syntax support; docs/raw-gfx-triangle.md records the disassembly.
 */
.text
.globl start
.p2align 8
start:
  .long 0xbfb50003
  .long 0x99000380
  .long 0xbf870009
  .long 0x947e00c1
  .long 0xbfa50011
  .long 0x4a060608
  .long 0x7e040280
  .long 0x7e0802f2
  .long 0xbf870003
  .long 0x7c840681
  .long 0xd5010001, 0x41a9feff, 0xbf400000
  .long 0x7c840680
  .long 0xbf88fffd
  .long 0xd5010001, 0x01a90101
  .long 0xd5010003, 0x41a9feff, 0x3f400000
  .long 0xf80008cf, 0x04020301
  .long 0x84038803
  .long 0xbfc40000
  .long 0xbf870009
  .long 0x947e03c1
  .long 0xbfa50002
  .long 0xf8000941, 0x80808000
  .long 0xbfb00000
