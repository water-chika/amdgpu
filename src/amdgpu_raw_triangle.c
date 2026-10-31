#define _GNU_SOURCE

#include "common.h"

#include <amdgpu.h>
#include <amdgpu_drm.h>
#include <fcntl.h>
#include <stdbool.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#define PKT3(op, count, pred) ((3u << 30) | (((count) & 0x3fff) << 16) | (((op) & 0xff) << 8) | ((pred) & 1u))
#define PKT3_CLEAR_STATE 0x12u
#define PKT3_CONTEXT_CONTROL 0x28u
#define PKT3_DRAW_INDEX_AUTO 0x2du
#define PKT3_NUM_INSTANCES 0x2fu
#define PKT3_EVENT_WRITE 0x46u
#define PKT3_ACQUIRE_MEM 0x58u
#define PKT3_SET_CONTEXT_REG 0x69u
#define PKT3_SET_UCONFIG_REG 0x79u
#define PKT3_SET_UCONFIG_REG_INDEX 0x7au
#define PKT3_SET_SH_REG 0x76u

#define WIDTH 256u
#define HEIGHT 256u
#define COLOR_BYTES (WIDTH * HEIGHT * 4u)

typedef struct { u32 *data; size_t count, capacity; } pm4;
typedef struct { amdgpu_bo_handle handle; amdgpu_va_handle va_handle; u64 va; size_t size; void *cpu; } bo;
typedef struct { int fd; u32 major, minor; amdgpu_device_handle device; amdgpu_context_handle context; } gpu;
typedef enum {
  RAW_MODE_VS_ONLY,
  RAW_MODE_PS_STORE_ONLY,
  RAW_MODE_COLOR_TARGET,
} raw_mode;

typedef struct { raw_mode mode; } options;

static options parse_options(int argc, char **argv) {
  options opt = {0};
  for (int i = 1; i < argc; i++) {
    if (!strcmp(argv[i], "--color-target")) {
      if (opt.mode != RAW_MODE_VS_ONLY) dief("only one experiment option can be selected");
      opt.mode = RAW_MODE_COLOR_TARGET;
    } else if (!strcmp(argv[i], "--ps-store-only")) {
      if (opt.mode != RAW_MODE_VS_ONLY) dief("only one experiment option can be selected");
      opt.mode = RAW_MODE_PS_STORE_ONLY;
    } else if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) {
      printf("usage: amdgpu-raw-triangle [--ps-store-only|--color-target]\n\n"
             "By default this runs a raw GFX DRAW_INDEX_AUTO and proves VS launch by\n"
             "writing 0x76543210 to a GTT output BO. --ps-store-only enables a\n"
             "PS memory-store experiment without CB output. --color-target enables\n"
             "the experimental PS/color-buffer path, which is not stable yet.\n");
      exit(0);
    } else {
      dief("unknown option: %s", argv[i]);
    }
  }
  return opt;
}

static void push(pm4 *p, u32 v) {
  if (p->count == p->capacity) {
    size_t cap = p->capacity ? p->capacity * 2 : 256;
    u32 *next = realloc(p->data, cap * sizeof(*next));
    if (!next) die_errno("realloc pm4");
    p->data = next;
    p->capacity = cap;
  }
  p->data[p->count++] = v;
}

static void set_context(pm4 *p, u32 offset, u32 value) {
  push(p, PKT3(PKT3_SET_CONTEXT_REG, 1, 0)); push(p, offset); push(p, value);
}

static void set_sh(pm4 *p, u32 offset, u32 value) {
  push(p, PKT3(PKT3_SET_SH_REG, 1, 0)); push(p, offset); push(p, value);
}

static void set_uconfig(pm4 *p, u32 offset, u32 value) {
  push(p, PKT3(PKT3_SET_UCONFIG_REG, 1, 0)); push(p, offset); push(p, value);
}

static void set_uconfig_index(pm4 *p, u32 index_offset, u32 value) {
  push(p, PKT3(PKT3_SET_UCONFIG_REG_INDEX, 1, 0)); push(p, index_offset); push(p, value);
}

