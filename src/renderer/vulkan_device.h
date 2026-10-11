#pragma once

#include "Graphics/GraphicsEngine/interface/GraphicsTypes.h"

#include <cstdint>
#include <memory>

namespace Diligent {
struct IRenderDevice;
struct IDeviceContext;
struct ITextureView;
struct IPipelineState;
}  // namespace Diligent

namespace renderer {

// DiligentCore Vulkan device and the game window's swap chain.
class VulkanDevice {
 public:
  VulkanDevice();
  ~VulkanDevice();

  VulkanDevice(const VulkanDevice&) = delete;
  VulkanDevice& operator=(const VulkanDevice&) = delete;

  bool Initialize(void* hwnd, uint32_t width, uint32_t height);

  void BeginFrame();

  void Clear(float r, float g, float b, float a);

  void Present(uint32_t sync_interval = 1);

  void SetDisplayAspect(double aspect, bool letterbox);

  // Blits a shader-resource view to the swapchain (letterboxed) and presents.
  bool PresentTexture(Diligent::ITextureView* srv, uint32_t sync_interval = 1);

  // One triangle covering the target, shaded by `pixel_source` (GLSL) into a
  // single render target, no depth; resources default to dynamic. The
  // caller owns the returned reference.
  Diligent::IPipelineState* CreateFullscreenPipeline(const char* name, const char* pixel_source,
                                                     Diligent::TEXTURE_FORMAT format,
                                                     bool scissor = false);

  Diligent::IRenderDevice* RenderDevice() const;
  Diligent::IDeviceContext* Context() const;

  // Resizes the swap chain to the window; false while the window is minimized.
  bool UpdateWindowSize();

  void Resize(uint32_t width, uint32_t height);

  void Shutdown();

 private:
  bool CreateBlitPipeline();

  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace renderer
