#define _GNU_SOURCE

#include "../src/common.h"

#include <inttypes.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>
#include <vulkan/vulkan.h>

#define WIDTH 256u
#define HEIGHT 256u

typedef struct {
  u32 *data;
  size_t count;
  size_t capacity;
} words;

typedef struct {
  VkBuffer buffer;
  VkDeviceMemory memory;
} buffer;

typedef struct {
  VkImage image;
  VkDeviceMemory memory;
  VkImageView view;
} image;

static void vk_check(VkResult result, const char *what) {
  if (result != VK_SUCCESS) dief("%s failed: VkResult %d", what, result);
}

static void wpush(words *w, u32 value) {
  if (w->count == w->capacity) {
    size_t next_capacity = w->capacity ? w->capacity * 2 : 128;
    u32 *next = realloc(w->data, next_capacity * sizeof(*next));
    if (!next) die_errno("realloc spirv");
    w->data = next;
    w->capacity = next_capacity;
  }
  w->data[w->count++] = value;
}

static void inst(words *w, u16 opcode, u16 word_count) {
  wpush(w, ((u32)word_count << 16) | opcode);
}

static void string_words(words *w, const char *s) {
  u32 word = 0;
  unsigned shift = 0;
  for (;;) {
    unsigned char c = (unsigned char)*s++;
    word |= (u32)c << shift;
    shift += 8;
    if (!c) {
      wpush(w, word);
      break;
    }
    if (shift == 32) {
      wpush(w, word);
      word = 0;
      shift = 0;
    }
  }
}

static void spirv_header(words *w, u32 bound) {
  wpush(w, 0x07230203);
  wpush(w, 0x00010000);
  wpush(w, 0);
  wpush(w, bound);
  wpush(w, 0);
}

static words make_vertex_spirv(void) {
  enum {
    id_void = 1,
    id_fn_type,
    id_float,
    id_v2,
    id_v4,
    id_ptr_input_v2,
    id_ptr_output_v4,
    id_in_pos,
    id_position,
    id_zero,
    id_one,
    id_main,
    id_label,
    id_loaded_pos,
    id_x,
    id_y,
    id_out_pos,
    id_bound,
  };
  words w = {0};
  spirv_header(&w, id_bound);

  inst(&w, 17, 2); /* OpCapability Shader */
  wpush(&w, 1);
  inst(&w, 14, 3); /* OpMemoryModel Logical GLSL450 */
  wpush(&w, 0);
  wpush(&w, 1);
  inst(&w, 15, 7); /* OpEntryPoint Vertex %main "main" %in_pos %position */
  wpush(&w, 0);
  wpush(&w, id_main);
  string_words(&w, "main");
  wpush(&w, id_in_pos);
  wpush(&w, id_position);
  inst(&w, 71, 4); /* OpDecorate %in_pos Location 0 */
  wpush(&w, id_in_pos);
  wpush(&w, 30);
  wpush(&w, 0);
  inst(&w, 71, 4); /* OpDecorate %position BuiltIn Position */
  wpush(&w, id_position);
  wpush(&w, 11);
  wpush(&w, 0);

  inst(&w, 19, 2); /* OpTypeVoid */
  wpush(&w, id_void);
  inst(&w, 33, 3); /* OpTypeFunction */
  wpush(&w, id_fn_type);
  wpush(&w, id_void);
  inst(&w, 22, 3); /* OpTypeFloat 32 */
  wpush(&w, id_float);
  wpush(&w, 32);
  inst(&w, 23, 4); /* OpTypeVector float 2 */
  wpush(&w, id_v2);
  wpush(&w, id_float);
  wpush(&w, 2);
  inst(&w, 23, 4); /* OpTypeVector float 4 */
  wpush(&w, id_v4);
  wpush(&w, id_float);
  wpush(&w, 4);
  inst(&w, 32, 4); /* OpTypePointer Input v2 */
  wpush(&w, id_ptr_input_v2);
  wpush(&w, 1);
  wpush(&w, id_v2);
  inst(&w, 32, 4); /* OpTypePointer Output v4 */
  wpush(&w, id_ptr_output_v4);
  wpush(&w, 3);
  wpush(&w, id_v4);
  inst(&w, 43, 4); /* OpConstant float 0 */
  wpush(&w, id_float);
  wpush(&w, id_zero);
  wpush(&w, 0x00000000);
  inst(&w, 43, 4); /* OpConstant float 1 */
  wpush(&w, id_float);
  wpush(&w, id_one);
  wpush(&w, 0x3f800000);
  inst(&w, 59, 4); /* OpVariable Input */
  wpush(&w, id_ptr_input_v2);
  wpush(&w, id_in_pos);
  wpush(&w, 1);
  inst(&w, 59, 4); /* OpVariable Output */
  wpush(&w, id_ptr_output_v4);
  wpush(&w, id_position);
  wpush(&w, 3);

  inst(&w, 54, 5); /* OpFunction */
  wpush(&w, id_void);
  wpush(&w, id_main);
  wpush(&w, 0);
  wpush(&w, id_fn_type);
  inst(&w, 248, 2); /* OpLabel */
  wpush(&w, id_label);
  inst(&w, 61, 4); /* OpLoad %v2 %in_pos */
  wpush(&w, id_v2);
  wpush(&w, id_loaded_pos);
  wpush(&w, id_in_pos);
  inst(&w, 81, 5); /* OpCompositeExtract x */
  wpush(&w, id_float);
  wpush(&w, id_x);
  wpush(&w, id_loaded_pos);
  wpush(&w, 0);
  inst(&w, 81, 5); /* OpCompositeExtract y */
  wpush(&w, id_float);
  wpush(&w, id_y);
  wpush(&w, id_loaded_pos);
  wpush(&w, 1);
  inst(&w, 80, 7); /* OpCompositeConstruct vec4(x, y, 0, 1) */
  wpush(&w, id_v4);
  wpush(&w, id_out_pos);
  wpush(&w, id_x);
  wpush(&w, id_y);
  wpush(&w, id_zero);
  wpush(&w, id_one);
  inst(&w, 62, 3); /* OpStore %position %out_pos */
  wpush(&w, id_position);
  wpush(&w, id_out_pos);
  inst(&w, 253, 1); /* OpReturn */
  inst(&w, 56, 1);  /* OpFunctionEnd */
  return w;
}