static u8 *read_file(const char *path, size_t *size) {
  int fd = open(path, O_RDONLY | O_CLOEXEC);
  if (fd < 0) die_errno(path);
  struct stat st;
  if (fstat(fd, &st) < 0) die_errno("fstat shader");
  if (st.st_size <= 0) dief("empty shader: %s", path);
  u8 *data = malloc((size_t)st.st_size);
  if (!data) die_errno("malloc shader");
  size_t off = 0;
  while (off < (size_t)st.st_size) {
    ssize_t got = read(fd, data + off, (size_t)st.st_size - off);
    if (got < 0) die_errno("read shader");
    if (!got) dief("short read: %s", path);
    off += (size_t)got;
  }
  close(fd);
  *size = (size_t)st.st_size;
  return data;
}

static void gpu_open(gpu *g, const char *path) {
  g->fd = open(path, O_RDWR | O_CLOEXEC);
  if (g->fd < 0) die_errno(path);
  int ret = amdgpu_device_initialize(g->fd, &g->major, &g->minor, &g->device);
  check_ret(ret, "amdgpu_device_initialize");
  ret = amdgpu_cs_ctx_create(g->device, &g->context);
  check_ret(ret, "amdgpu_cs_ctx_create");
}

static void gpu_close(gpu *g) {
  if (g->context) amdgpu_cs_ctx_free(g->context);
  if (g->device) amdgpu_device_deinitialize(g->device);
  if (g->fd >= 0) close(g->fd);
}

static void bo_alloc_aligned(gpu *g, size_t requested, u32 domain, u64 alignment, bo *b) {
  memset(b, 0, sizeof(*b));
  size_t size = (size_t)align_up_u64(requested, alignment);
  struct amdgpu_bo_alloc_request req = {
    .alloc_size = size,
    .phys_alignment = alignment,
    .preferred_heap = domain,
    .flags = AMDGPU_GEM_CREATE_CPU_ACCESS_REQUIRED,
  };
  int ret = amdgpu_bo_alloc(g->device, &req, &b->handle);
  check_ret(ret, "amdgpu_bo_alloc");
  ret = amdgpu_va_range_alloc(g->device, amdgpu_gpu_va_range_general, size, alignment, 0, &b->va, &b->va_handle, 0);
  check_ret(ret, "amdgpu_va_range_alloc");
  ret = amdgpu_bo_va_op(b->handle, 0, size, b->va,
                        AMDGPU_VM_PAGE_READABLE | AMDGPU_VM_PAGE_WRITEABLE | AMDGPU_VM_PAGE_EXECUTABLE,
                        AMDGPU_VA_OP_MAP);
  check_ret(ret, "amdgpu_bo_va_op");
  ret = amdgpu_bo_cpu_map(b->handle, &b->cpu);
  check_ret(ret, "amdgpu_bo_cpu_map");
  memset(b->cpu, 0, size);
  b->size = size;
}

static void bo_alloc(gpu *g, size_t requested, u32 domain, bo *b) {
  bo_alloc_aligned(g, requested, domain, 4096, b);
}

static void bo_free(bo *b) {
  if (!b->handle) return;
  if (b->cpu) amdgpu_bo_cpu_unmap(b->handle);
  if (b->va) amdgpu_bo_va_op(b->handle, 0, b->size, b->va, 0, AMDGPU_VA_OP_UNMAP);
  if (b->va_handle) amdgpu_va_range_free(b->va_handle);
  amdgpu_bo_free(b->handle);
}

static void upload(bo *b, const void *data, size_t size) {
  if (size > b->size) dief("upload too large");
  memcpy(b->cpu, data, size);
}

static void scan_pixels(const char *label, const u32 *pixels) {
  u32 first = 0, count = 0, min_x = WIDTH, min_y = HEIGHT, max_x = 0, max_y = 0;
  for (u32 y = 0; y < HEIGHT; y++) {
    for (u32 x = 0; x < WIDTH; x++) {
      u32 v = pixels[y * WIDTH + x];
      if (!v) continue;
      if (!first) first = v;
      count++;
      if (x < min_x) min_x = x;
      if (y < min_y) min_y = y;
      if (x > max_x) max_x = x;
      if (y > max_y) max_y = y;
    }
  }
  printf("%s[128,128] = 0x%08x first_nonzero = 0x%08x nonzero = %u bbox = %u,%u..%u,%u\n",
         label, pixels[128u * WIDTH + 128u], first, count, min_x, min_y, max_x, max_y);
}

