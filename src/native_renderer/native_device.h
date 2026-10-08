#pragma once

#include <cstddef>
#include <cstdint>

namespace Diligent {
struct IRenderDevice;
struct IDeviceContext;
struct ITextureView;
struct IShader;
}  // namespace Diligent

namespace dante {

struct NativeBuffer;
struct NativePipeline;
struct NativeTexture;

enum class VertexSemantic : uint8_t {
  Position,
  Normal,
  Color,
  TexCoord0,
  TexCoord1,
};

enum class VertexFormat : uint8_t {
  Float32,
  Float32x2,
  Float32x3,
  Float32x4,
  UNorm8x4,
};

struct VertexElement {
  VertexSemantic semantic;
  VertexFormat format;
  uint8_t offset;
  uint8_t binding;
};

enum class PrimitiveTopology : uint8_t {
  TriangleList,
  TriangleStrip,
  LineList,
};

struct BufferDesc {
  uint32_t size_bytes;
  bool is_index_buffer = false;
  bool index_32bit = false;
};

struct UniformBufferBinding {
  uint8_t shader_stage;
  uint8_t binding_slot;
};

enum class TextureFormat : uint8_t {
  RGBA8_UNorm,
  RGBA8_UNorm_sRGB,
  BGRA8_UNorm,
  BGRA8_UNorm_sRGB,
  R8_UNorm,
  BC1_UNorm,
  BC3_UNorm,
  BC7_UNorm,
};

enum class TextureFilter : uint8_t {
  Point,
  Linear,
  Anisotropic,
};

enum class TextureAddress : uint8_t {
  Wrap,
  Clamp,
  Mirror,
};

struct SamplerDesc {
  TextureFilter min_filter = TextureFilter::Linear;
  TextureFilter mag_filter = TextureFilter::Linear;
  TextureFilter mip_filter = TextureFilter::Linear;
  TextureAddress address_u = TextureAddress::Wrap;
  TextureAddress address_v = TextureAddress::Wrap;
  uint8_t max_anisotropy = 0;
};

struct TextureDesc {
  uint32_t width;
  uint32_t height;
  uint32_t mip_levels = 1;
  TextureFormat format;
  const void* initial_data = nullptr;
  uint32_t data_size = 0;
  bool use_custom_sampler = false;
  SamplerDesc sampler;
};

struct PipelineDesc {
  const VertexElement* vertex_elements;
  uint32_t vertex_element_count;
  PrimitiveTopology topology;
  const char* vertex_shader_source;
  const char* pixel_shader_source;
  const UniformBufferBinding* uniform_bindings;
  uint32_t uniform_binding_count;
};

struct NativePresentTimings {
  double upload_draw_cpu_ms = 0.0;
  double present_cpu_ms = 0.0;
};

struct NativeSwapchainDesc {
  uint32_t Width = 0;
  uint32_t Height = 0;
};

class NativeDevice {
 public:
  static constexpr bool kGpuInteropValidated = true;

  NativeDevice();
  ~NativeDevice();

  NativeDevice(const NativeDevice&) = delete;
  NativeDevice& operator=(const NativeDevice&) = delete;

  bool initialize(void* hwnd, uint32_t width, uint32_t height);

  void beginFrame();

  void clear(float r, float g, float b, float a);

  void present(uint32_t sync_interval = 1);

  bool presentImage(uint32_t width, uint32_t height, const void* rgba_data,
                    size_t row_stride, uint32_t sync_interval = 1,
                    NativePresentTimings* timings = nullptr);

  bool presentImageShared(void* shared_handle, uint32_t width, uint32_t height,
                          uint32_t sync_interval = 1,
                          NativePresentTimings* timings = nullptr,
                          uint32_t adapter_luid_low = 0,
                          int32_t adapter_luid_high = 0);

  void setDisplayAspect(double aspect, bool letterbox);

  // Blits a shader-resource view to the swapchain (letterboxed) and presents.
  bool presentTexture(Diligent::ITextureView* srv, uint32_t sync_interval = 1);

  // GLSL (Vulkan) -> SPIR-V -> shader; the caller owns the returned reference.
  Diligent::IShader* createGlslShader(const char* source, bool pixel, const char* name);

  Diligent::IRenderDevice* renderDevice() const;
  Diligent::IDeviceContext* immediateContext() const;

  bool updateWindowSize();

  NativeSwapchainDesc swapchainDesc() const;

  void resize(uint32_t width, uint32_t height);

  void shutdown();

  bool is_valid() const;

  NativeBuffer* createBuffer(const BufferDesc& desc, const void* initial_data);

  void destroyBuffer(NativeBuffer* buffer);

  NativeBuffer* createUniformBuffer(uint32_t size_bytes);

  void updateUniformBuffer(NativeBuffer* buffer, const void* data,
                           uint32_t size_bytes);

  void bindUniformBuffer(uint8_t shader_stage, uint8_t binding_slot,
                         NativeBuffer* buffer);

  NativeTexture* createTexture(const TextureDesc& desc);

  void destroyTexture(NativeTexture* texture);

  void bindTexture(uint8_t shader_stage, uint8_t binding_slot,
                   NativeTexture* texture);

  NativePipeline* createPipeline(const PipelineDesc& desc);

  void destroyPipeline(NativePipeline* pipeline);

  void bindPipeline(NativePipeline* pipeline);

  void bindVertexBuffer(uint32_t binding, NativeBuffer* buffer, uint32_t stride);

  void bindIndexBuffer(NativeBuffer* buffer);

  void draw(uint32_t vertex_count, uint32_t first_vertex = 0);

  void drawIndexed(uint32_t index_count, uint32_t first_index = 0,
                   uint32_t vertex_offset = 0);

 private:
  bool initializeBlitPipeline();

  struct Impl;
  Impl* impl_ = nullptr;
};

}
