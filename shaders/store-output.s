  .text
  .globl start
  .p2align 8
start:
  v_mov_b32 v0, 0
  v_mov_b32 v1, 0x12345678
  global_store_dword v0, v1, s[0:1]
  s_waitcnt vmcnt(0)
  s_endpgm