static void build_pm4(pm4 *p, const bo *vs, const bo *ps, const bo *out, const bo *color,
                      raw_mode mode) {
  bool ps_store_only = mode == RAW_MODE_PS_STORE_ONLY;
  bool color_target = mode == RAW_MODE_COLOR_TARGET;
  bool ps_enabled = ps_store_only || color_target;
  push(p, PKT3(PKT3_CONTEXT_CONTROL, 1, 0)); push(p, 0x80000000); push(p, 0x80000000);
  //push(p, PKT3(PKT3_CLEAR_STATE, 0, 0)); push(p, 0);
  push(p, PKT3(PKT3_ACQUIRE_MEM, 5, 0));
  push(p, 0x28c40000); push(p, 0xffffffff); push(p, 0x00ffffff); push(p, 0); push(p, 0); push(p, 10); push(p, 0);

  set_context(p, 0x81, 0x00000000); /* PA_SC_WINDOW_SCISSOR_TL */
  set_context(p, 0x82, 0x01000100); /* PA_SC_WINDOW_SCISSOR_BR */
  set_context(p, 0x0d, 0x01000100); /* PA_SC_SCREEN_SCISSOR_BR */
  set_context(p, 0x109, 0x00000412); /* CB_DCC_CONTROL */
  set_context(p, 0x01, 0x00000001); /* DB_COUNT_CONTROL */
  set_context(p, 0x311, 0x19fc0900); /* PA_SC_BINNER_CNTL_0 */
  set_context(p, 0x1c3, 0x00000004); /* SPI_SHADER_POS_FORMAT */
  set_context(p, 0x207, 0x00000000); /* PA_CL_VS_OUT_CNTL */
  set_context(p, 0x1b3, ps_enabled ? 0x00000080 : 0x00000000); /* SPI_PS_INPUT_ENA */
  set_context(p, 0x1b4, ps_enabled ? 0x00000080 : 0x00000000); /* SPI_PS_INPUT_ADDR */
  set_context(p, 0x1b6, 0x00000000); /* SPI_PS_IN_CONTROL */
  set_context(p, 0x310, 0x00000000); /* PA_SC_SHADER_CONTROL */
  set_context(p, 0x2a1, 0x00000000); /* VGT_PRIMITIVEID_EN */
  set_context(p, 0x290, 0x00000000); /* VGT_GS_MODE = GS_OFF */
  set_context(p, 0x316, 0x0000001e); /* VGT_VERTEX_REUSE_BLOCK_CNTL */
  set_context(p, 0x2d5, 0x00010000); /* VGT_SHADER_STAGES_EN = VS_STAGE_REAL */
  set_context(p, 0x29b, 0x00000002); /* VGT_GS_OUT_PRIM_TYPE = TRISTRIP */
  set_context(p, 0x282, 0x00000008); /* PA_SU_LINE_CNTL */
  set_context(p, 0x283, 0x00ff0000); /* PA_SC_LINE_STIPPLE */
  set_context(p, 0x2f7, 0x00000000); /* PA_SC_LINE_CNTL */
  set_context(p, 0x204, 0x01080000); /* PA_CL_CLIP_CNTL */
  set_context(p, 0x205, 0x00000240); /* PA_SU_SC_MODE_CNTL */
  set_context(p, 0x206, 0x0000003f); /* PA_CL_VTE_CNTL */
  set_context(p, 0x8f, ps_enabled ? 0x0000000f : 0x00000000); /* CB_SHADER_MASK */
  set_context(p, 0x1c4, 0x00000000); /* SPI_SHADER_Z_FORMAT */
  set_context(p, 0x1c5, color_target ? 0x00000009 : (ps_store_only ? 0x00000004 : 0x00000000)); /* SPI_SHADER_COL_FORMAT */
  if (color_target) {
    u64 color_base = color->va >> 8;
    set_context(p, 0x318, (u32)color_base);         /* CB_COLOR0_BASE */
    set_context(p, 0x319, (u32)(color_base >> 32)); /* CB_COLOR0_BASE_EXT */
    set_context(p, 0x31a, 0x003fc0ff); /* CB_COLOR0_ATTRIB2: 256x256 */
    set_context(p, 0x31b, 0x00000000); /* CB_COLOR0_VIEW */
    set_context(p, 0x31c, 0x00028028); /* CB_COLOR0_INFO: RGBA8 UNORM */
    set_context(p, 0x31d, 0xd0000000); /* CB_COLOR0_ATTRIB from RADV linear image */
    set_context(p, 0x31e, 0x00000218); /* CB_COLOR0_DCC_CONTROL */
    set_context(p, 0x31f, (u32)color_base); set_context(p, 0x320, (u32)(color_base >> 32));
    set_context(p, 0x321, (u32)color_base); set_context(p, 0x322, (u32)(color_base >> 32));
    set_context(p, 0x325, 0x00000000); set_context(p, 0x326, 0x00000000);
    set_context(p, 0x1e8, 0x000000ff); /* CB_MRT0_EPITCH */
  }
  set_context(p, 0x0e, 0x00000000); set_context(p, 0x0f, 0x00000000); /* DB_Z/STENCIL_INFO */
  set_context(p, 0x00, 0x00000000); /* DB_RENDER_CONTROL */
  set_context(p, 0x03, 0x00000000); /* DB_RENDER_OVERRIDE */
  set_context(p, 0x04, 0x00000000); /* DB_RENDER_OVERRIDE2 */
  set_context(p, 0x200, 0x00700770); /* DB_DEPTH_CONTROL */
  if (ps_store_only || color_target) {
    set_context(p, 0x203, ps_store_only ? 0x00000410 : 0x00000010); /* DB_SHADER_CONTROL */
  }
  set_context(p, 0x8e, color_target ? 0x0000000f : 0x00000000); /* CB_TARGET_MASK */
  set_context(p, 0x202, 0x00cc0010); /* CB_COLOR_CONTROL */
  set_context(p, 0x30e, 0xffffffff); /* PA_SC_AA_MASK_X0Y0_X1Y0 */
  set_context(p, 0x30f, 0xffffffff); /* PA_SC_AA_MASK_X0Y1_X1Y1 */
  set_context(p, 0x2f8, 0x00000000); /* PA_SC_AA_CONFIG */
  set_context(p, 0x2fe, 0x00000000); /* PA_SC_AA_SAMPLE_LOCS_PIXEL_X0Y0_0 */
  set_context(p, 0x302, 0x00000000); /* PA_SC_AA_SAMPLE_LOCS_PIXEL_X1Y0_0 */
  set_context(p, 0x306, 0x00000000); /* PA_SC_AA_SAMPLE_LOCS_PIXEL_X0Y1_0 */
  set_context(p, 0x30a, 0x00000000); /* PA_SC_AA_SAMPLE_LOCS_PIXEL_X1Y1_0 */
  set_context(p, 0x2f5, 0x00000000); /* PA_SC_CENTROID_PRIORITY_0 */
  set_context(p, 0x2f6, 0x00000000); /* PA_SC_CENTROID_PRIORITY_1 */
  for (u32 i = 0; i < 8; i++) set_context(p, 0x1e0 + i, 0x00000000); /* CB_BLEND*_CONTROL */
  for (u32 i = 0; i < 8; i++) set_context(p, 0x1d8 + i, 0x06000600); /* SX_MRT*_BLEND_OPT */
  set_context(p, 0x1d5, 0x00000005); /* SX_PS_DOWNCONVERT */
  set_context(p, 0x1d6, 0x00000006); /* SX_BLEND_OPT_EPSILON */
  set_context(p, 0x1d7, 0x00000000); /* SX_BLEND_OPT_CONTROL */
  set_context(p, 0x292, 0x00000022); /* PA_SC_MODE_CNTL_0 */
  set_context(p, 0x2dc, 0x00018700); /* DB_ALPHA_TO_MASK */
  set_context(p, 0x201, 0x00130000); /* DB_EQAA */
  set_context(p, 0x313, 0x00100000); /* PA_SC_CONSERVATIVE_RASTERIZATION_CNTL */
  set_context(p, 0x20b, 0xc0000000); /* PA_SU_PRIM_FILTER_CNTL */
  set_context(p, 0x1b8, 0x00000000); /* SPI_BARYC_CNTL */
  set_context(p, 0x293, 0x760201bc); /* PA_SC_MODE_CNTL_1 */
  set_context(p, 0x2fa, 0x437efe00); set_context(p, 0x2fb, 0x3f800000);
  set_context(p, 0x2fc, 0x437efe00); set_context(p, 0x2fd, 0x3f800000);
  set_context(p, 0x83, 0x0000ffff);  /* PA_SC_CLIPRECT_RULE */
  set_context(p, 0x10f, 0x43000000); set_context(p, 0x110, 0x43000000);
  set_context(p, 0x111, 0x43000000); set_context(p, 0x112, 0x43000000);
  set_context(p, 0x113, 0x3f800000); set_context(p, 0x114, 0x00000000);
  set_context(p, 0xb4, 0x00000000); set_context(p, 0xb5, 0x3f800000);
  set_context(p, 0x94, 0x80000000); set_context(p, 0x95, 0x01000100);

  set_context(p, 0x202, 0x00cc0010);
  set_uconfig_index(p, 0x10000242, 0x00000004); /* VGT_PRIMITIVE_TYPE = TRILIST */
  set_uconfig_index(p, 0x40000258, 0x0070007f); /* IA_MULTI_VGT_PARAM */
  set_uconfig(p, 0x24b, 0x00000000);            /* VGT_MULTI_PRIM_IB_RESET_EN */

  u64 vs_pgm = vs->va >> 8;
  set_sh(p, 0x48, (u32)vs_pgm); set_sh(p, 0x49, (u32)(vs_pgm >> 32));
  set_sh(p, 0x4a, 0x002c0042); set_sh(p, 0x4b, 0x00000004);
  set_sh(p, 0x4c, (u32)out->va); set_sh(p, 0x4d, (u32)(out->va >> 32));
  set_sh(p, 0x46, 0x003ffffe); set_sh(p, 0x47, 0x00000014);

  u64 ps_pgm = ps->va >> 8;
  set_sh(p, 0x08, (u32)ps_pgm); set_sh(p, 0x09, (u32)(ps_pgm >> 32));
  set_sh(p, 0x0a, 0x002c0040); set_sh(p, 0x0b, ps_enabled ? 0x0000000c : 0x00000004);
  set_sh(p, 0x0c, (u32)out->va); set_sh(p, 0x0d, (u32)(out->va >> 32));

  push(p, PKT3(PKT3_EVENT_WRITE, 0, 0)); push(p, 0x0000000e); /* BREAK_BATCH */
  push(p, PKT3(PKT3_NUM_INSTANCES, 0, 0)); push(p, 1);
  push(p, PKT3(PKT3_DRAW_INDEX_AUTO, 1, 0)); push(p, 3); push(p, 2);
  if (color_target) {
    push(p, PKT3(PKT3_ACQUIRE_MEM, 5, 0));
    push(p, 0x28c40000); push(p, 0xffffffff); push(p, 0x00ffffff); push(p, 0); push(p, 0); push(p, 10);
  }
}

