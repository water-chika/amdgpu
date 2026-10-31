#define _GNU_SOURCE
#include "common.h"
#include <amdgpu.h>
#include <amdgpu_drm.h>
#include <fcntl.h>
#include <stdbool.h>
#include <sys/stat.h>
#include <unistd.h>

#define PKT3(op, n) ((3u << 30) | (((n) & 0x3fffu) << 16) | ((op) << 8))
#define WIDTH 256u
#define HEIGHT 256u
#define COLOR_BYTES (WIDTH * HEIGHT * 4u)
#define FENCE_NS (5ull * 1000ull * 1000ull * 1000ull)

typedef struct {
  u32 *v;
  size_t n, cap;
} pm4;
typedef struct {
  amdgpu_bo_handle h;
  amdgpu_va_handle vh;
  u64 va;
  size_t size;
  void *cpu;
} bo;
typedef struct {
  int fd;
  u32 major, minor, address32_hi;
  amdgpu_device_handle dev;
  amdgpu_context_handle ctx;
  struct amdgpu_gpu_info info;
} gpu;
typedef enum { STAGE_INFO, STAGE_RINGS, STAGE_DRAW } stage;
typedef struct {
  const char *node;
  stage stop;
  bool dump;
} options;

static void emit(pm4 *p, u32 x) {
  if (p->n == p->cap) {
    p->cap = p->cap ? p->cap * 2 : 512;
    p->v = realloc(p->v, p->cap * 4);
    if (!p->v)
      die_errno("realloc PM4");
  }
  p->v[p->n++] = x;
}
static void ctx(pm4 *p, u32 off, u32 val) {
  emit(p, PKT3(0x69, 1));
  emit(p, off);
  emit(p, val);
}
static void sh(pm4 *p, u32 off, u32 val) {
  emit(p, PKT3(0x76, 1));
  emit(p, off);
  emit(p, val);
}
static void ucfg(pm4 *p, u32 off, u32 val) {
  emit(p, PKT3(0x79, 1));
  emit(p, off);
  emit(p, val);
}
static void ucfg_idx(pm4 *p, u32 off, u32 val) {
  emit(p, PKT3(0x7a, 1));
  emit(p, off);
  emit(p, val);
}
static void acquire(pm4 *p, u32 control, u32 gcr) {
  /* GFX12 layout/count from Mesa ac_emit_cp_acquire_mem: header count 6 means
   * seven payload dwords. */
  emit(p, PKT3(0x58, 6));
  emit(p, control);
  emit(p, 0xffffffff);
  emit(p, 0x01ffffff);
  emit(p, 0);
  emit(p, 0);
  emit(p, 0x80000000);
  emit(p, gcr);
}
static void release(pm4 *p, u32 event_gcr) {
  /* GFX12 RELEASE_MEM is likewise seven payload dwords (Mesa ac_cmdbuf_cp.c).
   */
  emit(p, PKT3(0x49, 6));
  emit(p, event_gcr);
  emit(p, 0);
  emit(p, 0);
  emit(p, 0);
  emit(p, 0);
  emit(p, 0);
  emit(p, 0);
}
static options parse(int argc, char **argv) {
  options o = {.node = "/dev/dri/renderD128", .stop = STAGE_DRAW};
  for (int i = 1; i < argc; i++) {
    if (!strcmp(argv[i], "--dump-pm4"))
      o.dump = true;
    else if (!strcmp(argv[i], "--device")) {
      if (i + 1 >= argc)
        dief("--device requires a node");
      o.node = argv[++i];
    } else if (!strcmp(argv[i], "--stage")) {
      if (i + 1 >= argc)
        dief("--stage requires info, rings, or draw");
      i++;
      if (!strcmp(argv[i], "info"))
        o.stop = STAGE_INFO;
      else if (!strcmp(argv[i], "rings"))
        o.stop = STAGE_RINGS;
      else if (!strcmp(argv[i], "draw"))
        o.stop = STAGE_DRAW;
      else
        dief("bad --stage: %s", argv[i]);
    } else if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) {
      printf("usage: amdgpu-raw-triangle-gfx1201 [--device NODE] [--stage "
             "info|rings|draw] [--dump-pm4]\n");
      exit(0);
    } else
      dief("unknown option: %s", argv[i]);
  }
  return o;
}
static void open_gpu(gpu *g, const char *node) {
  memset(g, 0, sizeof(*g));
  g->fd = open(node, O_RDWR | O_CLOEXEC);
  if (g->fd < 0)
    die_errno(node);
  check_ret(amdgpu_device_initialize(g->fd, &g->major, &g->minor, &g->dev),
            "amdgpu_device_initialize");
  check_ret(amdgpu_query_gpu_info(g->dev, &g->info), "amdgpu_query_gpu_info");
  check_ret(amdgpu_query_sw_info(g->dev, amdgpu_sw_info_address32_hi,
                                 &g->address32_hi),
            "amdgpu_query_sw_info(address32_hi)");
  check_ret(amdgpu_cs_ctx_create(g->dev, &g->ctx), "amdgpu_cs_ctx_create");
  printf("GPU: node=%s DRM=%u.%u asic=0x%04x family=%u chip_rev=%u "
         "external_rev=%u pci_rev=0x%02x SE=%u CU=%u address32_hi=0x%08x\n",
         node, g->major, g->minor, g->info.asic_id, g->info.family_id,
         g->info.chip_rev, g->info.chip_external_rev, g->info.pci_rev_id,
         g->info.num_shader_engines, g->info.cu_active_number, g->address32_hi);
}
static void close_gpu(gpu *g) {
  if (g->ctx)
    amdgpu_cs_ctx_free(g->ctx);
  if (g->dev)
    amdgpu_device_deinitialize(g->dev);
  if (g->fd >= 0)
    close(g->fd);
}
static void alloc_bo_common(gpu *g, size_t requested, u32 domain, u64 align,
                            bool va32, bool cpu_access, bo *b) {
  memset(b, 0, sizeof(*b));
  b->size = align_up_u64(requested, align);
  struct amdgpu_bo_alloc_request r = {
      .alloc_size = b->size,
      .phys_alignment = align,
      .preferred_heap = domain,
      .flags = cpu_access ? AMDGPU_GEM_CREATE_CPU_ACCESS_REQUIRED : 0};
  check_ret(amdgpu_bo_alloc(g->dev, &r, &b->h), "amdgpu_bo_alloc");
  check_ret(amdgpu_va_range_alloc(g->dev, amdgpu_gpu_va_range_general, b->size,
                                  align, 0, &b->va, &b->vh,
                                  AMDGPU_VA_RANGE_HIGH |
                                      (va32 ? AMDGPU_VA_RANGE_32_BIT : 0)),
            "amdgpu_va_range_alloc");
  check_ret(amdgpu_bo_va_op(b->h, 0, b->size, b->va,
                            AMDGPU_VM_PAGE_READABLE | AMDGPU_VM_PAGE_WRITEABLE |
                                AMDGPU_VM_PAGE_EXECUTABLE,
                            AMDGPU_VA_OP_MAP),
            "amdgpu_bo_va_op");
  if (cpu_access) {
    check_ret(amdgpu_bo_cpu_map(b->h, &b->cpu), "amdgpu_bo_cpu_map");
    memset(b->cpu, 0, b->size);
  }
}
static void alloc_bo(gpu *g, size_t requested, u32 domain, u64 align, bool va32,
                     bo *b) {
  alloc_bo_common(g, requested, domain, align, va32, true, b);
}
static void alloc_device_bo(gpu *g, size_t requested, u32 domain, u64 align,
                            bool va32, bo *b) {
  alloc_bo_common(g, requested, domain, align, va32, false, b);
}
static void free_bo(bo *b) {
  if (!b->h)
    return;
  if (b->cpu)
    amdgpu_bo_cpu_unmap(b->h);
  if (b->va)
    amdgpu_bo_va_op(b->h, 0, b->size, b->va, 0, AMDGPU_VA_OP_UNMAP);
  if (b->vh)
    amdgpu_va_range_free(b->vh);
  amdgpu_bo_free(b->h);
  memset(b, 0, sizeof(*b));
}
static u8 *load(const char *path, size_t *sz) {
  int fd = open(path, O_RDONLY | O_CLOEXEC);
  if (fd < 0)
    die_errno(path);
  struct stat s;
  if (fstat(fd, &s))
    die_errno("fstat");
  if (s.st_size <= 0 || (u64)s.st_size > SIZE_MAX ||
      s.st_size % (off_t)sizeof(u32))
    dief("invalid shader size for %s: %lld", path, (long long)s.st_size);
  size_t size = (size_t)s.st_size;
  u8 *p = malloc(size);
  if (!p)
    die_errno("malloc");
  size_t n = 0;
  while (n < size) {
    ssize_t r = read(fd, p + n, size - n);
    if (r < 0)
      die_errno("read");
    if (!r)
      dief("short read: %s", path);
    n += (size_t)r;
  }
  close(fd);
  *sz = n;
  return p;
}
static void sibling_path(const char *name, char *path, size_t size) {
  char exe[4096];
  ssize_t len = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
  if (len < 0)
    die_errno("readlink /proc/self/exe");
  if ((size_t)len >= sizeof(exe) - 1)
    dief("executable path too long");
  exe[len] = '\0';
  char *slash = strrchr(exe, '/');
  if (!slash)
    dief("executable has no directory: %s", exe);
  *slash = '\0';
  if (snprintf(path, size, "%s/%s", exe, name) >= (int)size)
    dief("shader path too long");
}
static void dump_pm4(const pm4 *p) {
  for (size_t i = 0; i < p->n; i++)
    printf("PM4[%04zu] = %08x\n", i, p->v[i]);
}

