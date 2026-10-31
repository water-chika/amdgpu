#define _GNU_SOURCE
#include "../src/common.h"

#include <inttypes.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <vulkan/vulkan.h>

#define RECORD_WORDS 8u
#define RECORD_COUNT 3u
#define POISON 0xcdcdcdcdu

typedef struct { VkBuffer buffer; VkDeviceMemory memory; VkDeviceAddress address; VkDeviceSize size; } buffer;
typedef struct { VkAccelerationStructureKHR as; buffer storage; VkDeviceAddress address; } accel;

static void vk_check(VkResult r, const char *what) { if (r != VK_SUCCESS) dief("%s failed: VkResult %d", what, r); }

static uint32_t memory_type(VkPhysicalDevice p, uint32_t bits, VkMemoryPropertyFlags wanted) {
  VkPhysicalDeviceMemoryProperties m;
  vkGetPhysicalDeviceMemoryProperties(p, &m);
  for (uint32_t i = 0; i < m.memoryTypeCount; ++i)
    if ((bits & (1u << i)) && (m.memoryTypes[i].propertyFlags & wanted) == wanted) return i;
  dief("no Vulkan memory type with flags 0x%x", wanted);
  return 0;
}

static buffer make_buffer_aligned(VkPhysicalDevice p, VkDevice d, VkDeviceSize size,
                                  VkDeviceSize alignment, VkBufferUsageFlags usage,
                                  VkMemoryPropertyFlags properties) {
  VkDeviceSize allocation_size = size + (alignment ? alignment - 1 : 0);
  buffer b = {.size = size};
  VkBufferCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = allocation_size,
                           .usage = usage, .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
  vk_check(vkCreateBuffer(d, &ci, NULL, &b.buffer), "vkCreateBuffer");
  VkMemoryRequirements req;
  vkGetBufferMemoryRequirements(d, b.buffer, &req);
  VkMemoryAllocateFlagsInfo flags = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO,
                                     .flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT};
  if (alignment > req.alignment) req.alignment = alignment;
  VkMemoryAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                             .pNext = (usage & VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT) ? &flags : NULL,
                             .allocationSize = align_up_u64(req.size, req.alignment),
                             .memoryTypeIndex = memory_type(p, req.memoryTypeBits, properties)};
  vk_check(vkAllocateMemory(d, &ai, NULL, &b.memory), "vkAllocateMemory");
  vk_check(vkBindBufferMemory(d, b.buffer, b.memory, 0), "vkBindBufferMemory");
  if (usage & VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT) {
    VkBufferDeviceAddressInfo bai = {.sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO, .buffer = b.buffer};
    b.address = vkGetBufferDeviceAddress(d, &bai);
  }
  if (alignment && b.address) b.address = align_up_u64(b.address, alignment);
  return b;
}

static buffer make_buffer(VkPhysicalDevice p, VkDevice d, VkDeviceSize size,
                          VkBufferUsageFlags usage, VkMemoryPropertyFlags properties) {
  return make_buffer_aligned(p, d, size, 0, usage, properties);
}

static void put(VkDevice d, buffer b, const void *data, size_t size) {
  void *map;
  vk_check(vkMapMemory(d, b.memory, 0, size, 0, &map), "vkMapMemory");
  memcpy(map, data, size);
  vkUnmapMemory(d, b.memory);
}

static bool diagnostic_env_enabled(void) {
  const char *value = getenv("RADV_RAY_QUERY_DIAGNOSTIC");
  return value && value[0] && strcmp(value, "0") != 0;
}

static void print_buffer_evidence(const char *role, buffer b) {
  printf("diagnostic buffer role=%s address=0x%016" PRIx64 " size=0x%016" PRIx64 "\n",
         role, (uint64_t)b.address, (uint64_t)b.size);
}