static void submit(gpu *g, pm4 *p, bo *ib, bo *vs, bo *ps, bo *out, bo *color, bool color_target) {
  bo_alloc(g, p->count * sizeof(u32), AMDGPU_GEM_DOMAIN_GTT, ib);
  upload(ib, p->data, p->count * sizeof(u32));
  amdgpu_bo_handle handles[5] = {ib->handle, vs->handle, ps->handle, out->handle, color->handle};
  amdgpu_bo_list_handle list = NULL;
  int ret = amdgpu_bo_list_create(g->device, color_target ? 5 : 4, handles, NULL, &list);
  check_ret(ret, "amdgpu_bo_list_create");
  struct amdgpu_cs_ib_info ib_info = {.ib_mc_address = ib->va, .size = (u32)p->count};
  struct amdgpu_cs_request req = {.ip_type = AMDGPU_HW_IP_GFX, .resources = list, .number_of_ibs = 1, .ibs = &ib_info};
  ret = amdgpu_cs_submit(g->context, 0, &req, 1);
  check_ret(ret, "amdgpu_cs_submit(GFX)");
  struct amdgpu_cs_fence fence = {.context = g->context, .ip_type = AMDGPU_HW_IP_GFX, .fence = req.seq_no};
  u32 expired = 0;
  ret = amdgpu_cs_query_fence_status(&fence, 5ull * 1000ull * 1000ull * 1000ull, 0, &expired);
  check_ret(ret, "amdgpu_cs_query_fence_status(GFX)");
  if (!expired) dief("GFX fence timed out");
  amdgpu_bo_list_destroy(list);
}

