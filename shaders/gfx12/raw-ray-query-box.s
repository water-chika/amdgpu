  .text
  .globl start
  .p2align 8
start:
  /* s[0:1] = output VA, s[2:3] = BVH VA. Zero BVH selects Phase 3A. */
  s_or_b32 s4, s2, s3
  s_cmp_lg_u32 s4, 0
  s_cbranch_scc1 .Lbox
  v_mov_b32 v0, 0
  v_mov_b32 v1, 0x3a5a3a5a
  global_store_dword v0, v1, s[0:1] scope:SCOPE_SYS th:TH_STORE_BYPASS
  v_mov_b32 v1, 0xc0decafe
  global_store_dword v0, v1, s[0:1] offset:64 scope:SCOPE_SYS th:TH_STORE_BYPASS
  s_waitcnt vmcnt(0)
  s_endpgm

.Lbox:
  v_mov_b32 v0, 0
  v_mov_b32 v1, 0
  v_mov_b32 v2, 0x41200000       /* tmax = 10 */
  v_mov_b32 v3, 0xff             /* cull mask */
  v_mov_b32 v4, 0                /* origin inside every test box */
  v_mov_b32 v5, 0
  v_mov_b32 v6, 0
  v_mov_b32 v7, 0x3f800000       /* nonzero direction components */
  v_mov_b32 v8, 0x3f800000
  v_mov_b32 v9, 0x3f800000
  v_mov_b32 v10, 5               /* root box node ID */

  /* Mesa create_bvh_descriptor(), with all dynamic ray flags false. */
  s_mov_b32 s4, s2
  s_and_b32 s5, s3, 0xffff
  s_or_b32 s5, s5, 0x80100000
  s_mov_b32 s6, 0xffffffff
  /* Mesa GFX12: 42-bit range, triangle IJ, pointer flags, compressed BVH8,
   * wide sort, instances, and descriptor valid. */
  s_mov_b32 s7, 0x81e803ff

  /* Exact gfx1201 encoding from this assembled object/disassembly:
   * d3e04010 0a000810 07040200
   * image_bvh8_intersect_ray v[16:25],
   *   [v[0:1],v[2:3],v[4:6],v[7:9],v10], s[4:7] */
  image_bvh8_intersect_ray v[16:25], [v[0:1], v[2:3], v[4:6], v[7:9], v10], s[4:7]
  s_wait_bvhcnt 0

  v_mov_b32 v11, 0
  global_store_dwordx4 v11, v[16:19], s[0:1] offset:0 scope:SCOPE_SYS th:TH_STORE_BYPASS
  global_store_dwordx4 v11, v[20:23], s[0:1] offset:16 scope:SCOPE_SYS th:TH_STORE_BYPASS
  global_store_dwordx2 v11, v[24:25], s[0:1] offset:32 scope:SCOPE_SYS th:TH_STORE_BYPASS
  /* Components 10..15 are the returned origin and direction. */
  global_store_dwordx3 v11, v[4:6], s[0:1] offset:40 scope:SCOPE_SYS th:TH_STORE_BYPASS
  global_store_dwordx3 v11, v[7:9], s[0:1] offset:52 scope:SCOPE_SYS th:TH_STORE_BYPASS
  v_mov_b32 v12, 0xc0decafe
  global_store_dword v11, v12, s[0:1] offset:64 scope:SCOPE_SYS th:TH_STORE_BYPASS
  s_waitcnt vmcnt(0)
  s_endpgm