static void print_as_bytes(const char *role, const unsigned char *bytes, VkDeviceSize size) {
  for (VkDeviceSize block = 0; block < size; block += 128) {
    VkDeviceSize valid = size - block < 128 ? size - block : 128;
    printf("diagnostic as-block role=%s offset=0x%08" PRIx64 " bytes=0x%04" PRIx64 "\n",
           role, (uint64_t)block, (uint64_t)valid);
    for (VkDeviceSize row = 0; row < valid; row += 16) {
      VkDeviceSize count = valid - row < 16 ? valid - row : 16;
      printf("  %08" PRIx64 ":", (uint64_t)(block + row));
      for (VkDeviceSize i = 0; i < count; ++i) printf(" %02x", bytes[block + row + i]);
      putchar('\n');
    }
  }
}

static void submit_wait(VkDevice d, VkQueue q, VkCommandBuffer cmd, const char *what) {
  vk_check(vkEndCommandBuffer(cmd), "vkEndCommandBuffer");
  VkFenceCreateInfo fci = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
  VkFence fence;
  vk_check(vkCreateFence(d, &fci, NULL, &fence), "vkCreateFence");
  VkSubmitInfo si = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cmd};
  vk_check(vkQueueSubmit(q, 1, &si, fence), what);
  vk_check(vkWaitForFences(d, 1, &fence, VK_TRUE, 10000000000ull), "vkWaitForFences(10s)");
  vkDestroyFence(d, fence, NULL);
}

static VkCommandBuffer begin_cmd(VkDevice d, VkCommandPool pool) {
  VkCommandBufferAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
                                    .commandPool = pool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
                                    .commandBufferCount = 1};
  VkCommandBuffer cmd;
  vk_check(vkAllocateCommandBuffers(d, &ai, &cmd), "vkAllocateCommandBuffers");
  VkCommandBufferBeginInfo bi = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                                 .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
  vk_check(vkBeginCommandBuffer(cmd, &bi), "vkBeginCommandBuffer");
  return cmd;
}

static unsigned char *read_file(const char *path, size_t *size) {
  FILE *f = fopen(path, "rb");
  if (!f) die_errno(path);
  if (fseek(f, 0, SEEK_END) || (*size = (size_t)ftell(f), fseek(f, 0, SEEK_SET))) die_errno(path);
  unsigned char *p = malloc(*size);
  if (!p) die_errno("malloc shader");
  if (fread(p, 1, *size, f) != *size) die_errno(path);
  fclose(f);
  return p;
}

static void sibling_path(const char *name, char *path, size_t size) {
  char exe[4096];
  ssize_t len = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
  if (len < 0) die_errno("readlink /proc/self/exe");
  if ((size_t)len >= sizeof(exe) - 1) dief("executable path too long");
  exe[len] = '\0';
  char *slash = strrchr(exe, '/');
  if (!slash) dief("executable has no directory: %s", exe);
  *slash = '\0';
  if (snprintf(path, size, "%s/%s", exe, name) >= (int)size) dief("shader path too long");
}