int main(int argc, char **argv) {
  options opt = parse_options(argc, argv);
  size_t vs_size, ps_size;
  u8 *vs_code = read_file("build/gfx-vs.bin", &vs_size);
  const char *ps_path = opt.mode == RAW_MODE_PS_STORE_ONLY ? "build/gfx-ps-store-only.bin" : "build/gfx-ps-store.bin";
  u8 *ps_code = read_file(ps_path, &ps_size);
  gpu g = {.fd = -1};
  bo vs = {0}, ps = {0}, out = {0}, color = {0}, ib = {0};
  pm4 p = {0};
  gpu_open(&g, "/dev/dri/renderD128");
  bo_alloc(&g, vs_size, AMDGPU_GEM_DOMAIN_VRAM, &vs);
  bo_alloc(&g, ps_size, AMDGPU_GEM_DOMAIN_VRAM, &ps);
  bo_alloc(&g, 4096, AMDGPU_GEM_DOMAIN_GTT, &out);
  bo_alloc_aligned(&g, WIDTH * HEIGHT * 4, AMDGPU_GEM_DOMAIN_VRAM, 65536, &color);
  upload(&vs, vs_code, vs_size);
  upload(&ps, ps_code, ps_size);
  build_pm4(&p, &vs, &ps, &out, &color, opt.mode);
  if (opt.mode == RAW_MODE_COLOR_TARGET) {
    amdgpu_bo_cpu_unmap(color.handle);
    color.cpu = NULL;
  }
  printf("submitting raw GFX triangle draw: %zu PM4 dwords\n", p.count);
  submit(&g, &p, &ib, &vs, &ps, &out, &color, opt.mode == RAW_MODE_COLOR_TARGET);
  printf("output[0] = 0x%08x\n", ((u32 *)out.cpu)[0]);
  if (opt.mode == RAW_MODE_COLOR_TARGET) {
    printf("vs_markers[4..7] = 0x%08x 0x%08x 0x%08x 0x%08x\n",
           ((u32 *)out.cpu)[4], ((u32 *)out.cpu)[5], ((u32 *)out.cpu)[6], ((u32 *)out.cpu)[7]);
    printf("vs_pos = (0x%08x,0x%08x) (0x%08x,0x%08x) (0x%08x,0x%08x)\n",
           ((u32 *)out.cpu)[16], ((u32 *)out.cpu)[17], ((u32 *)out.cpu)[18],
           ((u32 *)out.cpu)[19], ((u32 *)out.cpu)[20], ((u32 *)out.cpu)[21]);
    int ret = amdgpu_bo_cpu_map(color.handle, &color.cpu);
    check_ret(ret, "amdgpu_bo_cpu_map(color direct readback)");
    scan_pixels("direct", color.cpu);
  }
  bo_free(&ib); bo_free(&color); bo_free(&out); bo_free(&ps); bo_free(&vs); gpu_close(&g);
  free(p.data); free(vs_code); free(ps_code);
  return 0;
}