static void ring_state(pm4 *p, const bo *r, u32 se) {
  /* Mesa ac_gpu_info.c Navi48 defaults: 1400 KiB rounded to 1408 KiB attribute,
   * 16384 position and 16368 primitive exports per SE.
   * ac_emit_cp_gfx11_ge_rings defines the address shifts, units and
   * all-four-register update rule. */
  const u32 attr_per_se = 1408u * 1024u, pos_per_se = 16384u * 16u,
            prim_per_se = (16368u * 4u + 31u) & ~31u;
  u64 attr_total = (u64)attr_per_se * se, square = (u64)se * se;
  u64 pos_total = align_up_u64((u64)pos_per_se * square, 65536),
      pos = r->va + attr_total, prim = pos + pos_total;
  ucfg(p, 0x444, 0x12355123);
  ucfg(p, 0x445, 0x0001544d);
  ucfg(p, 0x446, (u32)(r->va >> 16));
  ucfg(p, 0x447, ((attr_per_se >> 16) - 1) | 0x00020000);
  /* GE ring state, including the GFX12 temporal/scope bits, matches the clean
   * RADV capture. */
  ucfg(p, 0x268, (u32)(pos >> 16));
  ucfg(p, 0x269, pos_per_se >> 5);
  ucfg(p, 0x26a, (u32)(prim >> 16));
  ucfg(p, 0x26b, (prim_per_se >> 5) | 0x0c6e0000);
}