static words make_procedural_vertex_spirv(void) {
  enum {
    id_void = 1,
    id_fn_type,
    id_float,
    id_int,
    id_bool,
    id_v4,
    id_ptr_input_int,
    id_ptr_output_v4,
    id_vertex_index,
    id_position,
    id_int_zero,
    id_int_one,
    id_float_zero,
    id_float_one,
    id_float_pos,
    id_float_neg,
    id_main,
    id_label,
    id_index,
    id_is_zero,
    id_is_one,
    id_x_nonzero,
    id_x,
    id_y,
    id_out_pos,
    id_bound,
  };
  words w = {0};
  spirv_header(&w, id_bound);

  inst(&w, 17, 2); /* OpCapability Shader */
  wpush(&w, 1);
  inst(&w, 14, 3); /* OpMemoryModel Logical GLSL450 */
  wpush(&w, 0);
  wpush(&w, 1);
  inst(&w, 15, 7); /* OpEntryPoint Vertex %main "main" %vertex_index %position */
  wpush(&w, 0);
  wpush(&w, id_main);
  string_words(&w, "main");
  wpush(&w, id_vertex_index);
  wpush(&w, id_position);
  inst(&w, 71, 4); /* OpDecorate %vertex_index BuiltIn VertexIndex */
  wpush(&w, id_vertex_index);
  wpush(&w, 11);
  wpush(&w, 42);
  inst(&w, 71, 4); /* OpDecorate %position BuiltIn Position */
  wpush(&w, id_position);
  wpush(&w, 11);
  wpush(&w, 0);

  inst(&w, 19, 2); /* OpTypeVoid */
  wpush(&w, id_void);
  inst(&w, 33, 3); /* OpTypeFunction */
  wpush(&w, id_fn_type);
  wpush(&w, id_void);
  inst(&w, 22, 3); /* OpTypeFloat 32 */
  wpush(&w, id_float);
  wpush(&w, 32);
  inst(&w, 21, 4); /* OpTypeInt 32 signed */
  wpush(&w, id_int);
  wpush(&w, 32);
  wpush(&w, 1);
  inst(&w, 20, 2); /* OpTypeBool */
  wpush(&w, id_bool);
  inst(&w, 23, 4); /* OpTypeVector float 4 */
  wpush(&w, id_v4);
  wpush(&w, id_float);
  wpush(&w, 4);
  inst(&w, 32, 4); /* OpTypePointer Input int */
  wpush(&w, id_ptr_input_int);
  wpush(&w, 1);
  wpush(&w, id_int);
  inst(&w, 32, 4); /* OpTypePointer Output v4 */
  wpush(&w, id_ptr_output_v4);
  wpush(&w, 3);
  wpush(&w, id_v4);
  inst(&w, 43, 4); /* OpConstant int 0 */
  wpush(&w, id_int);
  wpush(&w, id_int_zero);
  wpush(&w, 0);
  inst(&w, 43, 4); /* OpConstant int 1 */
  wpush(&w, id_int);
  wpush(&w, id_int_one);
  wpush(&w, 1);
  inst(&w, 43, 4); /* OpConstant float 0 */
  wpush(&w, id_float);
  wpush(&w, id_float_zero);
  wpush(&w, 0x00000000);
  inst(&w, 43, 4); /* OpConstant float 1 */
  wpush(&w, id_float);
  wpush(&w, id_float_one);
  wpush(&w, 0x3f800000);
  inst(&w, 43, 4); /* OpConstant float 0.75 */
  wpush(&w, id_float);
  wpush(&w, id_float_pos);
  wpush(&w, 0x3f400000);
  inst(&w, 43, 4); /* OpConstant float -0.75 */
  wpush(&w, id_float);
  wpush(&w, id_float_neg);
  wpush(&w, 0xbf400000);
  inst(&w, 59, 4); /* OpVariable Input */
  wpush(&w, id_ptr_input_int);
  wpush(&w, id_vertex_index);
  wpush(&w, 1);
  inst(&w, 59, 4); /* OpVariable Output */
  wpush(&w, id_ptr_output_v4);
  wpush(&w, id_position);
  wpush(&w, 3);

  inst(&w, 54, 5); /* OpFunction */
  wpush(&w, id_void);
  wpush(&w, id_main);
  wpush(&w, 0);
  wpush(&w, id_fn_type);
  inst(&w, 248, 2); /* OpLabel */
  wpush(&w, id_label);
  inst(&w, 61, 4); /* OpLoad int %vertex_index */
  wpush(&w, id_int);
  wpush(&w, id_index);
  wpush(&w, id_vertex_index);
  inst(&w, 170, 5); /* OpIEqual index, 0 */
  wpush(&w, id_bool);
  wpush(&w, id_is_zero);
  wpush(&w, id_index);
  wpush(&w, id_int_zero);
  inst(&w, 170, 5); /* OpIEqual index, 1 */
  wpush(&w, id_bool);
  wpush(&w, id_is_one);
  wpush(&w, id_index);
  wpush(&w, id_int_one);
  inst(&w, 169, 6); /* OpSelect index == 1 ? 0.75 : -0.75 */
  wpush(&w, id_float);
  wpush(&w, id_x_nonzero);
  wpush(&w, id_is_one);
  wpush(&w, id_float_pos);
  wpush(&w, id_float_neg);
  inst(&w, 169, 6); /* OpSelect index == 0 ? 0 : x_nonzero */
  wpush(&w, id_float);
  wpush(&w, id_x);
  wpush(&w, id_is_zero);
  wpush(&w, id_float_zero);
  wpush(&w, id_x_nonzero);
  inst(&w, 169, 6); /* OpSelect index == 0 ? -0.75 : 0.75 */
  wpush(&w, id_float);
  wpush(&w, id_y);
  wpush(&w, id_is_zero);
  wpush(&w, id_float_neg);
  wpush(&w, id_float_pos);
  inst(&w, 80, 7); /* OpCompositeConstruct vec4(x, y, 0, 1) */
  wpush(&w, id_v4);
  wpush(&w, id_out_pos);
  wpush(&w, id_x);
  wpush(&w, id_y);
  wpush(&w, id_float_zero);
  wpush(&w, id_float_one);
  inst(&w, 62, 3); /* OpStore %position %out_pos */
  wpush(&w, id_position);
  wpush(&w, id_out_pos);
  inst(&w, 253, 1); /* OpReturn */
  inst(&w, 56, 1);  /* OpFunctionEnd */
  return w;
}