int main(int argc, char **argv) {
  char default_spv[4096];
  sibling_path("radv-ray-query.comp.spv", default_spv, sizeof(default_spv));
  bool diagnostic = diagnostic_env_enabled();
  const char *spv = default_spv;
  for (int i = 1; i < argc; ++i) {
    if (strcmp(argv[i], "--diagnostic") == 0) diagnostic = true;
    else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
      printf("usage: %s [--diagnostic] [shader.spv]\n", argv[0]);
      return 0;
    } else if (argv[i][0] == '-') dief("unknown option: %s", argv[i]);
    else if (spv == default_spv) spv = argv[i];
    else dief("usage: %s [--diagnostic] [shader.spv]", argv[0]);
  }
  VkApplicationInfo app = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .pApplicationName = "RADV Ray Query",
                           .apiVersion = VK_API_VERSION_1_2};
  VkInstanceCreateInfo ici = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app};
  VkInstance instance;
  vk_check(vkCreateInstance(&ici, NULL, &instance), "vkCreateInstance");

  uint32_t count = 0;
  vk_check(vkEnumeratePhysicalDevices(instance, &count, NULL), "vkEnumeratePhysicalDevices(count)");
  VkPhysicalDevice *list = calloc(count, sizeof(*list));
  if (!list) die_errno("calloc devices");
  vk_check(vkEnumeratePhysicalDevices(instance, &count, list), "vkEnumeratePhysicalDevices(list)");
  VkPhysicalDevice physical = VK_NULL_HANDLE;
  VkPhysicalDeviceProperties props = {0};
  VkPhysicalDeviceDriverProperties driver = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES};
  VkPhysicalDeviceAccelerationStructurePropertiesKHR as_props = {
    .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_PROPERTIES_KHR,
    .pNext = &driver,
  };
  for (uint32_t i = 0; i < count; ++i) {
    VkPhysicalDeviceProperties p;
    vkGetPhysicalDeviceProperties(list[i], &p);
    if (p.vendorID == 0x1002 && p.deviceID == 0x7550) { physical = list[i]; props = p; break; }
  }
  free(list);
  if (!physical) dief("required AMD RX9070 device 1002:7550 not found");
  VkPhysicalDeviceProperties2 props2 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = &as_props};
  vkGetPhysicalDeviceProperties2(physical, &props2);
  if (strcmp(driver.driverName, "radv") != 0) dief("1002:7550 is using '%s', expected radv", driver.driverName);
  printf("device: %s (1002:%04x)\ndriver: %s - %s\napi: %u.%u.%u\n", props.deviceName, props.deviceID,
         driver.driverName, driver.driverInfo, VK_API_VERSION_MAJOR(props.apiVersion),
         VK_API_VERSION_MINOR(props.apiVersion), VK_API_VERSION_PATCH(props.apiVersion));

  uint32_t qcount = 0;
  vkGetPhysicalDeviceQueueFamilyProperties(physical, &qcount, NULL);
  VkQueueFamilyProperties *qp = calloc(qcount, sizeof(*qp));
  vkGetPhysicalDeviceQueueFamilyProperties(physical, &qcount, qp);
  uint32_t qfam = UINT32_MAX;
  for (uint32_t i = 0; i < qcount; ++i) if (qp[i].queueFlags & VK_QUEUE_COMPUTE_BIT) { qfam = i; break; }
  free(qp);
  if (qfam == UINT32_MAX) dief("no compute queue");

  VkPhysicalDeviceBufferDeviceAddressFeatures bda = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BUFFER_DEVICE_ADDRESS_FEATURES};
  VkPhysicalDeviceAccelerationStructureFeaturesKHR asf = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR, .pNext = &bda};
  VkPhysicalDeviceRayQueryFeaturesKHR rq = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_QUERY_FEATURES_KHR, .pNext = &asf};
  VkPhysicalDeviceFeatures2 features = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &rq};
  vkGetPhysicalDeviceFeatures2(physical, &features);
  if (!bda.bufferDeviceAddress || !asf.accelerationStructure || !rq.rayQuery)
    dief("required BDA/acceleration-structure/ray-query features unavailable");
  bda.bufferDeviceAddress = asf.accelerationStructure = rq.rayQuery = VK_TRUE;
  const char *exts[] = {VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME, VK_KHR_RAY_QUERY_EXTENSION_NAME,
                        VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME};
  float priority = 1.0f;
  VkDeviceQueueCreateInfo qci = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
                                 .queueFamilyIndex = qfam, .queueCount = 1, .pQueuePriorities = &priority};
  VkDeviceCreateInfo dci = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .pNext = &rq,
                            .queueCreateInfoCount = 1, .pQueueCreateInfos = &qci,
                            .enabledExtensionCount = 3, .ppEnabledExtensionNames = exts};
  VkDevice device;
  vk_check(vkCreateDevice(physical, &dci, NULL, &device), "vkCreateDevice");
  VkQueue queue;
  vkGetDeviceQueue(device, qfam, 0, &queue);
#define LOAD(name) PFN_##name name = (PFN_##name)vkGetDeviceProcAddr(device, #name); if (!name) dief("missing " #name)
  LOAD(vkCreateAccelerationStructureKHR);
  LOAD(vkDestroyAccelerationStructureKHR);
  LOAD(vkGetAccelerationStructureBuildSizesKHR);
  LOAD(vkGetAccelerationStructureDeviceAddressKHR);
  LOAD(vkCmdBuildAccelerationStructuresKHR);