static void captured_draw_state(pm4 *p, const bo *ngg, const bo *ps,
                                const bo *color) {
  /* Register values below are from the authoritative clean RADV GFX1201 linear
   * procedural draw. Offsets are packet-relative (context 0x28000, SH 0xb000,
   * uconfig 0x30000); only BO-dependent addresses are substituted. */
  ctx(p, 0x81, 0);
  ctx(p, 0x82, 0x00ff00ff);
  ctx(p, 0x61, 0x01000100);
  u64 base = color->va >> 8;
  ctx(p, 0x318, (u32)base);
  ctx(p, 0x319, 0);
  ctx(p, 0x31a, 0);
  ctx(p, 0x31b, 0);
  ctx(p, 0x31c, 0x10000004);
  ctx(p, 0x31e, 0x00ff00ff);
  ctx(p, 0x31f, 0x01000000);
  ctx(p, 0x390, (u32)(base >> 32) & 0xff);
  ctx(p, 0x3b0, 0x0002800a);
  for (u32 i = 1; i < 8; i++)
    ctx(p, 0x3b0 + i, 0);
  ctx(p, 0x06, 0);
  ctx(p, 0x07, 0x20000000);
  ctx(p, 0x2e5, 0);
  ctx(p, 0x04, 0x08000000);
  ctx(p, 0x1d5, 5);
  ctx(p, 0x1d6, 6);
  ctx(p, 0x1d7, 0);
  ctx(p, 0x18, 0);
  ctx(p, 0x311, 0x19fc01b0);
  ctx(p, 0x206, 0);
  ctx(p, 0x2cf, 4);
  ctx(p, 0x192, 1);
  ctx(p, 0x193, 4);
  ctx(p, 0x1ff, 0x80);
  ctx(p, 0x2d3, 1);
  ucfg(p, 0x25b, 0xa0010080);
  ucfg(p, 0x262, 0);
  ctx(p, 0x198, 1);
  ctx(p, 0x190, 0);
  ctx(p, 0x2ef, 2);
  ctx(p, 0x2a6, 0x04000000);
  ctx(p, 0x2a8, 0x40);
  ctx(p, 0x215, 0xf);
  ctx(p, 0x194, 0);
  ctx(p, 0x195, 4);
  ctx(p, 0x10b, 0x43800000);
  ctx(p, 0x10c, 0x3f800000);
  ctx(p, 0x10d, 0x43800000);
  ctx(p, 0x10e, 0x3f800000);
  ctx(p, 0x8d, 0x00080008);
  ctx(p, 0x1b, 0x10);
  ctx(p, 0x83, 0xffff);
  ctx(p, 0x10f, 0x43000000);
  ctx(p, 0x110, 0x43000000);
  ctx(p, 0x111, 0x43000000);
  ctx(p, 0x112, 0x43000000);
  ctx(p, 0x113, 0x3f800000);
  ctx(p, 0x114, 0);
  ctx(p, 0x115, 0);
  ctx(p, 0x116, 0x3f800000);
  ctx(p, 0x19, 0);
  ctx(p, 0x94, 0);
  ctx(p, 0x95, 0x00ff00ff);
  ucfg_idx(p, 0x10000242, 4);
  ucfg(p, 0x266, 2);
  ctx(p, 0x282, 8);
  ctx(p, 0x283, 0x00ff0000);
  ctx(p, 0x2f7, 0);
  ctx(p, 0x204, 0x01080000);
  ctx(p, 0x291, 2);
  ctx(p, 0x207, 0x240);
  ctx(p, 0x03, 0x1000);
  ctx(p, 0x1c, 0x00700770);
  for (u32 i = 0; i < 8; i++)
    ctx(p, 0x1e0 + i, 0);
  for (u32 i = 0; i < 8; i++)
    ctx(p, 0x1d8 + i, 0x06000600);
  ctx(p, 0x214, 0xf);
  ctx(p, 0x216, 0x00cc0010);
  ctx(p, 0x30e, 0xffffffff);
  ctx(p, 0x30f, 0xffffffff);
  ctx(p, 0x2f8, 0);
  ctx(p, 0x292, 0x22);
  ctx(p, 0x1f, 0x00018700);
  ctx(p, 0x317, 0);
  ctx(p, 0x1e, 0x00110000);
  ctx(p, 0x315, 0x00100000);
  ucfg(p, 0x263, 0);
  ctx(p, 0x212, 0);
  ctx(p, 0x2fe, 0);
  ctx(p, 0x302, 0);
  ctx(p, 0x306, 0);
  ctx(p, 0x30a, 0);
  ctx(p, 0x2fc, 0);
  ctx(p, 0x2fd, 0);
  ctx(p, 0x196, 0);
  ctx(p, 0x293, 0x060201bc);
  ctx(p, 0xf4, 0x51);
  ctx(p, 0x197, 1);
  u64 n = ngg->va >> 8, q = ps->va >> 8;
  sh(p, 0x89, (u32)n);
  sh(p, 0x86, (u32)(ngg->va >> 40) & 0xff);
  sh(p, 0x8a, 0x000c0002);
  sh(p, 0x8b, 4);
  sh(p, 0x88, 0x01ff0bff);
  sh(p, 0x8d, 0);
  sh(p, 0x08, (u32)q);
  sh(p, 0x09, (u32)(ps->va >> 40) & 0xff);
  sh(p, 0x0a, 0x000c0002);
  sh(p, 0x0b, 4);
  sh(p, 0x07, 0x000207ff);
  sh(p, 0x31, 0x400);
}
static void submit_gfx(gpu *g, pm4 *p, bo *ib, bo **res, u32 nr) {
  alloc_bo(g, p->n * 4, AMDGPU_GEM_DOMAIN_GTT, 4096, true, ib);
  memcpy(ib->cpu, p->v, p->n * 4);
  amdgpu_bo_handle *hs = calloc(nr + 1, sizeof(*hs));
  hs[0] = ib->h;
  for (u32 i = 0; i < nr; i++)
    hs[i + 1] = res[i]->h;
  amdgpu_bo_list_handle list;
  check_ret(amdgpu_bo_list_create(g->dev, nr + 1, hs, NULL, &list),
            "amdgpu_bo_list_create(GFX)");
  free(hs);
  struct amdgpu_cs_ib_info ii = {.ib_mc_address = ib->va, .size = (u32)p->n};
  struct amdgpu_cs_request rq = {.ip_type = AMDGPU_HW_IP_GFX,
                                 .resources = list,
                                 .number_of_ibs = 1,
                                 .ibs = &ii};
  check_ret(amdgpu_cs_submit(g->ctx, 0, &rq, 1), "amdgpu_cs_submit(GFX)");
  struct amdgpu_cs_fence f = {
      .context = g->ctx, .ip_type = AMDGPU_HW_IP_GFX, .fence = rq.seq_no};
  u32 done = 0;
  check_ret(amdgpu_cs_query_fence_status(&f, FENCE_NS, 0, &done), "GFX fence");
  if (!done)
    dief("GFX fence timed out after 5 seconds");
  amdgpu_bo_list_destroy(list);
}
static void sdma_copy(gpu *g, const bo *dst, const bo *src, u32 bytes) {
  u32 pkt[7] = {1,
                bytes - 1,
                0,
                (u32)src->va,
                (u32)(src->va >> 32),
                (u32)dst->va,
                (u32)(dst->va >> 32)};
  bo ib;
  alloc_bo(g, sizeof(pkt), AMDGPU_GEM_DOMAIN_GTT, 4096, true, &ib);
  memcpy(ib.cpu, pkt, sizeof(pkt));
  amdgpu_bo_handle hs[3] = {ib.h, src->h, dst->h};
  amdgpu_bo_list_handle list;
  check_ret(amdgpu_bo_list_create(g->dev, 3, hs, NULL, &list),
            "amdgpu_bo_list_create(SDMA)");
  struct amdgpu_cs_ib_info ii = {.ib_mc_address = ib.va, .size = 7};
  struct amdgpu_cs_request rq = {.ip_type = AMDGPU_HW_IP_DMA,
                                 .resources = list,
                                 .number_of_ibs = 1,
                                 .ibs = &ii};
  check_ret(amdgpu_cs_submit(g->ctx, 0, &rq, 1), "amdgpu_cs_submit(SDMA)");
  struct amdgpu_cs_fence f = {
      .context = g->ctx, .ip_type = AMDGPU_HW_IP_DMA, .fence = rq.seq_no};
  u32 done = 0;
  check_ret(amdgpu_cs_query_fence_status(&f, FENCE_NS, 0, &done), "SDMA fence");
  if (!done)
    dief("SDMA fence timed out after 5 seconds");
  amdgpu_bo_list_destroy(list);
  free_bo(&ib);
}
static void scan(const u32 *p) {
  u32 count = 0, first = 0, minx = WIDTH, miny = HEIGHT, maxx = 0, maxy = 0;
  for (u32 y = 0; y < HEIGHT; y++)
    for (u32 x = 0; x < WIDTH; x++) {
      u32 v = p[y * WIDTH + x];
      if (!v)
        continue;
      if (!first)
        first = v;
      count++;
      if (x < minx)
        minx = x;
      if (y < miny)
        miny = y;
      if (x > maxx)
        maxx = x;
      if (y > maxy)
        maxy = y;
    }
  u32 center = p[128 * WIDTH + 128];
  printf("readback: center=0x%08x first=0x%08x nonzero=%u bbox=%u,%u..%u,%u\n",
         center, first, count, minx, miny, maxx, maxy);
  if (center != 0xff0000ff || first != 0xff0000ff || count != 18432 ||
      minx != 32 || miny != 33 || maxx != 223 || maxy != 223)
    dief("unexpected GFX1201 triangle readback");
}
int main(int argc, char **argv) {
  options o = parse(argc, argv);
  gpu g = {.fd = -1};
  open_gpu(&g, o.node);
  if (g.info.asic_id != 0x7550)
    dief("expected Navi48/GFX1201 ASIC 0x7550, got 0x%04x", g.info.asic_id);
  if (o.stop == STAGE_INFO) {
    close_gpu(&g);
    return 0;
  }
  u32 se = g.info.num_shader_engines ? g.info.num_shader_engines : 4;
  u64 attr = (u64)1408 * 1024 * se,
      pos = align_up_u64((u64)(16384 * 16) * se * se, 65536),
      prim = align_up_u64((u64)(((16368 * 4 + 31) & ~31)) * se * se, 65536);
  bo rings = {0}, ngg = {0}, ps = {0}, color = {0}, readback = {0}, ib = {0};
  pm4 p = {0};
  alloc_device_bo(&g, attr + pos + prim, AMDGPU_GEM_DOMAIN_VRAM, 65536, true,
                  &rings);
  emit(&p, PKT3(0x28, 1));
  emit(&p, 0x80000000);
  emit(&p, 0x80000000);
  acquire(&p, 0, 0x0000c181);
  ring_state(&p, &rings, se);
  if (o.stop == STAGE_RINGS) {
    if (o.dump)
      dump_pm4(&p);
    bo *rs[] = {&rings};
    submit_gfx(&g, &p, &ib, rs, 1);
    printf("ring stage completed: VA=0x%llx bytes=%zu PM4=%zu dwords\n",
           (unsigned long long)rings.va, rings.size, p.n);
    goto done;
  }
  char ngg_path[4096], ps_path[4096];
  sibling_path("gfx12-raw-triangle-ngg.bin", ngg_path, sizeof(ngg_path));
  sibling_path("gfx12-raw-triangle-ps.bin", ps_path, sizeof(ps_path));
  size_t ns, pss;
  u8 *nc = load(ngg_path, &ns), *pc = load(ps_path, &pss);
  alloc_bo(&g, ns, AMDGPU_GEM_DOMAIN_VRAM, 256, true, &ngg);
  alloc_bo(&g, pss, AMDGPU_GEM_DOMAIN_VRAM, 256, true, &ps);
  alloc_bo(&g, COLOR_BYTES, AMDGPU_GEM_DOMAIN_VRAM, 256, true, &color);
  alloc_bo(&g, COLOR_BYTES, AMDGPU_GEM_DOMAIN_GTT, 4096, true, &readback);
  memcpy(ngg.cpu, nc, ns);
  memcpy(ps.cpu, pc, pss);
  free(nc);
  free(pc);
  captured_draw_state(&p, &ngg, &ps, &color);
  release(&p, 0x80404514);
  acquire(&p, 0x00022000, 0x00010000);
  emit(&p, PKT3(0x2f, 0));
  emit(&p, 1);
  sh(&p, 0x8c, 0);
  emit(&p, PKT3(0x2d, 1));
  emit(&p, 3);
  emit(&p, 2);
  release(&p, 0x80404514);
  if (o.dump)
    dump_pm4(&p);
  bo *rs[] = {&rings, &ngg, &ps, &color};
  printf("submitting GFX1201 DRAW_INDEX_AUTO: %zu PM4 dwords\n", p.n);
  submit_gfx(&g, &p, &ib, rs, 4);
  sdma_copy(&g, &readback, &color, COLOR_BYTES);
  scan(readback.cpu);
done:
  free_bo(&ib);
  free_bo(&readback);
  free_bo(&color);
  free_bo(&ps);
  free_bo(&ngg);
  free_bo(&rings);
  free(p.v);
  close_gpu(&g);
  return 0;
}