static words make_fragment_spirv(void) {
  enum {
    id_void = 1,
    id_fn_type,
    id_float,
    id_v4,
    id_ptr_output_v4,
    id_out_color,
    id_zero,
    id_one,
    id_main,
    id_label,
    id_red,
    id_bound,
  };
  words w = {0};
  spirv_header(&w, id_bound);

  inst(&w, 17, 2);
  wpush(&w, 1);
  inst(&w, 14, 3);
  wpush(&w, 0);
  wpush(&w, 1);
  inst(&w, 15, 6); /* OpEntryPoint Fragment %main "main" %out_color */
  wpush(&w, 4);
  wpush(&w, id_main);
  string_words(&w, "main");
  wpush(&w, id_out_color);
  inst(&w, 16, 3); /* OpExecutionMode OriginUpperLeft */
  wpush(&w, id_main);
  wpush(&w, 7);
  inst(&w, 71, 4); /* OpDecorate %out_color Location 0 */
  wpush(&w, id_out_color);
  wpush(&w, 30);
  wpush(&w, 0);

  inst(&w, 19, 2);
  wpush(&w, id_void);
  inst(&w, 33, 3);
  wpush(&w, id_fn_type);
  wpush(&w, id_void);
  inst(&w, 22, 3);
  wpush(&w, id_float);
  wpush(&w, 32);
  inst(&w, 23, 4);
  wpush(&w, id_v4);
  wpush(&w, id_float);
  wpush(&w, 4);
  inst(&w, 32, 4);
  wpush(&w, id_ptr_output_v4);
  wpush(&w, 3);
  wpush(&w, id_v4);
  inst(&w, 43, 4);
  wpush(&w, id_float);
  wpush(&w, id_zero);
  wpush(&w, 0x00000000);
  inst(&w, 43, 4);
  wpush(&w, id_float);
  wpush(&w, id_one);
  wpush(&w, 0x3f800000);
  inst(&w, 59, 4);
  wpush(&w, id_ptr_output_v4);
  wpush(&w, id_out_color);
  wpush(&w, 3);

  inst(&w, 54, 5);
  wpush(&w, id_void);
  wpush(&w, id_main);
  wpush(&w, 0);
  wpush(&w, id_fn_type);
  inst(&w, 248, 2);
  wpush(&w, id_label);
  inst(&w, 80, 7); /* vec4(1, 0, 0, 1) */
  wpush(&w, id_v4);
  wpush(&w, id_red);
  wpush(&w, id_one);
  wpush(&w, id_zero);
  wpush(&w, id_zero);
  wpush(&w, id_one);
  inst(&w, 62, 3);
  wpush(&w, id_out_color);
  wpush(&w, id_red);
  inst(&w, 253, 1);
  inst(&w, 56, 1);
  return w;
}

