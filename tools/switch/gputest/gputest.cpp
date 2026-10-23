/**
 * @file    tools/switch/gputest/gputest.cpp
 * @brief   GPU time of re:Blue's heavy pixel shaders on deko3d (uam) and on NVK, on the console.
 *
 * P0.4 of docs/SWITCH-deko3d.md: uam's code generator is not NAK, so before moving the renderer the same shaders are
 * timed on both. Each pixel shader of gputest.bin (make_pack.py) is drawn full screen at 960x536 into an RGBA16F
 * target, 20 draws between two GPU timestamps, three times, the fastest kept. Constants: every boolean on (every
 * feature path of the shader), floats 0.25, a 1024x1024 noise texture in every 2D slot, a 64x64 cube map. Both
 * APIs get the same inputs; only the shader compiler and the driver differ.
 *
 * Results go to sdmc:/switch/reblue/logs/gputest.log and to the screen. The GPU runs at the game's handheld profile
 * (460.8 MHz, memory 1331.2 MHz) when the system accepts it, as the log says.
 *
 * @copyright Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *            All rights reserved.
 * @license   BSD 3-Clause License
 */
#include <switch.h>

#include <deko3d.hpp>

#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>

#include <algorithm>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include <sys/stat.h>

namespace {

constexpr uint32_t kWidth = 960, kHeight = 536;
constexpr uint32_t kDraws = 20, kWarmup = 3, kRuns = 3;
constexpr uint32_t kTexSize = 1024, kCubeSize = 64;
constexpr uint32_t kVsBytes = 4096, kPsBytes = 4096, kSharedBytes = 352;

FILE *g_log = nullptr;

void logf(const char *fmt, ...) {
  char buf[1024];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  printf("%s\n", buf);
  consoleUpdate(nullptr);
  if (g_log) {
    fprintf(g_log, "%s\n", buf);
    fflush(g_log);
  }
}

double ticksToMs(uint64_t ticks) { return double(armTicksToNs(ticks)) / 1e6; }

// ---------------------------------------------------------------- the pack

struct Shader {
  std::string name;
  uint64_t hash = 0;
  std::vector<uint8_t> spirv, dksh;
};

struct Pack {
  std::vector<uint8_t> vsSpirv, vsDksh;
  std::vector<Shader> ps;
};

bool readBlob(const std::vector<uint8_t> &f, size_t &o, std::vector<uint8_t> &out) {
  if (o + 4 > f.size())
    return false;
  uint32_t n;
  memcpy(&n, &f[o], 4);
  o += 4;
  if (o + n > f.size())
    return false;
  out.assign(f.begin() + o, f.begin() + o + n);
  o += (n + 3) & ~3u;
  return true;
}

bool loadPack(const char *path, Pack &pack) {
  FILE *fp = fopen(path, "rb");
  if (!fp)
    return false;
  std::vector<uint8_t> f;
  uint8_t buf[65536];
  size_t n;
  while ((n = fread(buf, 1, sizeof(buf), fp)) > 0)
    f.insert(f.end(), buf, buf + n);
  fclose(fp);
  if (f.size() < 12 || memcmp(f.data(), "RBGT", 4))
    return false;
  uint32_t version, count;
  memcpy(&version, &f[4], 4);
  memcpy(&count, &f[8], 4);
  size_t o = 12;
  if (version != 1 || !readBlob(f, o, pack.vsSpirv) || !readBlob(f, o, pack.vsDksh))
    return false;
  for (uint32_t i = 0; i < count; i++) {
    Shader s;
    if (o + 72 > f.size())
      return false;
    s.name.assign(reinterpret_cast<const char *>(&f[o]), strnlen(reinterpret_cast<const char *>(&f[o]), 64));
    memcpy(&s.hash, &f[o + 64], 8);
    o += 72;
    if (!readBlob(f, o, s.spirv) || !readBlob(f, o, s.dksh))
      return false;
    pack.ps.push_back(std::move(s));
  }
  return true;
}

// The three constant blocks. cubeIndex: the cube map's descriptor index (NVK: its own heap; deko3d: one image set).
void fillConstants(uint8_t *vs, uint8_t *ps, uint8_t *shared, uint32_t cubeIndex) {
  memset(vs, 0, kVsBytes);
  const float quarter = 0.25f;
  for (uint32_t i = 0; i < kPsBytes / 4; i++)
    memcpy(ps + i * 4, &quarter, 4);
  memset(shared, 0, kSharedBytes);
  auto *w = reinterpret_cast<uint32_t *>(shared);
  for (uint32_t i = 0; i < 16; i++) {
    w[i] = 1;             // texture2DIndices
    w[16 + i] = 1;        // texture3DIndices (no test shader reads one)
    w[32 + i] = cubeIndex; // textureCubeIndices
    w[48 + i] = 1;        // samplerIndices
  }
  for (uint32_t i = 64; i < 72; i++)
    w[i] = 0xFFFFFFFFu;   // booleansArr: every feature on
  const float one = 1.0f;
  memcpy(shared + 328, &one, 4); // shadowPcfScale
}

void fillNoise(uint8_t *dst, uint32_t texels) {
  uint32_t x = 0x12345678u;
  for (uint32_t i = 0; i < texels * 4; i++) {
    x = x * 1664525u + 1013904223u;
    dst[i] = uint8_t(x >> 24);
  }
}

struct Timing {
  double gpuMs = 0, wallMs = 0; // per draw
  bool ok = false;
};

// ---------------------------------------------------------------- deko3d

void dkDebug(void *, const char *context, DkResult result, const char *message) {
  logf("deko3d: %s: %s (%d)", context ? context : "", message ? message : "", int(result));
}

struct DkCodeShader {
  dk::Shader shader;
  bool ok = false;
};

// A DKSH: control section (kept on the CPU), then code (copied into the code block).
bool loadDksh(const std::vector<uint8_t> &dksh, DkMemBlock codeMem, uint32_t &codeOffset, uint32_t codeLimit,
              std::vector<std::vector<uint8_t>> &controls, dk::Shader &out) {
  struct DkshHeader {
    uint32_t magic, header_sz, control_sz, code_sz, programs_off, num_programs;
  } h;
  if (dksh.size() < sizeof(h))
    return false;
  memcpy(&h, dksh.data(), sizeof(h));
  if (h.control_sz + h.code_sz > dksh.size())
    return false;
  codeOffset = (codeOffset + DK_SHADER_CODE_ALIGNMENT - 1) & ~(DK_SHADER_CODE_ALIGNMENT - 1);
  if (codeOffset + h.code_sz > codeLimit)
    return false;
  memcpy(static_cast<uint8_t *>(dkMemBlockGetCpuAddr(codeMem)) + codeOffset, dksh.data() + h.control_sz, h.code_sz);
  controls.emplace_back(dksh.begin(), dksh.begin() + h.control_sz);
  dk::ShaderMaker{codeMem, codeOffset}.setControl(controls.back().data()).setProgramId(0).initialize(out);
  codeOffset += h.code_sz;
  return true;
}

std::vector<Timing> runDeko3d(const Pack &pack) {
  std::vector<Timing> results(pack.ps.size());
  dk::UniqueDevice device = dk::DeviceMaker{}
                                .setFlags(DkDeviceFlags_DepthZeroToOne | DkDeviceFlags_OriginUpperLeft)
                                .setCbDebug(dkDebug)
                                .create();
  dk::UniqueQueue queue = dk::QueueMaker{device}.setFlags(DkQueueFlags_Graphics).create();

  // Memory: commands, CPU-written data (constants, descriptors, reports, staging), code, images.
  constexpr uint32_t kCmdSize = 4u << 20;
  constexpr uint32_t kDataSize = 8u << 20;
  dk::UniqueMemBlock cmdMem =
      dk::MemBlockMaker{device, kCmdSize}.setFlags(DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached).create();
  dk::UniqueMemBlock dataMem =
      dk::MemBlockMaker{device, kDataSize}.setFlags(DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached).create();
  uint32_t codeSize = DK_SHADER_CODE_UNUSABLE_SIZE + pack.vsDksh.size() + DK_SHADER_CODE_ALIGNMENT;
  for (const Shader &s : pack.ps)
    codeSize += uint32_t(s.dksh.size()) + DK_SHADER_CODE_ALIGNMENT;
  codeSize = (codeSize + DK_MEMBLOCK_ALIGNMENT - 1) & ~(DK_MEMBLOCK_ALIGNMENT - 1);
  dk::UniqueMemBlock codeMem = dk::MemBlockMaker{device, codeSize}
                                   .setFlags(DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached |
                                             DkMemBlockFlags_Code)
                                   .create();

  dk::ImageLayout rtLayout, texLayout, cubeLayout;
  dk::ImageLayoutMaker{device}
      .setFlags(DkImageFlags_UsageRender | DkImageFlags_HwCompression)
      .setFormat(DkImageFormat_RGBA16_Float)
      .setDimensions(kWidth, kHeight)
      .initialize(rtLayout);
  dk::ImageLayoutMaker{device}.setFormat(DkImageFormat_RGBA8_Unorm).setDimensions(kTexSize, kTexSize).initialize(
      texLayout);
  dk::ImageLayoutMaker{device}
      .setType(DkImageType_Cubemap)
      .setFormat(DkImageFormat_RGBA8_Unorm)
      .setDimensions(kCubeSize, kCubeSize)
      .initialize(cubeLayout);
  auto align = [](uint64_t v, uint64_t a) { return (v + a - 1) & ~(a - 1); };
  uint64_t imgSize = 0;
  const uint64_t rtOff = 0;
  imgSize = align(rtOff + rtLayout.getSize(), texLayout.getAlignment());
  const uint64_t texOff = imgSize;
  imgSize = align(texOff + texLayout.getSize(), cubeLayout.getAlignment());
  const uint64_t cubeOff = imgSize;
  imgSize = align(cubeOff + cubeLayout.getSize(), DK_MEMBLOCK_ALIGNMENT);
  dk::UniqueMemBlock imageMem =
      dk::MemBlockMaker{device, uint32_t(imgSize)}.setFlags(DkMemBlockFlags_GpuCached | DkMemBlockFlags_Image).create();
  dk::Image rt, tex, cube;
  rt.initialize(rtLayout, imageMem, uint32_t(rtOff));
  tex.initialize(texLayout, imageMem, uint32_t(texOff));
  cube.initialize(cubeLayout, imageMem, uint32_t(cubeOff));
  dk::ImageView rtView{rt}, texView{tex}, cubeView{cube};

  // The data block: constants at 0, descriptors at 64 KB, reports at 128 KB, staging from 1 MB.
  uint8_t *data = static_cast<uint8_t *>(dataMem.getCpuAddr());
  const DkGpuAddr dataGpu = dataMem.getGpuAddr();
  constexpr uint32_t kVsOff = 0, kPsOff = 4096, kSharedOff = 8192, kImgDescOff = 65536, kSmpDescOff = 65536 + 4096,
                     kReportOff = 131072, kStagingOff = 1u << 20;
  fillConstants(data + kVsOff, data + kPsOff, data + kSharedOff, 2);
  // Image descriptors: 0 and 1 the noise texture, 2 the cube map. Samplers: 0 and 1 linear, repeat.
  auto *imgDesc = reinterpret_cast<dk::ImageDescriptor *>(data + kImgDescOff);
  imgDesc[0].initialize(texView);
  imgDesc[1].initialize(texView);
  imgDesc[2].initialize(cubeView);
  auto *smpDesc = reinterpret_cast<dk::SamplerDescriptor *>(data + kSmpDescOff);
  dk::Sampler sampler;
  sampler.setFilter(DkFilter_Linear, DkFilter_Linear).setWrapMode(DkWrapMode_Repeat, DkWrapMode_Repeat,
                                                                   DkWrapMode_Repeat);
  smpDesc[0].initialize(sampler);
  smpDesc[1].initialize(sampler);
  fillNoise(data + kStagingOff, kTexSize * kTexSize);
  const uint32_t cubeStaging = kStagingOff + kTexSize * kTexSize * 4;
  fillNoise(data + cubeStaging, kCubeSize * kCubeSize);

  dk::UniqueCmdBuf cmd = dk::CmdBufMaker{device}.create();
  cmd.addMemory(cmdMem, 0, kCmdSize);

  // Uploads.
  cmd.copyBufferToImage({dataGpu + kStagingOff, 0, 0}, texView, {0, 0, 0, kTexSize, kTexSize, 1});
  for (uint32_t face = 0; face < 6; face++)
    cmd.copyBufferToImage({dataGpu + cubeStaging, 0, 0}, cubeView, {0, 0, face, kCubeSize, kCubeSize, 1});
  cmd.barrier(DkBarrier_Full, DkInvalidateFlags_Image | DkInvalidateFlags_Descriptors);
  queue.submitCommands(cmd.finishList());
  queue.waitIdle();
  cmd.clear();

  std::vector<std::vector<uint8_t>> controls;
  uint32_t codeOffset = 0;
  const uint32_t codeLimit = codeSize - DK_SHADER_CODE_UNUSABLE_SIZE;
  dk::Shader vs;
  if (!loadDksh(pack.vsDksh, codeMem, codeOffset, codeLimit, controls, vs)) {
    logf("deko3d: the vertex shader's DKSH did not load");
    return results;
  }
  std::vector<dk::Shader> ps(pack.ps.size());
  std::vector<bool> psOk(pack.ps.size());
  for (size_t i = 0; i < pack.ps.size(); i++)
    psOk[i] = loadDksh(pack.ps[i].dksh, codeMem, codeOffset, codeLimit, controls, ps[i]);

  const DkGpuAddr reports = dataGpu + kReportOff;
  for (size_t i = 0; i < pack.ps.size(); i++) {
    if (!psOk[i]) {
      logf("deko3d: %s: DKSH did not load", pack.ps[i].name.c_str());
      continue;
    }
    double bestGpu = 1e9, bestWall = 1e9;
    for (uint32_t run = 0; run < kRuns; run++) {
      cmd.bindRenderTargets({&rtView});
      cmd.setViewports(0, {{0.0f, 0.0f, float(kWidth), float(kHeight), 0.0f, 1.0f}});
      cmd.setScissors(0, {{0, 0, kWidth, kHeight}});
      cmd.bindRasterizerState(dk::RasterizerState{}.setCullMode(DkFace_None));
      cmd.bindColorState(dk::ColorState{});
      cmd.bindColorWriteState(dk::ColorWriteState{});
      cmd.bindDepthStencilState(dk::DepthStencilState{}.setDepthTestEnable(false).setDepthWriteEnable(false));
      cmd.bindImageDescriptorSet(dataGpu + kImgDescOff, 3);
      cmd.bindSamplerDescriptorSet(dataGpu + kSmpDescOff, 2);
      cmd.bindShaders(DkStageFlag_GraphicsMask, {&vs, &ps[i]});
      for (DkStage st : {DkStage_Vertex, DkStage_Fragment}) {
        cmd.bindUniformBuffer(st, 0, dataGpu + kVsOff, kVsBytes);
        cmd.bindUniformBuffer(st, 1, dataGpu + kPsOff, kPsBytes);
        cmd.bindUniformBuffer(st, 2, dataGpu + kSharedOff, 512);
      }
      for (uint32_t d = 0; d < kWarmup; d++)
        cmd.draw(DkPrimitive_Triangles, 3, 1, 0, 0);
      cmd.reportCounter(DkCounter_Timestamp, reports);
      for (uint32_t d = 0; d < kDraws; d++)
        cmd.draw(DkPrimitive_Triangles, 3, 1, 0, 0);
      cmd.reportCounter(DkCounter_Timestamp, reports + 16);
      const uint64_t t0 = armGetSystemTick();
      queue.submitCommands(cmd.finishList());
      queue.waitIdle();
      const uint64_t t1 = armGetSystemTick();
      cmd.clear();
      uint64_t a, b;
      memcpy(&a, data + kReportOff + 8, 8);
      memcpy(&b, data + kReportOff + 24, 8);
      bestGpu = std::min(bestGpu, double(b - a) / 1e6 / kDraws);
      bestWall = std::min(bestWall, ticksToMs(t1 - t0) / (kDraws + kWarmup));
    }
    results[i] = {bestGpu, bestWall, true};
    logf("deko3d  %-24s %7.3f ms/draw (GPU timestamps), %7.3f ms/draw (wall)", pack.ps[i].name.c_str(), bestGpu,
         bestWall);
  }
  return results;
}

// ---------------------------------------------------------------- NVK (Vulkan)

extern "C" VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vk_icdGetInstanceProcAddr(VkInstance instance, const char *pName);
extern "C" VKAPI_ATTR VkResult VKAPI_CALL vk_icdNegotiateLoaderICDInterfaceVersion(uint32_t *pSupportedVersion);

#define VK_FUNCS(X)                                                                                                    \
  X(vkEnumeratePhysicalDevices) X(vkGetPhysicalDeviceProperties) X(vkGetPhysicalDeviceQueueFamilyProperties)        \
  X(vkGetPhysicalDeviceMemoryProperties) X(vkGetPhysicalDeviceFeatures2) X(vkCreateDevice) X(vkDestroyDevice)       \
  X(vkDestroyInstance) X(vkGetDeviceQueue) X(vkCreateBuffer) X(vkGetBufferMemoryRequirements) X(vkAllocateMemory)   \
  X(vkBindBufferMemory) X(vkMapMemory) X(vkCreateImage) X(vkGetImageMemoryRequirements) X(vkBindImageMemory)        \
  X(vkCreateImageView) X(vkCreateSampler) X(vkCreateRenderPass) X(vkCreateFramebuffer) X(vkCreateShaderModule)      \
  X(vkCreateDescriptorSetLayout) X(vkCreatePipelineLayout) X(vkCreateGraphicsPipelines) X(vkCreateDescriptorPool)   \
  X(vkAllocateDescriptorSets) X(vkUpdateDescriptorSets) X(vkCreateCommandPool) X(vkAllocateCommandBuffers)          \
  X(vkBeginCommandBuffer) X(vkEndCommandBuffer) X(vkResetCommandBuffer) X(vkCmdPipelineBarrier)                     \
  X(vkCmdCopyBufferToImage) X(vkCmdBeginRenderPass) X(vkCmdEndRenderPass) X(vkCmdBindPipeline)                      \
  X(vkCmdBindDescriptorSets) X(vkCmdDraw) X(vkCmdWriteTimestamp) X(vkCmdResetQueryPool) X(vkCreateQueryPool)        \
  X(vkGetQueryPoolResults) X(vkQueueSubmit) X(vkQueueWaitIdle) X(vkDeviceWaitIdle) X(vkDestroyPipeline)

struct Vk {
#define DECL(f) PFN_##f f = nullptr;
  VK_FUNCS(DECL)
#undef DECL
  PFN_vkCreateInstance vkCreateInstance = nullptr;
};

std::vector<Timing> runVulkan(const Pack &pack) {
  std::vector<Timing> results(pack.ps.size());
  Vk vk;
  uint32_t icdVersion = 7;
  vk_icdNegotiateLoaderICDInterfaceVersion(&icdVersion);
  auto gipa = reinterpret_cast<PFN_vkGetInstanceProcAddr>(vk_icdGetInstanceProcAddr(VK_NULL_HANDLE,
                                                                                     "vkGetInstanceProcAddr"));
  if (!gipa)
    gipa = &vk_icdGetInstanceProcAddr;
  vk.vkCreateInstance = reinterpret_cast<PFN_vkCreateInstance>(gipa(VK_NULL_HANDLE, "vkCreateInstance"));
  VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
  app.pApplicationName = "reblue-gputest";
  app.apiVersion = VK_API_VERSION_1_2;
  VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
  ici.pApplicationInfo = &app;
  VkInstance instance = VK_NULL_HANDLE;
  if (!vk.vkCreateInstance || vk.vkCreateInstance(&ici, nullptr, &instance) != VK_SUCCESS) {
    logf("NVK: vkCreateInstance failed");
    return results;
  }
#define LOAD(f) vk.f = reinterpret_cast<PFN_##f>(gipa(instance, #f));
  VK_FUNCS(LOAD)
#undef LOAD
  uint32_t n = 1;
  VkPhysicalDevice phys = VK_NULL_HANDLE;
  vk.vkEnumeratePhysicalDevices(instance, &n, &phys);
  VkPhysicalDeviceProperties props;
  vk.vkGetPhysicalDeviceProperties(phys, &props);
  logf("NVK: %s, timestamp period %.3f ns", props.deviceName, props.limits.timestampPeriod);
  VkPhysicalDeviceMemoryProperties mem;
  vk.vkGetPhysicalDeviceMemoryProperties(phys, &mem);

  VkPhysicalDeviceVulkan12Features have12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
  VkPhysicalDeviceFeatures2 have{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
  have.pNext = &have12;
  vk.vkGetPhysicalDeviceFeatures2(phys, &have);
  VkPhysicalDeviceVulkan12Features want12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
  want12.runtimeDescriptorArray = have12.runtimeDescriptorArray;
  want12.descriptorBindingPartiallyBound = have12.descriptorBindingPartiallyBound;
  want12.shaderSampledImageArrayNonUniformIndexing = have12.shaderSampledImageArrayNonUniformIndexing;
  want12.descriptorIndexing = have12.descriptorIndexing;
  float prio = 1.0f;
  VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
  qci.queueFamilyIndex = 0;
  qci.queueCount = 1;
  qci.pQueuePriorities = &prio;
  VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
  dci.pNext = &want12;
  dci.queueCreateInfoCount = 1;
  dci.pQueueCreateInfos = &qci;
  VkDevice dev = VK_NULL_HANDLE;
  if (vk.vkCreateDevice(phys, &dci, nullptr, &dev) != VK_SUCCESS) {
    logf("NVK: vkCreateDevice failed");
    vk.vkDestroyInstance(instance, nullptr);
    return results;
  }
  VkQueue queue;
  vk.vkGetDeviceQueue(dev, 0, 0, &queue);

  auto memType = [&](uint32_t bits, VkMemoryPropertyFlags want) -> uint32_t {
    for (uint32_t i = 0; i < mem.memoryTypeCount; i++)
      if ((bits & (1u << i)) && (mem.memoryTypes[i].propertyFlags & want) == want)
        return i;
    return 0;
  };
  auto alloc = [&](VkMemoryRequirements req, VkMemoryPropertyFlags want) {
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = memType(req.memoryTypeBits, want);
    VkDeviceMemory m = VK_NULL_HANDLE;
    vk.vkAllocateMemory(dev, &ai, nullptr, &m);
    return m;
  };

  // Host-visible buffer: constants at 0/4096/8192, staging from 64 KB.
  constexpr VkDeviceSize kStaging = 65536;
  const VkDeviceSize bufSize = kStaging + kTexSize * kTexSize * 4 + kCubeSize * kCubeSize * 4;
  VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
  bci.size = bufSize;
  bci.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
  VkBuffer buf;
  vk.vkCreateBuffer(dev, &bci, nullptr, &buf);
  VkMemoryRequirements breq;
  vk.vkGetBufferMemoryRequirements(dev, buf, &breq);
  VkDeviceMemory bmem = alloc(breq, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
  vk.vkBindBufferMemory(dev, buf, bmem, 0);
  uint8_t *bdata = nullptr;
  vk.vkMapMemory(dev, bmem, 0, bufSize, 0, reinterpret_cast<void **>(&bdata));
  fillConstants(bdata, bdata + 4096, bdata + 8192, 1);
  fillNoise(bdata + kStaging, kTexSize * kTexSize);
  fillNoise(bdata + kStaging + kTexSize * kTexSize * 4, kCubeSize * kCubeSize);

  auto makeImage = [&](VkFormat fmt, uint32_t w, uint32_t h, uint32_t layers, VkImageUsageFlags usage,
                       VkImageCreateFlags flags, VkImage &img) {
    VkImageCreateInfo ii{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ii.flags = flags;
    ii.imageType = VK_IMAGE_TYPE_2D;
    ii.format = fmt;
    ii.extent = {w, h, 1};
    ii.mipLevels = 1;
    ii.arrayLayers = layers;
    ii.samples = VK_SAMPLE_COUNT_1_BIT;
    ii.tiling = VK_IMAGE_TILING_OPTIMAL;
    ii.usage = usage;
    vk.vkCreateImage(dev, &ii, nullptr, &img);
    VkMemoryRequirements req;
    vk.vkGetImageMemoryRequirements(dev, img, &req);
    vk.vkBindImageMemory(dev, img, alloc(req, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT), 0);
  };
  VkImage rt, tex, cube;
  makeImage(VK_FORMAT_R16G16B16A16_SFLOAT, kWidth, kHeight, 1, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT, 0, rt);
  makeImage(VK_FORMAT_R8G8B8A8_UNORM, kTexSize, kTexSize, 1,
            VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, 0, tex);
  makeImage(VK_FORMAT_R8G8B8A8_UNORM, kCubeSize, kCubeSize, 6,
            VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT, cube);
  auto makeView = [&](VkImage img, VkImageViewType type, VkFormat fmt, uint32_t layers) {
    VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vi.image = img;
    vi.viewType = type;
    vi.format = fmt;
    vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, layers};
    VkImageView v;
    vk.vkCreateImageView(dev, &vi, nullptr, &v);
    return v;
  };
  VkImageView rtView = makeView(rt, VK_IMAGE_VIEW_TYPE_2D, VK_FORMAT_R16G16B16A16_SFLOAT, 1);
  VkImageView texView = makeView(tex, VK_IMAGE_VIEW_TYPE_2D, VK_FORMAT_R8G8B8A8_UNORM, 1);
  VkImageView cubeView = makeView(cube, VK_IMAGE_VIEW_TYPE_CUBE, VK_FORMAT_R8G8B8A8_UNORM, 6);
  VkSamplerCreateInfo si{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
  si.magFilter = si.minFilter = VK_FILTER_LINEAR;
  si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
  si.maxLod = 0.25f;
  VkSampler sampler;
  vk.vkCreateSampler(dev, &si, nullptr, &sampler);

  VkCommandPoolCreateInfo cpi{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
  cpi.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
  VkCommandPool pool;
  vk.vkCreateCommandPool(dev, &cpi, nullptr, &pool);
  VkCommandBufferAllocateInfo cai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
  cai.commandPool = pool;
  cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
  cai.commandBufferCount = 1;
  VkCommandBuffer cb;
  vk.vkAllocateCommandBuffers(dev, &cai, &cb);
  auto submitWait = [&]() {
    VkSubmitInfo sub{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    sub.commandBufferCount = 1;
    sub.pCommandBuffers = &cb;
    vk.vkQueueSubmit(queue, 1, &sub, VK_NULL_HANDLE);
    vk.vkQueueWaitIdle(queue);
  };
  VkCommandBufferBeginInfo cbi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};

  // Uploads and layouts.
  vk.vkBeginCommandBuffer(cb, &cbi);
  auto barrier = [&](VkImage img, uint32_t layers, VkImageLayout from, VkImageLayout to, VkAccessFlags src,
                     VkAccessFlags dst, VkPipelineStageFlags s0, VkPipelineStageFlags s1) {
    VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    b.srcAccessMask = src;
    b.dstAccessMask = dst;
    b.oldLayout = from;
    b.newLayout = to;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = img;
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, layers};
    vk.vkCmdPipelineBarrier(cb, s0, s1, 0, 0, nullptr, 0, nullptr, 1, &b);
  };
  barrier(tex, 1, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT,
          VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
  barrier(cube, 6, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT,
          VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
  VkBufferImageCopy copy{};
  copy.bufferOffset = kStaging;
  copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
  copy.imageExtent = {kTexSize, kTexSize, 1};
  vk.vkCmdCopyBufferToImage(cb, buf, tex, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
  for (uint32_t face = 0; face < 6; face++) {
    copy.bufferOffset = kStaging + kTexSize * kTexSize * 4;
    copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, face, 1};
    copy.imageExtent = {kCubeSize, kCubeSize, 1};
    vk.vkCmdCopyBufferToImage(cb, buf, cube, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
  }
  barrier(tex, 1, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
          VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
          VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
  barrier(cube, 6, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
          VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
          VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
  vk.vkEndCommandBuffer(cb);
  submitWait();

  // re:Blue's layout: set 0 2D heap, 1 3D heap, 2 cube heap, 3 samplers, 4 (unused here), 5 the constant blocks.
  constexpr uint32_t kHeap = 4;
  VkDescriptorSetLayout layouts[6];
  auto heapLayout = [&](VkDescriptorType type, VkDescriptorSetLayout &out) {
    VkDescriptorSetLayoutBinding b{0, type, kHeap, VK_SHADER_STAGE_ALL_GRAPHICS, nullptr};
    VkDescriptorSetLayoutCreateInfo li{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    li.bindingCount = 1;
    li.pBindings = &b;
    vk.vkCreateDescriptorSetLayout(dev, &li, nullptr, &out);
  };
  heapLayout(VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, layouts[0]);
  heapLayout(VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, layouts[1]);
  heapLayout(VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, layouts[2]);
  heapLayout(VK_DESCRIPTOR_TYPE_SAMPLER, layouts[3]);
  {
    VkDescriptorSetLayoutCreateInfo li{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    vk.vkCreateDescriptorSetLayout(dev, &li, nullptr, &layouts[4]);
    VkDescriptorSetLayoutBinding ub[3];
    for (uint32_t i = 0; i < 3; i++)
      ub[i] = {i, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_ALL_GRAPHICS, nullptr};
    li.bindingCount = 3;
    li.pBindings = ub;
    vk.vkCreateDescriptorSetLayout(dev, &li, nullptr, &layouts[5]);
  }
  VkPipelineLayoutCreateInfo pli{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
  pli.setLayoutCount = 6;
  pli.pSetLayouts = layouts;
  VkPipelineLayout playout;
  vk.vkCreatePipelineLayout(dev, &pli, nullptr, &playout);

  VkDescriptorPoolSize sizes[3] = {{VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 3 * kHeap},
                                   {VK_DESCRIPTOR_TYPE_SAMPLER, kHeap},
                                   {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 3}};
  VkDescriptorPoolCreateInfo dpi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
  dpi.maxSets = 6;
  dpi.poolSizeCount = 3;
  dpi.pPoolSizes = sizes;
  VkDescriptorPool dpool;
  vk.vkCreateDescriptorPool(dev, &dpi, nullptr, &dpool);
  VkDescriptorSetAllocateInfo dsai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
  dsai.descriptorPool = dpool;
  dsai.descriptorSetCount = 6;
  dsai.pSetLayouts = layouts;
  VkDescriptorSet sets[6];
  vk.vkAllocateDescriptorSets(dev, &dsai, sets);
  std::vector<VkWriteDescriptorSet> writes;
  VkDescriptorImageInfo tex2d[kHeap], texCube[kHeap], smp[kHeap];
  for (uint32_t i = 0; i < kHeap; i++) {
    tex2d[i] = {VK_NULL_HANDLE, texView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    texCube[i] = {VK_NULL_HANDLE, cubeView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    smp[i] = {sampler, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED};
  }
  auto write = [&](VkDescriptorSet set, uint32_t binding, VkDescriptorType type, uint32_t count,
                   const VkDescriptorImageInfo *img, const VkDescriptorBufferInfo *bufInfo) {
    VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    w.dstSet = set;
    w.dstBinding = binding;
    w.descriptorCount = count;
    w.descriptorType = type;
    w.pImageInfo = img;
    w.pBufferInfo = bufInfo;
    writes.push_back(w);
  };
  write(sets[0], 0, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, kHeap, tex2d, nullptr);
  write(sets[2], 0, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, kHeap, texCube, nullptr);
  write(sets[3], 0, VK_DESCRIPTOR_TYPE_SAMPLER, kHeap, smp, nullptr);
  VkDescriptorBufferInfo ubo[3] = {{buf, 0, kVsBytes}, {buf, 4096, kPsBytes}, {buf, 8192, kSharedBytes}};
  for (uint32_t i = 0; i < 3; i++)
    write(sets[5], i, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, nullptr, &ubo[i]);
  vk.vkUpdateDescriptorSets(dev, uint32_t(writes.size()), writes.data(), 0, nullptr);

  VkAttachmentDescription att{};
  att.format = VK_FORMAT_R16G16B16A16_SFLOAT;
  att.samples = VK_SAMPLE_COUNT_1_BIT;
  att.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
  att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
  att.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
  att.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
  att.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  att.finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
  VkAttachmentReference ref{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
  VkSubpassDescription sub{};
  sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
  sub.colorAttachmentCount = 1;
  sub.pColorAttachments = &ref;
  VkRenderPassCreateInfo rpi{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
  rpi.attachmentCount = 1;
  rpi.pAttachments = &att;
  rpi.subpassCount = 1;
  rpi.pSubpasses = &sub;
  VkRenderPass rp;
  vk.vkCreateRenderPass(dev, &rpi, nullptr, &rp);
  VkFramebufferCreateInfo fbi{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
  fbi.renderPass = rp;
  fbi.attachmentCount = 1;
  fbi.pAttachments = &rtView;
  fbi.width = kWidth;
  fbi.height = kHeight;
  fbi.layers = 1;
  VkFramebuffer fb;
  vk.vkCreateFramebuffer(dev, &fbi, nullptr, &fb);

  auto module = [&](const std::vector<uint8_t> &code) {
    VkShaderModuleCreateInfo mi{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    mi.codeSize = code.size();
    mi.pCode = reinterpret_cast<const uint32_t *>(code.data());
    VkShaderModule m = VK_NULL_HANDLE;
    vk.vkCreateShaderModule(dev, &mi, nullptr, &m);
    return m;
  };
  VkShaderModule vsMod = module(pack.vsSpirv);

  VkQueryPoolCreateInfo qpi{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
  qpi.queryType = VK_QUERY_TYPE_TIMESTAMP;
  qpi.queryCount = 2;
  VkQueryPool qp;
  vk.vkCreateQueryPool(dev, &qpi, nullptr, &qp);

  for (size_t i = 0; i < pack.ps.size(); i++) {
    VkShaderModule psMod = module(pack.ps[i].spirv);
    VkPipelineShaderStageCreateInfo stages[2] = {{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO},
                                                 {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO}};
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vsMod;
    stages[0].pName = "main";
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = psMod;
    stages[1].pName = "main";
    VkPipelineVertexInputStateCreateInfo vin{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkViewport vp{0, 0, float(kWidth), float(kHeight), 0, 1};
    VkRect2D sc{{0, 0}, {kWidth, kHeight}};
    VkPipelineViewportStateCreateInfo vps{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    vps.viewportCount = 1;
    vps.pViewports = &vp;
    vps.scissorCount = 1;
    vps.pScissors = &sc;
    VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rs.cullMode = VK_CULL_MODE_NONE;
    rs.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineColorBlendAttachmentState cba{};
    cba.colorWriteMask = 0xF;
    VkPipelineColorBlendStateCreateInfo cbs{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    cbs.attachmentCount = 1;
    cbs.pAttachments = &cba;
    VkPipelineDepthStencilStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    VkGraphicsPipelineCreateInfo gpi{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    gpi.stageCount = 2;
    gpi.pStages = stages;
    gpi.pVertexInputState = &vin;
    gpi.pInputAssemblyState = &ia;
    gpi.pViewportState = &vps;
    gpi.pRasterizationState = &rs;
    gpi.pMultisampleState = &ms;
    gpi.pDepthStencilState = &ds;
    gpi.pColorBlendState = &cbs;
    gpi.layout = playout;
    gpi.renderPass = rp;
    VkPipeline pipe = VK_NULL_HANDLE;
    if (!psMod || vk.vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &gpi, nullptr, &pipe) != VK_SUCCESS) {
      logf("NVK: %s: pipeline creation failed", pack.ps[i].name.c_str());
      continue;
    }
    double bestGpu = 1e9, bestWall = 1e9;
    for (uint32_t run = 0; run < kRuns; run++) {
      vk.vkResetCommandBuffer(cb, 0);
      vk.vkBeginCommandBuffer(cb, &cbi);
      vk.vkCmdResetQueryPool(cb, qp, 0, 2);
      VkRenderPassBeginInfo rbi{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
      rbi.renderPass = rp;
      rbi.framebuffer = fb;
      rbi.renderArea = sc;
      vk.vkCmdBeginRenderPass(cb, &rbi, VK_SUBPASS_CONTENTS_INLINE);
      vk.vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
      vk.vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, playout, 0, 6, sets, 0, nullptr);
      for (uint32_t d = 0; d < kWarmup; d++)
        vk.vkCmdDraw(cb, 3, 1, 0, 0);
      vk.vkCmdWriteTimestamp(cb, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, qp, 0);
      for (uint32_t d = 0; d < kDraws; d++)
        vk.vkCmdDraw(cb, 3, 1, 0, 0);
      vk.vkCmdWriteTimestamp(cb, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, qp, 1);
      vk.vkCmdEndRenderPass(cb);
      vk.vkEndCommandBuffer(cb);
      const uint64_t t0 = armGetSystemTick();
      submitWait();
      const uint64_t t1 = armGetSystemTick();
      uint64_t ts[2] = {0, 0};
      vk.vkGetQueryPoolResults(dev, qp, 0, 2, sizeof(ts), ts, 8, VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT);
      bestGpu = std::min(bestGpu, double(ts[1] - ts[0]) * props.limits.timestampPeriod / 1e6 / kDraws);
      bestWall = std::min(bestWall, ticksToMs(t1 - t0) / (kDraws + kWarmup));
    }
    results[i] = {bestGpu, bestWall, true};
    logf("NVK     %-24s %7.3f ms/draw (GPU timestamps), %7.3f ms/draw (wall)", pack.ps[i].name.c_str(), bestGpu,
         bestWall);
    vk.vkDestroyPipeline(dev, pipe, nullptr);
  }
  vk.vkDeviceWaitIdle(dev);
  vk.vkDestroyDevice(dev, nullptr); // the rest goes with the process
  vk.vkDestroyInstance(instance, nullptr);
  return results;
}

} // namespace

int main(int, char **) {
  consoleInit(nullptr);
  padConfigureInput(1, HidNpadStyleSet_NpadStandard);
  PadState pad;
  padInitializeDefault(&pad);
  mkdir("sdmc:/switch/reblue/logs", 0777);
  g_log = fopen("sdmc:/switch/reblue/logs/gputest.log", "w");
  logf("re:Blue GPU test: deko3d (uam) against NVK, %ux%u RGBA16F, %u draws x %u runs", kWidth, kHeight, kDraws,
       kRuns);

  // The game's handheld GPU profile (460.8 MHz GPU, 1331.2 MHz memory), as re:Blue asks for it.
  u32 oldConf = 0;
  bool apm = R_SUCCEEDED(apmInitialize());
  if (apm && R_SUCCEEDED(apmGetPerformanceConfiguration(ApmPerformanceMode_Normal, &oldConf))) {
    const Result r = apmSetPerformanceConfiguration(ApmPerformanceMode_Normal, 0x92220008u);
    logf("GPU profile 0x92220008 (460.8 MHz): %s (was 0x%08X)", R_SUCCEEDED(r) ? "set" : "refused", oldConf);
  }

  Pack pack;
  if (!loadPack("sdmc:/switch/reblue/gputest.bin", pack)) {
    logf("sdmc:/switch/reblue/gputest.bin is missing or damaged");
  } else {
    logf("%zu pixel shaders", pack.ps.size());
    std::vector<Timing> dk = runDeko3d(pack);
    std::vector<Timing> nv = runVulkan(pack);
    logf("");
    logf("%-24s %10s %10s %8s", "shader", "deko3d ms", "NVK ms", "dk/NVK");
    double sumDk = 0, sumNv = 0;
    for (size_t i = 0; i < pack.ps.size(); i++) {
      if (!dk[i].ok || !nv[i].ok) {
        logf("%-24s %10s %10s", pack.ps[i].name.c_str(), dk[i].ok ? "ok" : "-", nv[i].ok ? "ok" : "-");
        continue;
      }
      sumDk += dk[i].gpuMs;
      sumNv += nv[i].gpuMs;
      logf("%-24s %10.3f %10.3f %7.2fx", pack.ps[i].name.c_str(), dk[i].gpuMs, nv[i].gpuMs,
           dk[i].gpuMs / nv[i].gpuMs);
    }
    if (sumNv > 0)
      logf("%-24s %10.3f %10.3f %7.2fx", "all", sumDk, sumNv, sumDk / sumNv);
  }
  if (apm && oldConf)
    apmSetPerformanceConfiguration(ApmPerformanceMode_Normal, oldConf);
  logf("");
  logf("Done: press + to exit (the log is sdmc:/switch/reblue/logs/gputest.log)");
  while (appletMainLoop()) {
    padUpdate(&pad);
    if (padGetButtonsDown(&pad) & HidNpadButton_Plus)
      break;
    svcSleepThread(16'000'000);
  }
  if (g_log)
    fclose(g_log);
  if (apm)
    apmExit();
  consoleExit(nullptr);
  return 0;
}
