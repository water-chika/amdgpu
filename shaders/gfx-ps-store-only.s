  .text
  .globl start
  .p2align 8
start:
  ;; s[0:1] is an output buffer address programmed through SPI_SHADER_USER_DATA_PS_0/1.
  v_mov_b32 v0, 0
  v_mov_b32 v1, 0xfeed1234
  global_store_dword v0, v1, s[0:1] scope:SCOPE_SYS th:TH_STORE_BYPASS
  s_waitcnt vmcnt(0)
  exp mrt0 v0, v0, v0, v0 done
  s_waitcnt expcnt(0)
  s_endpgm