static u32 find_memory_type(VkPhysicalDevice physical, u32 bits, VkMemoryPropertyFlags flags) {
  VkPhysicalDeviceMemoryProperties props;
  vkGetPhysicalDeviceMemoryProperties(physical, &props);
  for (u32 i = 0; i < props.memoryTypeCount; i++) {
    if ((bits & (1u << i)) && (props.memoryTypes[i].propertyFlags & flags) == flags) return i;
  }
  dief("no matching Vulkan memory type");
  return 0;
}

static buffer create_buffer(VkPhysicalDevice physical, VkDevice device, VkDeviceSize size,
                            VkBufferUsageFlags usage, VkMemoryPropertyFlags flags) {
  buffer b = {0};
  VkBufferCreateInfo bi = {
    .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
    .size = size,
    .usage = usage,
    .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
  };
  vk_check(vkCreateBuffer(device, &bi, NULL, &b.buffer), "vkCreateBuffer");

  VkMemoryRequirements req;
  vkGetBufferMemoryRequirements(device, b.buffer, &req);
  VkMemoryAllocateInfo ai = {
    .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
    .allocationSize = req.size,
    .memoryTypeIndex = find_memory_type(physical, req.memoryTypeBits, flags),
  };
  vk_check(vkAllocateMemory(device, &ai, NULL, &b.memory), "vkAllocateMemory(buffer)");
  vk_check(vkBindBufferMemory(device, b.buffer, b.memory, 0), "vkBindBufferMemory");
  return b;
}