#undef LOAD
  VkCommandPoolCreateInfo cpci = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, .queueFamilyIndex = qfam,
                                   .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT};
  VkCommandPool pool;
  vk_check(vkCreateCommandPool(device, &cpci, NULL, &pool), "vkCreateCommandPool");

  const float vertices[9] = {-1,-1,0, 1,-1,0, 0,1,0};
  buffer vertex = make_buffer(physical, device, sizeof(vertices),
      VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
  put(device, vertex, vertices, sizeof(vertices));
  VkAccelerationStructureGeometryTrianglesDataKHR triangles = {
    .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR,
    .vertexFormat = VK_FORMAT_R32G32B32_SFLOAT, .vertexData.deviceAddress = vertex.address,
    .vertexStride = 12, .maxVertex = 2, .indexType = VK_INDEX_TYPE_NONE_KHR};
  VkAccelerationStructureGeometryKHR blas_geom = {.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR,
    .geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR, .geometry.triangles = triangles,
    .flags = VK_GEOMETRY_OPAQUE_BIT_KHR};
  VkAccelerationStructureBuildGeometryInfoKHR build = {.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR,
    .type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR, .flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR,
    .mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR, .geometryCount = 1, .pGeometries = &blas_geom};
  uint32_t primitive_count = 1;
  VkAccelerationStructureBuildSizesInfoKHR sizes = {.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR};
  vkGetAccelerationStructureBuildSizesKHR(device, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &build, &primitive_count, &sizes);
  accel blas = {0};
  blas.storage = make_buffer(physical, device, sizes.accelerationStructureSize,
      VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT |
          (diagnostic ? VK_BUFFER_USAGE_TRANSFER_SRC_BIT : 0),
      VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
  VkAccelerationStructureCreateInfoKHR asci = {.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR,
    .buffer = blas.storage.buffer, .size = sizes.accelerationStructureSize, .type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR};
  vk_check(vkCreateAccelerationStructureKHR(device, &asci, NULL, &blas.as), "vkCreateAccelerationStructureKHR(BLAS)");
  buffer scratch = make_buffer_aligned(physical, device, sizes.buildScratchSize,
      as_props.minAccelerationStructureScratchOffsetAlignment,
      VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
  build.dstAccelerationStructure = blas.as; build.scratchData.deviceAddress = scratch.address;
  VkAccelerationStructureBuildRangeInfoKHR range = {.primitiveCount = 1};
  const VkAccelerationStructureBuildRangeInfoKHR *ranges[] = {&range};
  VkCommandBuffer cmd = begin_cmd(device, pool);
  vkCmdBuildAccelerationStructuresKHR(cmd, 1, &build, ranges);
  submit_wait(device, queue, cmd, "vkQueueSubmit(BLAS)");
  VkAccelerationStructureDeviceAddressInfoKHR addr_info = {.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR, .accelerationStructure = blas.as};
  blas.address = vkGetAccelerationStructureDeviceAddressKHR(device, &addr_info);

  VkAccelerationStructureInstanceKHR inst = {.transform = {{{1,0,0,0},{0,1,0,0},{0,0,1,0}}},
    .instanceCustomIndex = 7, .mask = 0xff, .instanceShaderBindingTableRecordOffset = 0,
    .flags = VK_GEOMETRY_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT_KHR,
    .accelerationStructureReference = blas.address};
  buffer instances = make_buffer(physical, device, sizeof(inst),
      VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
  put(device, instances, &inst, sizeof(inst));
  VkAccelerationStructureGeometryInstancesDataKHR instance_data = {.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR,
    .arrayOfPointers = VK_FALSE, .data.deviceAddress = instances.address};
  VkAccelerationStructureGeometryKHR tlas_geom = {.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR,
    .geometryType = VK_GEOMETRY_TYPE_INSTANCES_KHR, .geometry.instances = instance_data};
  build.type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR; build.pGeometries = &tlas_geom;
  vkGetAccelerationStructureBuildSizesKHR(device, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &build, &primitive_count, &sizes);
  accel tlas = {0};
  tlas.storage = make_buffer(physical, device, sizes.accelerationStructureSize,
      VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT |
          (diagnostic ? VK_BUFFER_USAGE_TRANSFER_SRC_BIT : 0), VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
  asci.buffer = tlas.storage.buffer; asci.size = sizes.accelerationStructureSize; asci.type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
  vk_check(vkCreateAccelerationStructureKHR(device, &asci, NULL, &tlas.as), "vkCreateAccelerationStructureKHR(TLAS)");
  buffer tlas_scratch = make_buffer_aligned(physical, device, sizes.buildScratchSize,
      as_props.minAccelerationStructureScratchOffsetAlignment,
      VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
  build.dstAccelerationStructure = tlas.as; build.scratchData.deviceAddress = tlas_scratch.address;
  cmd = begin_cmd(device, pool);
  VkMemoryBarrier blas_to_tlas = {
    .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
    .srcAccessMask = VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR,
    .dstAccessMask = VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR,
  };
  vkCmdPipelineBarrier(cmd,
      VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
      VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
      0, 1, &blas_to_tlas, 0, NULL, 0, NULL);
  vkCmdBuildAccelerationStructuresKHR(cmd, 1, &build, ranges);
  submit_wait(device, queue, cmd, "vkQueueSubmit(TLAS)");
  addr_info.accelerationStructure = tlas.as;
  tlas.address = vkGetAccelerationStructureDeviceAddressKHR(device, &addr_info);

  buffer diagnostic_capture = {0};
  if (diagnostic) {
    VkDeviceSize blas_offset = 0;
    VkDeviceSize tlas_offset = align_up_u64(blas.storage.size, 16);
    diagnostic_capture = make_buffer(physical, device, tlas_offset + tlas.storage.size,
        VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    cmd = begin_cmd(device, pool);
    VkBufferMemoryBarrier capture_barriers[2] = {
      {.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
       .srcAccessMask = VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR,
       .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
       .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
       .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
       .buffer = blas.storage.buffer, .offset = 0, .size = blas.storage.size},
      {.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
       .srcAccessMask = VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR,
       .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
       .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
       .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
       .buffer = tlas.storage.buffer, .offset = 0, .size = tlas.storage.size},
    };
    vkCmdPipelineBarrier(cmd,
        VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
        VK_PIPELINE_STAGE_TRANSFER_BIT,
        0, 0, NULL, 2, capture_barriers, 0, NULL);
    VkBufferCopy blas_copy = {.srcOffset = 0, .dstOffset = blas_offset, .size = blas.storage.size};
    VkBufferCopy tlas_copy = {.srcOffset = 0, .dstOffset = tlas_offset, .size = tlas.storage.size};
    vkCmdCopyBuffer(cmd, blas.storage.buffer, diagnostic_capture.buffer, 1, &blas_copy);
    vkCmdCopyBuffer(cmd, tlas.storage.buffer, diagnostic_capture.buffer, 1, &tlas_copy);
    submit_wait(device, queue, cmd, "vkQueueSubmit(AS diagnostic capture)");

    print_buffer_evidence("vertex-build-input", vertex);
    print_buffer_evidence("blas-storage", blas.storage);
    print_buffer_evidence("blas-scratch", scratch);
    print_buffer_evidence("instance-build-input", instances);
    print_buffer_evidence("tlas-storage", tlas.storage);
    print_buffer_evidence("tlas-scratch", tlas_scratch);
    printf("diagnostic as-address role=blas address=0x%016" PRIx64 "\n", (uint64_t)blas.address);
    printf("diagnostic as-address role=tlas address=0x%016" PRIx64 "\n", (uint64_t)tlas.address);
    printf("diagnostic descriptor binding=0 type=acceleration-structure address=0x%016" PRIx64 "\n",
           (uint64_t)tlas.address);
    printf("diagnostic instance custom-index=0x%06x mask=0x%02x sbt-offset=0x%06x flags=0x%02x reference=0x%016" PRIx64 "\n",
           inst.instanceCustomIndex, inst.mask, inst.instanceShaderBindingTableRecordOffset, inst.flags,
           (uint64_t)inst.accelerationStructureReference);
    printf("diagnostic capture role=as-staging size=0x%016" PRIx64
           " blas-offset=0x%016" PRIx64 " tlas-offset=0x%016" PRIx64 "\n",
           (uint64_t)diagnostic_capture.size, (uint64_t)blas_offset, (uint64_t)tlas_offset);
    unsigned char *capture_bytes;
    vk_check(vkMapMemory(device, diagnostic_capture.memory, 0, diagnostic_capture.size, 0,
                         (void **)&capture_bytes), "vkMapMemory(AS diagnostic staging)");
    print_as_bytes("blas-storage", capture_bytes + blas_offset, blas.storage.size);
    print_as_bytes("tlas-storage", capture_bytes + tlas_offset, tlas.storage.size);
    vkUnmapMemory(device, diagnostic_capture.memory);
  }

  uint32_t initial[RECORD_COUNT * RECORD_WORDS];
  for (size_t i = 0; i < sizeof(initial)/sizeof(initial[0]); ++i) initial[i] = POISON;
  buffer output = make_buffer(physical, device, sizeof(initial), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
      (diagnostic ? VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT : 0),
      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
  put(device, output, initial, sizeof(initial));
  if (diagnostic) {
    print_buffer_evidence("query-output", output);
    printf("diagnostic descriptor binding=1 type=storage-buffer address=0x%016" PRIx64
           " offset=0x%016x range=0x%016zx\n",
           (uint64_t)output.address, 0, sizeof(initial));
  }
  VkDescriptorSetLayoutBinding bindings[2] = {
    {.binding=0, .descriptorType=VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, .descriptorCount=1, .stageFlags=VK_SHADER_STAGE_COMPUTE_BIT},
    {.binding=1, .descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .descriptorCount=1, .stageFlags=VK_SHADER_STAGE_COMPUTE_BIT}};
  VkDescriptorSetLayoutCreateInfo dlci = {.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount=2, .pBindings=bindings};
  VkDescriptorSetLayout dsl; vk_check(vkCreateDescriptorSetLayout(device, &dlci, NULL, &dsl), "vkCreateDescriptorSetLayout");
  VkPipelineLayoutCreateInfo plci = {.sType=VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount=1, .pSetLayouts=&dsl};
  VkPipelineLayout layout; vk_check(vkCreatePipelineLayout(device, &plci, NULL, &layout), "vkCreatePipelineLayout");
  VkDescriptorPoolSize ps[2] = {{VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR,1},{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,1}};
  VkDescriptorPoolCreateInfo dpci = {.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,.maxSets=1,.poolSizeCount=2,.pPoolSizes=ps};
  VkDescriptorPool dp; vk_check(vkCreateDescriptorPool(device,&dpci,NULL,&dp),"vkCreateDescriptorPool");
  VkDescriptorSetAllocateInfo dsai = {.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,.descriptorPool=dp,.descriptorSetCount=1,.pSetLayouts=&dsl};
  VkDescriptorSet ds; vk_check(vkAllocateDescriptorSets(device,&dsai,&ds),"vkAllocateDescriptorSets");
  VkWriteDescriptorSetAccelerationStructureKHR as_write = {.sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR,.accelerationStructureCount=1,.pAccelerationStructures=&tlas.as};
  VkDescriptorBufferInfo obi = {.buffer=output.buffer,.range=sizeof(initial)};
  VkWriteDescriptorSet writes[2] = {{.sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,.pNext=&as_write,.dstSet=ds,.dstBinding=0,.descriptorCount=1,.descriptorType=VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR},
    {.sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,.dstSet=ds,.dstBinding=1,.descriptorCount=1,.descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,.pBufferInfo=&obi}};
  vkUpdateDescriptorSets(device,2,writes,0,NULL);
  size_t code_size; unsigned char *code = read_file(spv,&code_size);
  if ((code_size & 3) != 0) dief("SPIR-V size is not a multiple of four");
  VkShaderModuleCreateInfo smci = {.sType=VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,.codeSize=code_size,.pCode=(const uint32_t *)code};
  VkShaderModule shader; vk_check(vkCreateShaderModule(device,&smci,NULL,&shader),"vkCreateShaderModule"); free(code);
  VkComputePipelineCreateInfo pci = {.sType=VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
    .stage={.sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,.stage=VK_SHADER_STAGE_COMPUTE_BIT,.module=shader,.pName="main"},.layout=layout};
  VkPipeline pipeline; vk_check(vkCreateComputePipelines(device,VK_NULL_HANDLE,1,&pci,NULL,&pipeline),"vkCreateComputePipelines");
  cmd=begin_cmd(device,pool);
  VkMemoryBarrier tlas_to_query = {
    .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
    .srcAccessMask = VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR,
    .dstAccessMask = VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR,
  };
  vkCmdPipelineBarrier(cmd,
      VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
      VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
      0, 1, &tlas_to_query, 0, NULL, 0, NULL);
  vkCmdBindPipeline(cmd,VK_PIPELINE_BIND_POINT_COMPUTE,pipeline);
  vkCmdBindDescriptorSets(cmd,VK_PIPELINE_BIND_POINT_COMPUTE,layout,0,1,&ds,0,NULL); vkCmdDispatch(cmd,3,1,1);
  submit_wait(device,queue,cmd,"vkQueueSubmit(ray queries)");

  uint32_t *got; vk_check(vkMapMemory(device,output.memory,0,sizeof(initial),0,(void **)&got),"vkMapMemory(results)");
  const uint32_t expected[24] = {
    1,0x3f800000,0x3e800000,0x3f000000,7,0,0,0,
    1,0x40000000,0x3e000000,0x3e800000,7,0,0,0,
    0,0xffffffff,0xffffffff,0xffffffff,0xffffffff,0xffffffff,0xffffffff,0xffffffff};
  bool pass=true;
  for (uint32_t r=0;r<RECORD_COUNT;++r) {
    printf("row %u:",r); for(uint32_t w=0;w<RECORD_WORDS;++w) printf(" %08x",got[r*RECORD_WORDS+w]); putchar('\n');
    for(uint32_t w=0;w<RECORD_WORDS;++w) if(got[r*RECORD_WORDS+w]!=expected[r*RECORD_WORDS+w]) {
      fprintf(stderr,"mismatch row %u word %u: got %08x expected %08x\n",r,w,got[r*RECORD_WORDS+w],expected[r*RECORD_WORDS+w]); pass=false;
    }
  }
  vkUnmapMemory(device,output.memory); printf("%s\n",pass?"PASS":"FAIL");
  vkDeviceWaitIdle(device);
  vkDestroyPipeline(device,pipeline,NULL); vkDestroyShaderModule(device,shader,NULL); vkDestroyDescriptorPool(device,dp,NULL);
  vkDestroyPipelineLayout(device,layout,NULL); vkDestroyDescriptorSetLayout(device,dsl,NULL);
  vkDestroyAccelerationStructureKHR(device,tlas.as,NULL); vkDestroyAccelerationStructureKHR(device,blas.as,NULL);
#define FREE_BUFFER(b) do { vkDestroyBuffer(device,(b).buffer,NULL); vkFreeMemory(device,(b).memory,NULL); } while(0)
  FREE_BUFFER(output); if (diagnostic) FREE_BUFFER(diagnostic_capture);
  FREE_BUFFER(tlas_scratch); FREE_BUFFER(tlas.storage); FREE_BUFFER(instances);
  FREE_BUFFER(scratch); FREE_BUFFER(blas.storage); FREE_BUFFER(vertex);
#undef FREE_BUFFER
  vkDestroyCommandPool(device,pool,NULL); vkDestroyDevice(device,NULL); vkDestroyInstance(instance,NULL);
  return pass ? 0 : 1;
}
