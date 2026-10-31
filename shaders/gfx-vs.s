  .text
  .globl start
  .p2align 8
start:
  ;; Assume v0 contains vertex id. Emit three clip-space positions.
  v_mov_b32 v5, 0
  v_mov_b32 v6, 0x3f400000      ;;  0.75
  v_mov_b32 v7, 0xbf400000      ;; -0.75

  ;; x = vertex_id == 0 ? 0.0 : vertex_id == 1 ? 0.75 : -0.75
  v_mov_b32 v1, v7
  v_cmp_eq_u32 0, v0
  v_cndmask_b32 v1, v1, v5
  v_cmp_eq_u32 1, v0
  v_cndmask_b32 v1, v1, v6

  ;; y = vertex_id == 0 ? -0.75 : 0.75
  v_mov_b32 v2, v6
  v_cmp_eq_u32 0, v0
  v_cndmask_b32 v2, v2, v7

  v_mov_b32 v3, 0
  v_mov_b32 v4, 0x3f800000
  v_mov_b32 v8, 0x76543210
  global_store_dword v5, v8, s[0:1] scope:SCOPE_SYS th:TH_STORE_BYPASS
  v_add_u32_e32 v9, 4, v0
  v_lshlrev_b32 v9, 2, v9
  global_store_dword v9, v8, s[0:1] scope:SCOPE_SYS th:TH_STORE_BYPASS
  v_lshlrev_b32 v9, 1, v0
  v_add_u32_e32 v9, 16, v9
  v_lshlrev_b32 v9, 2, v9
  global_store_dword v9, v1, s[0:1] scope:SCOPE_SYS th:TH_STORE_BYPASS
  v_add_u32_e32 v9, 4, v9
  global_store_dword v9, v2, s[0:1] scope:SCOPE_SYS th:TH_STORE_BYPASS
  s_waitcnt vmcnt(0)
  exp pos0 v1, v2, v3, v4 done
  s_waitcnt expcnt(0)
  s_endpgm
