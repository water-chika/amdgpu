  .text
  .globl start
  .p2align 8
start:
  ;; s[0:1] is an output buffer address programmed through COMPUTE_USER_DATA_0/1.
  v_mov_b32 v0, 0
  v_mov_b32 v1, 2
  v_add_u32_e32 v1, 3, v1
  global_store_dword v0, v1, s[0:1] scope:SCOPE_SYS th:TH_STORE_BYPASS
  s_waitcnt vmcnt(0)
  s_endpgm