static image create_color_image(VkPhysicalDevice physical, VkDevice device, bool linear) {
  image img = {0};
  VkImageCreateInfo ii = {
    .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
    .imageType = VK_IMAGE_TYPE_2D,
    .format = VK_FORMAT_R8G8B8A8_UNORM,
    .extent = {WIDTH, HEIGHT, 1},
    .mipLevels = 1,
    .arrayLayers = 1,
    .samples = VK_SAMPLE_COUNT_1_BIT,
    .tiling = linear ? VK_IMAGE_TILING_LINEAR : VK_IMAGE_TILING_OPTIMAL,
    .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
    .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
    .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
  };
  vk_check(vkCreateImage(device, &ii, NULL, &img.image), "vkCreateImage");

  VkMemoryRequirements req;
  vkGetImageMemoryRequirements(device, img.image, &req);
  if (linear) {
    printf("image memory requirements: size=%" PRIu64 " alignment=%" PRIu64
           " memoryTypeBits=0x%08x\n",
           (uint64_t)req.size, (uint64_t)req.alignment, req.memoryTypeBits);
  }
  VkMemoryAllocateInfo ai = {
    .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
    .allocationSize = req.size,
    .memoryTypeIndex = find_memory_type(physical, req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT),
  };
  vk_check(vkAllocateMemory(device, &ai, NULL, &img.memory), "vkAllocateMemory(image)");
  vk_check(vkBindImageMemory(device, img.image, img.memory, 0), "vkBindImageMemory");
  if (linear) {
    VkImageSubresource subresource = {
      .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
      .mipLevel = 0,
      .arrayLayer = 0,
    };
    VkSubresourceLayout layout;
    vkGetImageSubresourceLayout(device, img.image, &subresource, &layout);
    printf("image subresource layout: offset=%" PRIu64 " size=%" PRIu64
           " rowPitch=%" PRIu64 " arrayPitch=%" PRIu64 " depthPitch=%" PRIu64 "\n",
           (uint64_t)layout.offset, (uint64_t)layout.size, (uint64_t)layout.rowPitch,
           (uint64_t)layout.arrayPitch, (uint64_t)layout.depthPitch);
  }

  VkImageViewCreateInfo vi = {
    .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
    .image = img.image,
    .viewType = VK_IMAGE_VIEW_TYPE_2D,
    .format = VK_FORMAT_R8G8B8A8_UNORM,
    .subresourceRange = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .levelCount = 1, .layerCount = 1},
  };
  vk_check(vkCreateImageView(device, &vi, NULL, &img.view), "vkCreateImageView");
  return img;
}

static VkShaderModule create_shader(VkDevice device, const words *spirv) {
  VkShaderModuleCreateInfo ci = {
    .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
    .codeSize = spirv->count * sizeof(u32),
    .pCode = spirv->data,
  };
  VkShaderModule module;
  vk_check(vkCreateShaderModule(device, &ci, NULL, &module), "vkCreateShaderModule");
  return module;
}

static void write_ppm(const char *path, const u8 *rgba) {
  FILE *f = fopen(path, "wb");
  if (!f) die_errno(path);
  fprintf(f, "P6\n%u %u\n255\n", WIDTH, HEIGHT);
  for (u32 i = 0; i < WIDTH * HEIGHT; i++) {
    fputc(rgba[i * 4 + 0], f);
    fputc(rgba[i * 4 + 1], f);
    fputc(rgba[i * 4 + 2], f);
  }
  fclose(f);
}

int main(void) {
  bool linear_image = getenv("RADV_TRIANGLE_LINEAR") != NULL;
  bool procedural = getenv("RADV_TRIANGLE_PROCEDURAL") != NULL;
  bool no_clear = getenv("RADV_TRIANGLE_NO_CLEAR") != NULL;
  VkInstance instance;
  VkApplicationInfo app = {
    .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
    .pApplicationName = "RADV Triangle",
    .apiVersion = VK_API_VERSION_1_0,
  };
  VkInstanceCreateInfo ici = {
    .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
    .pApplicationInfo = &app,
  };
  vk_check(vkCreateInstance(&ici, NULL, &instance), "vkCreateInstance");

  u32 physical_count = 0;
  vk_check(vkEnumeratePhysicalDevices(instance, &physical_count, NULL), "vkEnumeratePhysicalDevices(count)");
  if (!physical_count) dief("no Vulkan physical devices");
  VkPhysicalDevice *physicals = calloc(physical_count, sizeof(*physicals));
  if (!physicals) die_errno("calloc physical devices");
  vk_check(vkEnumeratePhysicalDevices(instance, &physical_count, physicals),
           "vkEnumeratePhysicalDevices(list)");

  VkPhysicalDevice physical = VK_NULL_HANDLE;
  VkPhysicalDeviceProperties chosen_props = {0};
  for (u32 i = 0; i < physical_count; i++) {
    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(physicals[i], &props);
    if (props.vendorID == 0x1002) {
      physical = physicals[i];
      chosen_props = props;
      break;
    }
  }
  if (!physical) dief("no AMD RADV Vulkan device found");
  printf("device: %s\n", chosen_props.deviceName);
  free(physicals);

  u32 qcount = 0;
  vkGetPhysicalDeviceQueueFamilyProperties(physical, &qcount, NULL);
  VkQueueFamilyProperties *qprops = calloc(qcount, sizeof(*qprops));
  if (!qprops) die_errno("calloc queue families");
  vkGetPhysicalDeviceQueueFamilyProperties(physical, &qcount, qprops);
  u32 qfam = UINT32_MAX;
  for (u32 i = 0; i < qcount; i++) {
    if (qprops[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) {
      qfam = i;
      break;
    }
  }
  free(qprops);
  if (qfam == UINT32_MAX) dief("no graphics queue family");

  float priority = 1.0f;
  VkDeviceQueueCreateInfo qci = {
    .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
    .queueFamilyIndex = qfam,
    .queueCount = 1,
    .pQueuePriorities = &priority,
  };
  VkDeviceCreateInfo dci = {
    .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
    .queueCreateInfoCount = 1,
    .pQueueCreateInfos = &qci,
  };
  VkDevice device;
  vk_check(vkCreateDevice(physical, &dci, NULL, &device), "vkCreateDevice");
  VkQueue queue;
  vkGetDeviceQueue(device, qfam, 0, &queue);

  float vertices[] = {
     0.0f, -0.75f,
     0.75f, 0.75f,
    -0.75f, 0.75f,
  };
  buffer vertex = {0};
  if (!procedural) {
    vertex = create_buffer(physical, device, sizeof(vertices), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                           VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                             VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    void *mapped = NULL;
    vk_check(vkMapMemory(device, vertex.memory, 0, sizeof(vertices), 0, &mapped),
             "vkMapMemory(vertex)");
    memcpy(mapped, vertices, sizeof(vertices));
    vkUnmapMemory(device, vertex.memory);
  }

  image color = create_color_image(physical, device, linear_image);
  buffer readback = create_buffer(physical, device, WIDTH * HEIGHT * 4,
                                  VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                  VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);

  VkAttachmentDescription attachment = {
    .format = VK_FORMAT_R8G8B8A8_UNORM,
    .samples = VK_SAMPLE_COUNT_1_BIT,
    .loadOp = no_clear ? VK_ATTACHMENT_LOAD_OP_DONT_CARE : VK_ATTACHMENT_LOAD_OP_CLEAR,
    .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
    .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    .finalLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
  };
  VkAttachmentReference color_ref = {
    .attachment = 0,
    .layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
  };
  VkSubpassDescription subpass = {
    .pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
    .colorAttachmentCount = 1,
    .pColorAttachments = &color_ref,
  };
  VkRenderPassCreateInfo rpci = {
    .sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
    .attachmentCount = 1,
    .pAttachments = &attachment,
    .subpassCount = 1,
    .pSubpasses = &subpass,
  };
  VkRenderPass render_pass;
  vk_check(vkCreateRenderPass(device, &rpci, NULL, &render_pass), "vkCreateRenderPass");

  VkFramebufferCreateInfo fbci = {
    .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
    .renderPass = render_pass,
    .attachmentCount = 1,
    .pAttachments = &color.view,
    .width = WIDTH,
    .height = HEIGHT,
    .layers = 1,
  };
  VkFramebuffer framebuffer;
  vk_check(vkCreateFramebuffer(device, &fbci, NULL, &framebuffer), "vkCreateFramebuffer");

  words vs_words = procedural ? make_procedural_vertex_spirv() : make_vertex_spirv();
  words fs_words = make_fragment_spirv();
  VkShaderModule vs = create_shader(device, &vs_words);
  VkShaderModule fs = create_shader(device, &fs_words);

  VkPipelineShaderStageCreateInfo stages[2] = {
    {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
     .stage = VK_SHADER_STAGE_VERTEX_BIT,
     .module = vs,
     .pName = "main"},
    {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
     .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
     .module = fs,
     .pName = "main"},
  };
  VkVertexInputBindingDescription binding = {
    .binding = 0,
    .stride = 2 * sizeof(float),
    .inputRate = VK_VERTEX_INPUT_RATE_VERTEX,
  };
  VkVertexInputAttributeDescription attr = {
    .location = 0,
    .binding = 0,
    .format = VK_FORMAT_R32G32_SFLOAT,
    .offset = 0,
  };
  VkPipelineVertexInputStateCreateInfo vis = {
    .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
    .vertexBindingDescriptionCount = procedural ? 0 : 1,
    .pVertexBindingDescriptions = procedural ? NULL : &binding,
    .vertexAttributeDescriptionCount = procedural ? 0 : 1,
    .pVertexAttributeDescriptions = procedural ? NULL : &attr,
  };
  VkPipelineInputAssemblyStateCreateInfo ia = {
    .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
    .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
  };
  VkViewport viewport = {.x = 0, .y = 0, .width = WIDTH, .height = HEIGHT, .minDepth = 0, .maxDepth = 1};
  VkRect2D scissor = {.extent = {WIDTH, HEIGHT}};
  VkPipelineViewportStateCreateInfo vp = {
    .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
    .viewportCount = 1,
    .pViewports = &viewport,
    .scissorCount = 1,
    .pScissors = &scissor,
  };
  VkPipelineRasterizationStateCreateInfo rs = {
    .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
    .polygonMode = VK_POLYGON_MODE_FILL,
    .cullMode = VK_CULL_MODE_NONE,
    .frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE,
    .lineWidth = 1.0f,
  };
  VkPipelineMultisampleStateCreateInfo ms = {
    .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
    .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT,
  };
  VkPipelineColorBlendAttachmentState blend_attachment = {.colorWriteMask = 0xf};
  VkPipelineColorBlendStateCreateInfo blend = {
    .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
    .attachmentCount = 1,
    .pAttachments = &blend_attachment,
  };
  VkPipelineLayoutCreateInfo plci = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
  VkPipelineLayout layout;
  vk_check(vkCreatePipelineLayout(device, &plci, NULL, &layout), "vkCreatePipelineLayout");
  VkGraphicsPipelineCreateInfo gpci = {
    .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
    .stageCount = 2,
    .pStages = stages,
    .pVertexInputState = &vis,
    .pInputAssemblyState = &ia,
    .pViewportState = &vp,
    .pRasterizationState = &rs,
    .pMultisampleState = &ms,
    .pColorBlendState = &blend,
    .layout = layout,
    .renderPass = render_pass,
    .subpass = 0,
  };
  VkPipeline pipeline;
  vk_check(vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &gpci, NULL, &pipeline),
           "vkCreateGraphicsPipelines");

  VkCommandPoolCreateInfo cpci = {
    .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
    .queueFamilyIndex = qfam,
  };
  VkCommandPool pool;
  vk_check(vkCreateCommandPool(device, &cpci, NULL, &pool), "vkCreateCommandPool");
  VkCommandBufferAllocateInfo cbai = {
    .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
    .commandPool = pool,
    .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
    .commandBufferCount = 1,
  };
  VkCommandBuffer cmd;
  vk_check(vkAllocateCommandBuffers(device, &cbai, &cmd), "vkAllocateCommandBuffers");
  VkCommandBufferBeginInfo cbi = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
  vk_check(vkBeginCommandBuffer(cmd, &cbi), "vkBeginCommandBuffer");

  VkClearValue clear = {.color = {{0.02f, 0.02f, 0.04f, 1.0f}}};
  VkRenderPassBeginInfo rpbi = {
    .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
    .renderPass = render_pass,
    .framebuffer = framebuffer,
    .renderArea = {.extent = {WIDTH, HEIGHT}},
    .clearValueCount = no_clear ? 0 : 1,
    .pClearValues = no_clear ? NULL : &clear,
  };
  vkCmdBeginRenderPass(cmd, &rpbi, VK_SUBPASS_CONTENTS_INLINE);
  vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
  if (!procedural) {
    VkDeviceSize offset = 0;
    vkCmdBindVertexBuffers(cmd, 0, 1, &vertex.buffer, &offset);
  }
  vkCmdDraw(cmd, 3, 1, 0, 0);
  vkCmdEndRenderPass(cmd);

  VkBufferImageCopy copy = {
    .imageSubresource = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .layerCount = 1},
    .imageExtent = {WIDTH, HEIGHT, 1},
  };
  vkCmdCopyImageToBuffer(cmd, color.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, readback.buffer, 1, &copy);
  vk_check(vkEndCommandBuffer(cmd), "vkEndCommandBuffer");

  VkSubmitInfo submit = {
    .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
    .commandBufferCount = 1,
    .pCommandBuffers = &cmd,
  };
  vk_check(vkQueueSubmit(queue, 1, &submit, VK_NULL_HANDLE), "vkQueueSubmit");
  vk_check(vkQueueWaitIdle(queue), "vkQueueWaitIdle");

  void *pixels = NULL;
  vk_check(vkMapMemory(device, readback.memory, 0, WIDTH * HEIGHT * 4, 0, &pixels),
           "vkMapMemory(readback)");
  write_ppm("triangle.ppm", pixels);
  vkUnmapMemory(device, readback.memory);
  printf("wrote triangle.ppm (%ux%u)\n", WIDTH, HEIGHT);

  vkDestroyCommandPool(device, pool, NULL);
  vkDestroyPipeline(device, pipeline, NULL);
  vkDestroyPipelineLayout(device, layout, NULL);
  vkDestroyShaderModule(device, fs, NULL);
  vkDestroyShaderModule(device, vs, NULL);
  free(fs_words.data);
  free(vs_words.data);
  vkDestroyFramebuffer(device, framebuffer, NULL);
  vkDestroyRenderPass(device, render_pass, NULL);
  vkDestroyImageView(device, color.view, NULL);
  vkDestroyImage(device, color.image, NULL);
  vkFreeMemory(device, color.memory, NULL);
  vkDestroyBuffer(device, readback.buffer, NULL);
  vkFreeMemory(device, readback.memory, NULL);
  if (!procedural) {
    vkDestroyBuffer(device, vertex.buffer, NULL);
    vkFreeMemory(device, vertex.memory, NULL);
  }
  vkDestroyDevice(device, NULL);
  vkDestroyInstance(instance, NULL);
  return 0;
}
