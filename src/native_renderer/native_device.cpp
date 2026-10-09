#include "native_device.h"

#if defined(_WIN32)
#include <windows.h>
#define VK_USE_PLATFORM_WIN32_KHR
#define VK_NO_PROTOTYPES
#include "volk.h"
#endif

#include "Graphics/GraphicsEngineVulkan/interface/EngineFactoryVk.h"
#include "Graphics/GraphicsEngineVulkan/interface/RenderDeviceVk.h"
#include "Graphics/GraphicsEngine/interface/RenderDevice.h"
#include "Graphics/GraphicsEngine/interface/DeviceContext.h"
#include "Graphics/GraphicsEngine/interface/SwapChain.h"
#include "Graphics/GraphicsEngine/interface/Buffer.h"
#include "Graphics/GraphicsEngine/interface/PipelineState.h"
#include "Graphics/GraphicsEngine/interface/Shader.h"
#include "Graphics/GraphicsEngine/interface/InputLayout.h"
#include "Graphics/GraphicsEngine/interface/ShaderResourceBinding.h"
#include "Graphics/GraphicsEngine/interface/ResourceMapping.h"
#include "Graphics/GraphicsEngine/interface/ShaderResourceVariable.h"
#include "Graphics/GraphicsEngine/interface/Texture.h"
#include "Graphics/GraphicsEngine/interface/TextureView.h"
#include "Graphics/GraphicsEngine/interface/Sampler.h"
#include "Graphics/GraphicsTools/interface/GraphicsUtilities.h"
#include "Graphics/GraphicsAccessories/interface/GraphicsAccessories.hpp"
#include "Primitives/interface/DebugOutput.h"

#include <rex/logging/macros.h>

#include <glslang/Public/ShaderLang.h>
#include <glslang/Include/ResourceLimits.h>
#include <SPIRV/GlslangToSpv.h>

#include <algorithm>
#include <cmath>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <unordered_map>
#include <vector>

// DiligentCore is built without glslang (DILIGENT_NO_GLSLANG): its vendored
// glslang/SPIRV-Tools collide with the SDK's equivalents by target name.
// Shaders are compiled to SPIR-V here with the SDK's glslang instead.
// DefaultTBuiltInResource lives in StandAlone/ResourceLimits.cpp (compiled
// into this target); glslang's public headers only declare TBuiltInResource.
namespace glslang {
extern const TBuiltInResource DefaultTBuiltInResource;
}

namespace dante {

struct NativeBuffer {
  Diligent::IBuffer* buffer = nullptr;
};

struct NativePipeline {
  Diligent::IPipelineState* pipeline = nullptr;
};

struct NativeTexture {
  Diligent::ITexture* texture = nullptr;
  Diligent::ISampler* sampler = nullptr;
};

struct NativeDevice::Impl {
  Diligent::IEngineFactoryVk* factory = nullptr;
  Diligent::IRenderDevice* device = nullptr;
  Diligent::IDeviceContext* context = nullptr;
  Diligent::ISwapChain* swapchain = nullptr;
  Diligent::IPipelineState* current_pipeline = nullptr;
  bool initialized = false;

  Diligent::IPipelineState* blit_pipeline = nullptr;
  Diligent::ISampler* blit_sampler = nullptr;
  Diligent::IShaderResourceBinding* blit_srb = nullptr;
  Diligent::ITexture* blit_texture = nullptr;
  uint32_t blit_texture_w = 0;
  uint32_t blit_texture_h = 0;

  void* hwnd = nullptr;
  double display_aspect = 0.0;
  bool letterbox = true;
  uint32_t logged_width = 0;
  uint32_t logged_height = 0;
  double logged_aspect = -1.0;
  bool logged_letterbox = false;

  void setBlitViewport() {
    const auto& desc = swapchain->GetDesc();
    uint32_t width = desc.Width;
    uint32_t height = desc.Height;
    if (letterbox && std::isfinite(display_aspect) && display_aspect > 0.0) {
      if (display_aspect > double(width) / height) {
        height = uint32_t(std::clamp(std::round(width / display_aspect),
                                     1.0, double(height)));
      } else {
        width = uint32_t(std::clamp(std::round(height * display_aspect),
                                    1.0, double(width)));
      }
    }
    const uint32_t x = (desc.Width - width) / 2;
    const uint32_t y = (desc.Height - height) / 2;
    Diligent::Viewport viewport;
    viewport.TopLeftX = float(x);
    viewport.TopLeftY = float(y);
    viewport.Width = float(width);
    viewport.Height = float(height);
    Diligent::Rect scissor{int32_t(x), int32_t(y),
                           int32_t(x + width), int32_t(y + height)};
    context->SetViewports(1, &viewport, desc.Width, desc.Height);
    context->SetScissorRects(1, &scissor, desc.Width, desc.Height);
    if (logged_width != desc.Width || logged_height != desc.Height ||
        logged_aspect != display_aspect || logged_letterbox != letterbox) {
      REXLOG_INFO("NativeDevice: presentation window={}x{} target_aspect={:.6f} "
                  "letterbox={} viewport=({},{}) {}x{}",
                  desc.Width, desc.Height, display_aspect, letterbox,
                  x, y, width, height);
      logged_width = desc.Width;
      logged_height = desc.Height;
      logged_aspect = display_aspect;
      logged_letterbox = letterbox;
    }
  }

  // The D3D12 presenter rotates shared handles across its mailbox slots, so
  // imported images are cached per handle instead of re-importing per frame.
  struct ImportedImage {
    VkImage vk_image = VK_NULL_HANDLE;
    VkDeviceMemory vk_memory = VK_NULL_HANDLE;
    Diligent::ITexture* texture = nullptr;
    Diligent::ITextureView* srv = nullptr;
    Diligent::IShaderResourceBinding* srb = nullptr;
    uint32_t w = 0;
    uint32_t h = 0;
  };
  std::unordered_map<void*, ImportedImage> imported_images;
  bool ext_memory_supported = false;

  void releaseImportedImage(ImportedImage& img) {
    if (img.srb) { img.srb->Release(); img.srb = nullptr; }
    if (img.srv) { img.srv->Release(); img.srv = nullptr; }
    if (img.texture) { img.texture->Release(); img.texture = nullptr; }
    if (img.vk_image != VK_NULL_HANDLE) {
      vkDestroyImage(static_cast<Diligent::IRenderDeviceVk*>(device)->GetVkDevice(),
                     img.vk_image, nullptr);
      img.vk_image = VK_NULL_HANDLE;
    }
    if (img.vk_memory != VK_NULL_HANDLE) {
      vkFreeMemory(static_cast<Diligent::IRenderDeviceVk*>(device)->GetVkDevice(),
                   img.vk_memory, nullptr);
      img.vk_memory = VK_NULL_HANDLE;
    }
    img.w = 0;
    img.h = 0;
  }
};

NativeDevice::NativeDevice() : impl_(new Impl()) {}

NativeDevice::~NativeDevice() {
  shutdown();
  delete impl_;
}

bool NativeDevice::initialize(void* hwnd, uint32_t width, uint32_t height) {
  shutdown();

  Diligent::SetDebugMessageCallback(
      [](Diligent::DEBUG_MESSAGE_SEVERITY severity, const char* message,
         const char*, const char*, int) {
        if (severity >= Diligent::DEBUG_MESSAGE_SEVERITY_ERROR) {
          REXLOG_ERROR("Diligent: {}", message ? message : "");
        } else if (severity == Diligent::DEBUG_MESSAGE_SEVERITY_WARNING) {
          REXLOG_WARN("Diligent: {}", message ? message : "");
        } else {
          REXLOG_INFO("Diligent: {}", message ? message : "");
        }
      });

  impl_->factory = Diligent::GetEngineFactoryVk();
  if (!impl_->factory) {
    REXLOG_ERROR("NativeDevice: Failed to get Vulkan engine factory");
    return false;
  }

  Diligent::EngineVkCreateInfo engine_ci;
  engine_ci.GraphicsAPIVersion = Diligent::Version{1, 3};
  // Per-draw constant uploads of a full frame (renderer=dante) exceed the 8 MB default.
  engine_ci.DynamicHeapSize = 64u << 20;
#ifdef NDEBUG
  engine_ci.EnableValidation = false;
#else
  engine_ci.EnableValidation = true;
#endif
#if defined(_WIN32)
  const char* device_exts[] = {
      VK_KHR_EXTERNAL_MEMORY_WIN32_EXTENSION_NAME,
  };
  engine_ci.DeviceExtensionCount = 1;
  engine_ci.ppDeviceExtensionNames = device_exts;
#endif

  impl_->hwnd = hwnd;
#if defined(_WIN32)
  RECT client;
  if (GetClientRect(static_cast<HWND>(hwnd), &client) &&
      client.right > 0 && client.bottom > 0) {
    width = uint32_t(client.right);
    height = uint32_t(client.bottom);
  }
  REXLOG_INFO("NativeDevice: hwnd={} client={}x{} swapchain {}x{} visible={}",
              fmt::ptr(hwnd), client.right, client.bottom, width, height,
              hwnd ? IsWindowVisible(static_cast<HWND>(hwnd)) : false);
#endif

  impl_->factory->CreateDeviceAndContextsVk(engine_ci, &impl_->device,
                                             &impl_->context);
  if (!impl_->device || !impl_->context) {
    REXLOG_ERROR("NativeDevice: Failed to create Vulkan device/context");
    shutdown();
    return false;
  }

  Diligent::SwapChainDesc sc_desc;
  sc_desc.Width = width;
  sc_desc.Height = height;
  sc_desc.ColorBufferFormat = Diligent::TEX_FORMAT_RGBA8_UNORM;
  sc_desc.DepthBufferFormat = Diligent::TEX_FORMAT_D32_FLOAT;
  sc_desc.BufferCount = 3;
  sc_desc.DefaultDepthValue = 1.0f;
  sc_desc.IsPrimary = true;

  Diligent::NativeWindow native_window(hwnd);

  impl_->factory->CreateSwapChainVk(impl_->device, impl_->context, sc_desc,
                                    native_window, &impl_->swapchain);
  if (!impl_->swapchain) {
    REXLOG_ERROR("NativeDevice: Failed to create Vulkan swapchain");
    shutdown();
    return false;
  }

  if (!initializeBlitPipeline()) {
    shutdown();
    return false;
  }

  impl_->initialized = true;
  REXLOG_INFO("NativeDevice: Vulkan device initialized ({}x{}, encoded RGB passthrough, format={})",
              width, height, Diligent::GetTextureFormatAttribs(
                  impl_->swapchain->GetDesc().ColorBufferFormat).Name);
  return true;
}

void NativeDevice::beginFrame() {
  if (!impl_->initialized) return;

  auto* rtv = impl_->swapchain->GetCurrentBackBufferRTV();
  auto* dsv = impl_->swapchain->GetDepthBufferDSV();
  impl_->context->SetRenderTargets(1, &rtv, dsv,
                                   Diligent::RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
}

void NativeDevice::clear(float r, float g, float b, float a) {
  if (!impl_->initialized) return;

  const float clear_color[] = {r, g, b, a};
  auto* rtv = impl_->swapchain->GetCurrentBackBufferRTV();
  impl_->context->ClearRenderTarget(rtv, clear_color,
                                    Diligent::RESOURCE_STATE_TRANSITION_MODE_TRANSITION);

  auto* dsv = impl_->swapchain->GetDepthBufferDSV();
  if (dsv) {
    impl_->context->ClearDepthStencil(dsv, Diligent::CLEAR_DEPTH_FLAG, 1.0f, 0,
                                      Diligent::RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
  }
}

void NativeDevice::present(uint32_t sync_interval) {
  if (!impl_->initialized) return;
  impl_->swapchain->Present(sync_interval);
}

static const char* kBlitVS = R"(
#version 450
layout(location = 0) out vec2 out_uv;
void main() {
    uint vid = uint(gl_VertexIndex);
    gl_Position = vec4(vid == 1 ? 3.0 : -1.0,
                       vid == 2 ? 3.0 : -1.0, 0.0, 1.0);
    out_uv = vec2(vid == 1 ? 2.0 : 0.0,
                  vid == 2 ? 2.0 : 0.0);
}
)";

static const char* kBlitPS = R"(
#version 450
layout(binding = 0) uniform texture2D g_Texture;
layout(binding = 1) uniform sampler g_Sampler;
layout(location = 0) in vec2 in_uv;
layout(location = 0) out vec4 out_color;
void main() {
    vec2 uv = vec2(in_uv.x, 1.0 - in_uv.y);
    out_color = texture(sampler2D(g_Texture, g_Sampler), uv);
}
)";

static bool compileGlslToSpv(const char* source, EShLanguage stage,
                             std::vector<uint32_t>& out_spirv) {
  static const bool glslang_ready = [] {
    return glslang::InitializeProcess();
  }();
  if (!glslang_ready) {
    REXLOG_ERROR("NativeDevice: glslang InitializeProcess failed");
    return false;
  }

  glslang::TShader shader(stage);
  const char* strings[] = {source};
  shader.setStrings(strings, 1);
  shader.setEntryPoint("main");
  shader.setSourceEntryPoint("main");
  shader.setEnvInput(glslang::EShSourceGlsl, stage, glslang::EShClientVulkan,
                     450);
  shader.setEnvClient(glslang::EShClientVulkan, glslang::EShTargetVulkan_1_0);
  shader.setEnvTarget(glslang::EShTargetSpv, glslang::EShTargetSpv_1_0);

  const auto messages = EShMessages(EShMsgSpvRules | EShMsgVulkanRules);
  if (!shader.parse(&glslang::DefaultTBuiltInResource, 450, false, messages)) {
    REXLOG_ERROR("NativeDevice: glslang parse failed: {}",
                 shader.getInfoLog() ? shader.getInfoLog() : "");
    return false;
  }
  glslang::TProgram program;
  program.addShader(&shader);
  if (!program.link(messages)) {
    REXLOG_ERROR("NativeDevice: glslang link failed: {}",
                 program.getInfoLog() ? program.getInfoLog() : "");
    return false;
  }
  glslang::GlslangToSpv(*program.getIntermediate(stage), out_spirv);
  return !out_spirv.empty();
}

static Diligent::IShader* createShader(Diligent::IRenderDevice* device,
                                           const char* source,
                                           Diligent::SHADER_TYPE type,
                                           const char* name) {
  std::vector<uint32_t> spirv;
  if (!compileGlslToSpv(
          source,
          type == Diligent::SHADER_TYPE_VERTEX ? EShLangVertex
                                               : EShLangFragment,
          spirv)) {
    return nullptr;
  }
  Diligent::ShaderCreateInfo ci;
  ci.ByteCode = spirv.data();
  ci.ByteCodeSize = spirv.size() * sizeof(uint32_t);
  ci.EntryPoint = "main";
  ci.Desc.ShaderType = type;
  ci.Desc.Name = name;
  Diligent::IShader* shader = nullptr;
  device->CreateShader(ci, &shader);
  return shader;
}

bool NativeDevice::initializeBlitPipeline() {
  if (!impl_->blit_pipeline) {
    Diligent::IShader* vs = createShader(
        impl_->device, kBlitVS, Diligent::SHADER_TYPE_VERTEX, "BlitVS");
    if (!vs) {
      REXLOG_ERROR("NativeDevice: Failed to compile blit VS");
      return false;
    }

    Diligent::IShader* ps = createShader(
        impl_->device, kBlitPS, Diligent::SHADER_TYPE_PIXEL, "BlitPS");
    if (!ps) {
      REXLOG_ERROR("NativeDevice: Failed to compile blit PS");
      vs->Release();
      return false;
    }

    Diligent::SamplerDesc sdesc;
    sdesc.MinFilter = Diligent::FILTER_TYPE_LINEAR;
    sdesc.MagFilter = Diligent::FILTER_TYPE_LINEAR;
    sdesc.MipFilter = Diligent::FILTER_TYPE_LINEAR;
    sdesc.AddressU = Diligent::TEXTURE_ADDRESS_CLAMP;
    sdesc.AddressV = Diligent::TEXTURE_ADDRESS_CLAMP;
    sdesc.Name = "BlitSampler";
    impl_->device->CreateSampler(sdesc, &impl_->blit_sampler);

    const auto& sc_desc = impl_->swapchain->GetDesc();

    Diligent::GraphicsPipelineStateCreateInfo pso_ci;
    pso_ci.PSODesc.Name = "BlitPipeline";
    pso_ci.PSODesc.PipelineType = Diligent::PIPELINE_TYPE_GRAPHICS;
    pso_ci.pVS = vs;
    pso_ci.pPS = ps;
    pso_ci.GraphicsPipeline.NumRenderTargets = 1;
    pso_ci.GraphicsPipeline.RTVFormats[0] = sc_desc.ColorBufferFormat;
    pso_ci.GraphicsPipeline.DSVFormat = sc_desc.DepthBufferFormat;
    pso_ci.GraphicsPipeline.PrimitiveTopology =
        Diligent::PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    pso_ci.GraphicsPipeline.InputLayout.NumElements = 0;
    pso_ci.GraphicsPipeline.RasterizerDesc.CullMode = Diligent::CULL_MODE_NONE;
    pso_ci.GraphicsPipeline.RasterizerDesc.ScissorEnable = Diligent::True;
    pso_ci.GraphicsPipeline.DepthStencilDesc.DepthEnable = Diligent::False;
    auto& rt0 = pso_ci.GraphicsPipeline.BlendDesc.RenderTargets[0];
    rt0.BlendEnable = Diligent::False;
    rt0.RenderTargetWriteMask = Diligent::COLOR_MASK_ALL;
    pso_ci.PSODesc.ResourceLayout.DefaultVariableType =
        Diligent::SHADER_RESOURCE_VARIABLE_TYPE_STATIC;
    Diligent::ShaderResourceVariableDesc vars[] = {
        {Diligent::SHADER_TYPE_PIXEL, "g_Texture",
         Diligent::SHADER_RESOURCE_VARIABLE_TYPE_MUTABLE},
    };
    pso_ci.PSODesc.ResourceLayout.NumVariables = 1;
    pso_ci.PSODesc.ResourceLayout.Variables = vars;

    impl_->device->CreateGraphicsPipelineState(pso_ci, &impl_->blit_pipeline);
    vs->Release();
    ps->Release();

    if (!impl_->blit_pipeline) {
      REXLOG_ERROR("NativeDevice: Failed to create blit pipeline");
      return false;
    }

    auto* samp_var = impl_->blit_pipeline->GetStaticVariableByName(
        Diligent::SHADER_TYPE_PIXEL, "g_Sampler");
    if (samp_var && impl_->blit_sampler)
      samp_var->Set(impl_->blit_sampler);
    else {
      REXLOG_ERROR("NativeDevice: blit sampler static var not found or null sampler");
      return false;
    }

    REXLOG_INFO("NativeDevice: blit pipeline created (GLSL->SPIRV, cull=none)");

  }
  return true;
}

bool NativeDevice::presentImage(uint32_t width, uint32_t height,
                                const void* rgba_data, size_t row_stride,
                                uint32_t sync_interval, NativePresentTimings* timings) {
  if (!impl_->initialized || width == 0 || height == 0 || !rgba_data ||
      row_stride < size_t(width) * 4)
    return false;

  using Clock = std::chrono::steady_clock;
  const auto upload_start = timings ? Clock::now() : Clock::time_point{};
  if (timings) *timings = {};

  if (!impl_->blit_texture || !impl_->blit_srb ||
      impl_->blit_texture_w != width || impl_->blit_texture_h != height) {
    if (impl_->blit_srb) {
      impl_->blit_srb->Release();
      impl_->blit_srb = nullptr;
    }
    if (impl_->blit_texture) {
      impl_->blit_texture->Release();
      impl_->blit_texture = nullptr;
    }
    Diligent::TextureDesc tex_desc;
    tex_desc.Type = Diligent::RESOURCE_DIM_TEX_2D;
    tex_desc.Width = width;
    tex_desc.Height = height;
    tex_desc.MipLevels = 1;
    tex_desc.Format = Diligent::TEX_FORMAT_RGBA8_UNORM;
    tex_desc.BindFlags = Diligent::BIND_SHADER_RESOURCE;
    tex_desc.Usage = Diligent::USAGE_DEFAULT;
    tex_desc.Name = "BlitStagingTexture";
    impl_->device->CreateTexture(tex_desc, nullptr, &impl_->blit_texture);
    if (!impl_->blit_texture) {
      REXLOG_ERROR("NativeDevice: Failed to create blit texture");
      return false;
    }
    impl_->blit_pipeline->CreateShaderResourceBinding(&impl_->blit_srb, true);
    if (!impl_->blit_srb) {
      REXLOG_ERROR("NativeDevice: Failed to create blit SRB");
      return false;
    }
    auto* tex_var = impl_->blit_srb->GetVariableByName(
        Diligent::SHADER_TYPE_PIXEL, "g_Texture");
    auto* srv =
        impl_->blit_texture->GetDefaultView(Diligent::TEXTURE_VIEW_SHADER_RESOURCE);
    if (!tex_var || !srv) {
      REXLOG_ERROR("NativeDevice: blit texture variable or SRV unavailable");
      impl_->blit_srb->Release();
      impl_->blit_srb = nullptr;
      return false;
    }
    tex_var->Set(srv);
    impl_->blit_texture_w = width;
    impl_->blit_texture_h = height;
    REXLOG_INFO("NativeDevice: blit texture created ({}x{})", width, height);
  }

  Diligent::Box box;
  box.MinX = 0;
  box.MaxX = width;
  box.MinY = 0;
  box.MaxY = height;
  box.MinZ = 0;
  box.MaxZ = 1;
  Diligent::TextureSubResData sub_res;
  sub_res.pData = rgba_data;
  sub_res.Stride = row_stride;
  impl_->context->UpdateTexture(
      impl_->blit_texture, 0, 0, box, sub_res,
      Diligent::RESOURCE_STATE_TRANSITION_MODE_TRANSITION,
      Diligent::RESOURCE_STATE_TRANSITION_MODE_TRANSITION);

  auto* rtv = impl_->swapchain->GetCurrentBackBufferRTV();
  auto* dsv = impl_->swapchain->GetDepthBufferDSV();
  impl_->context->SetRenderTargets(1, &rtv, dsv,
                                   Diligent::RESOURCE_STATE_TRANSITION_MODE_TRANSITION);

  const float clear_color[] = {0.0f, 0.0f, 0.0f, 1.0f};
  impl_->context->ClearRenderTarget(rtv, clear_color,
                                    Diligent::RESOURCE_STATE_TRANSITION_MODE_TRANSITION);

  impl_->setBlitViewport();

  impl_->context->SetPipelineState(impl_->blit_pipeline);
  impl_->context->CommitShaderResources(
      impl_->blit_srb, Diligent::RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
  Diligent::DrawAttribs da;
  da.NumVertices = 3;
  da.Flags = Diligent::DRAW_FLAG_VERIFY_ALL;
  impl_->context->Draw(da);

  const auto present_start = timings ? Clock::now() : Clock::time_point{};
  impl_->swapchain->Present(sync_interval);
  if (timings) {
    timings->upload_draw_cpu_ms =
        std::chrono::duration<double, std::milli>(present_start - upload_start).count();
    timings->present_cpu_ms =
        std::chrono::duration<double, std::milli>(Clock::now() - present_start).count();
  }
  return true;
}

bool NativeDevice::presentTexture(Diligent::ITextureView* srv, uint32_t sync_interval) {
  if (!impl_->initialized || !srv) return false;
  Diligent::IShaderResourceBinding* srb = nullptr;
  impl_->blit_pipeline->CreateShaderResourceBinding(&srb, true);
  if (!srb) return false;
  if (auto* var = srb->GetVariableByName(Diligent::SHADER_TYPE_PIXEL, "g_Texture")) {
    var->Set(srv);
  }
  auto* rtv = impl_->swapchain->GetCurrentBackBufferRTV();
  auto* dsv = impl_->swapchain->GetDepthBufferDSV();
  impl_->context->SetRenderTargets(1, &rtv, dsv,
                                   Diligent::RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
  const float clear_color[] = {0.0f, 0.0f, 0.0f, 1.0f};
  impl_->context->ClearRenderTarget(rtv, clear_color,
                                    Diligent::RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
  impl_->setBlitViewport();
  impl_->context->SetPipelineState(impl_->blit_pipeline);
  impl_->context->CommitShaderResources(srb, Diligent::RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
  Diligent::DrawAttribs da;
  da.NumVertices = 3;
  impl_->context->Draw(da);
  srb->Release();
  impl_->swapchain->Present(sync_interval);
  return true;
}

Diligent::IShader* NativeDevice::createGlslShader(const char* source, bool pixel,
                                                  const char* name) {
  return createShader(impl_->device, source,
                      pixel ? Diligent::SHADER_TYPE_PIXEL : Diligent::SHADER_TYPE_VERTEX, name);
}

Diligent::IRenderDevice* NativeDevice::renderDevice() const { return impl_->device; }
Diligent::IDeviceContext* NativeDevice::immediateContext() const { return impl_->context; }

void NativeDevice::setDisplayAspect(double aspect, bool letterbox) {
  if (!impl_->initialized) return;
  impl_->display_aspect = aspect;
  impl_->letterbox = letterbox;
}

bool NativeDevice::updateWindowSize() {
  if (!impl_->initialized) return false;
#if defined(_WIN32)
  const auto hwnd = static_cast<HWND>(impl_->hwnd);
  RECT client;
  if (IsIconic(hwnd) || !GetClientRect(hwnd, &client) ||
      client.right <= 0 || client.bottom <= 0) return false;
  const auto& desc = impl_->swapchain->GetDesc();
  if (desc.Width != uint32_t(client.right) || desc.Height != uint32_t(client.bottom)) {
    resize(uint32_t(client.right), uint32_t(client.bottom));
    REXLOG_INFO("NativeDevice: client resized to {}x{}", client.right, client.bottom);
  }
#endif
  return true;
}

NativeSwapchainDesc NativeDevice::swapchainDesc() const {
  NativeSwapchainDesc desc;
  if (impl_->initialized && impl_->swapchain) {
    const auto& sc_desc = impl_->swapchain->GetDesc();
    desc.Width = sc_desc.Width;
    desc.Height = sc_desc.Height;
  }
  return desc;
}

bool NativeDevice::presentImageShared(void* shared_handle, uint32_t width,
                                       uint32_t height, uint32_t sync_interval,
                                       NativePresentTimings* timings,
                                       uint32_t adapter_luid_low,
                                       int32_t adapter_luid_high) {
  if (!impl_->initialized || !shared_handle || width == 0 || height == 0)
    return false;

#if !defined(_WIN32)
  return false;
#else
  using Clock = std::chrono::steady_clock;
  const auto upload_start = timings ? Clock::now() : Clock::time_point{};
  if (timings) *timings = {};

  if (!impl_->ext_memory_supported) {
    auto* render_device_vk = static_cast<Diligent::IRenderDeviceVk*>(impl_->device);
    VkPhysicalDevice phys_dev = render_device_vk->GetVkPhysicalDevice();
    VkDevice vk_dev = render_device_vk->GetVkDevice();

    VkPhysicalDeviceIDProperties id_props{};
    id_props.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES;
    VkPhysicalDeviceProperties2 props2{};
    props2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
    props2.pNext = &id_props;
    vkGetPhysicalDeviceProperties2(phys_dev, &props2);

    if (adapter_luid_low != 0 || adapter_luid_high != 0) {
      if (id_props.deviceLUIDValid) {
        const uint32_t vk_luid_low =
            reinterpret_cast<const uint32_t*>(id_props.deviceLUID)[0];
        const int32_t vk_luid_high =
            reinterpret_cast<const int32_t*>(id_props.deviceLUID)[1];
        if (vk_luid_low != adapter_luid_low || vk_luid_high != adapter_luid_high) {
          REXLOG_ERROR("NativeDevice: adapter LUID mismatch (d3d12=({}, {}) "
                       "vulkan=({}, {})) - cross-API sharing requires same GPU",
                       adapter_luid_low, adapter_luid_high,
                       vk_luid_low, vk_luid_high);
          return false;
        }
        REXLOG_INFO("NativeDevice: adapter LUID match verified ({}, {})",
                    vk_luid_low, vk_luid_high);
      } else {
        REXLOG_WARN("NativeDevice: Vulkan deviceLUID not valid, cannot verify "
                    "adapter match");
      }
    }

    VkPhysicalDeviceExternalImageFormatInfo external_fmt_info{};
    external_fmt_info.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_IMAGE_FORMAT_INFO;
    external_fmt_info.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D12_RESOURCE_BIT;

    VkPhysicalDeviceImageFormatInfo2 fmt_info{};
    fmt_info.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2;
    fmt_info.format = VK_FORMAT_A2R10G10B10_UNORM_PACK32;
    fmt_info.type = VK_IMAGE_TYPE_2D;
    fmt_info.tiling = VK_IMAGE_TILING_OPTIMAL;
    fmt_info.usage = VK_IMAGE_USAGE_SAMPLED_BIT;
    fmt_info.pNext = &external_fmt_info;

    VkExternalImageFormatProperties external_fmt_props{};
    external_fmt_props.sType = VK_STRUCTURE_TYPE_EXTERNAL_IMAGE_FORMAT_PROPERTIES;
    VkImageFormatProperties2 fmt_props2{};
    fmt_props2.sType = VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2;
    fmt_props2.pNext = &external_fmt_props;

    VkResult res = vkGetPhysicalDeviceImageFormatProperties2(
        phys_dev, &fmt_info, &fmt_props2);
    if (res != VK_SUCCESS) {
      REXLOG_WARN("NativeDevice: VK_KHR_external_memory_win32 not supported for "
                  "R10G10B10A2 (res={})", int(res));
      return false;
    }
    impl_->ext_memory_supported = true;
    REXLOG_INFO("NativeDevice: external memory import supported");
  }

  auto img_it = impl_->imported_images.find(shared_handle);
  if (img_it != impl_->imported_images.end() &&
      (img_it->second.w != width || img_it->second.h != height)) {
    impl_->releaseImportedImage(img_it->second);
    impl_->imported_images.erase(img_it);
    img_it = impl_->imported_images.end();
  }

  if (img_it == impl_->imported_images.end()) {
    Impl::ImportedImage img;

    auto* render_device_vk = static_cast<Diligent::IRenderDeviceVk*>(impl_->device);
    VkDevice vk_dev = render_device_vk->GetVkDevice();

    VkExternalMemoryImageCreateInfo external_ci{};
    external_ci.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO;
    external_ci.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D12_RESOURCE_BIT;

    VkImageCreateInfo image_ci{};
    image_ci.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    image_ci.pNext = &external_ci;
    image_ci.imageType = VK_IMAGE_TYPE_2D;
    image_ci.format = VK_FORMAT_A2R10G10B10_UNORM_PACK32;
    image_ci.extent.width = width;
    image_ci.extent.height = height;
    image_ci.extent.depth = 1;
    image_ci.mipLevels = 1;
    image_ci.arrayLayers = 1;
    image_ci.samples = VK_SAMPLE_COUNT_1_BIT;
    image_ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    image_ci.usage = VK_IMAGE_USAGE_SAMPLED_BIT;
    image_ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    image_ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    VkResult res = vkCreateImage(vk_dev, &image_ci, nullptr, &img.vk_image);
    if (res != VK_SUCCESS) {
      REXLOG_ERROR("NativeDevice: vkCreateImage failed (res={})", int(res));
      return false;
    }

    VkMemoryRequirements mem_reqs;
    vkGetImageMemoryRequirements(vk_dev, img.vk_image, &mem_reqs);

    VkMemoryDedicatedAllocateInfo dedicated_ai{};
    dedicated_ai.sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO;
    dedicated_ai.image = img.vk_image;
    dedicated_ai.buffer = VK_NULL_HANDLE;

    VkImportMemoryWin32HandleInfoKHR import_info{};
    import_info.sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_WIN32_HANDLE_INFO_KHR;
    import_info.pNext = &dedicated_ai;
    import_info.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D12_RESOURCE_BIT;
    import_info.handle = shared_handle;
    import_info.name = nullptr;

    VkMemoryAllocateInfo alloc_info{};
    alloc_info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    alloc_info.pNext = &import_info;
    alloc_info.allocationSize = mem_reqs.size;

    VkPhysicalDeviceMemoryProperties mem_props;
    vkGetPhysicalDeviceMemoryProperties(
        render_device_vk->GetVkPhysicalDevice(), &mem_props);
    alloc_info.memoryTypeIndex = UINT32_MAX;
    for (uint32_t i = 0; i < mem_props.memoryTypeCount; ++i) {
      if ((mem_reqs.memoryTypeBits & (1 << i)) &&
          (mem_props.memoryTypes[i].propertyFlags &
           VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) {
        alloc_info.memoryTypeIndex = i;
        break;
      }
    }
    if (alloc_info.memoryTypeIndex == UINT32_MAX) {
      for (uint32_t i = 0; i < mem_props.memoryTypeCount; ++i) {
        if (mem_reqs.memoryTypeBits & (1 << i)) {
          alloc_info.memoryTypeIndex = i;
          break;
        }
      }
    }
    if (alloc_info.memoryTypeIndex == UINT32_MAX) {
      REXLOG_ERROR("NativeDevice: No suitable memory type for imported image");
      impl_->releaseImportedImage(img);
      return false;
    }

    res = vkAllocateMemory(vk_dev, &alloc_info, nullptr, &img.vk_memory);
    if (res != VK_SUCCESS) {
      REXLOG_ERROR("NativeDevice: vkAllocateMemory failed (res={})", int(res));
      impl_->releaseImportedImage(img);
      return false;
    }

    res = vkBindImageMemory(vk_dev, img.vk_image, img.vk_memory, 0);
    if (res != VK_SUCCESS) {
      REXLOG_ERROR("NativeDevice: vkBindImageMemory failed (res={})", int(res));
      impl_->releaseImportedImage(img);
      return false;
    }

    Diligent::TextureDesc tex_desc;
    tex_desc.Type = Diligent::RESOURCE_DIM_TEX_2D;
    tex_desc.Width = width;
    tex_desc.Height = height;
    tex_desc.MipLevels = 1;
    tex_desc.Format = Diligent::TEX_FORMAT_RGB10A2_UNORM;
    tex_desc.BindFlags = Diligent::BIND_SHADER_RESOURCE;
    tex_desc.Usage = Diligent::USAGE_DEFAULT;
    tex_desc.Name = "ImportedGuestOutput";

    render_device_vk->CreateTextureFromVulkanImage(
        img.vk_image, tex_desc,
        Diligent::RESOURCE_STATE_SHADER_RESOURCE, &img.texture);
    if (!img.texture) {
      REXLOG_ERROR("NativeDevice: CreateTextureFromVulkanImage failed");
      impl_->releaseImportedImage(img);
      return false;
    }

    impl_->blit_pipeline->CreateShaderResourceBinding(&img.srb, true);
    if (!img.srb) {
      REXLOG_ERROR("NativeDevice: Failed to create imported SRB");
      impl_->releaseImportedImage(img);
      return false;
    }
    auto* tex_var = img.srb->GetVariableByName(
        Diligent::SHADER_TYPE_PIXEL, "g_Texture");
    Diligent::TextureViewDesc srv_desc;
    srv_desc.ViewType = Diligent::TEXTURE_VIEW_SHADER_RESOURCE;
    srv_desc.TextureDim = Diligent::RESOURCE_DIM_TEX_2D;
    srv_desc.Format = Diligent::TEX_FORMAT_RGB10A2_UNORM;
    srv_desc.Name = "ImportedGuestOutputSRV";
    srv_desc.Swizzle = Diligent::TextureComponentMapping(
        Diligent::TEXTURE_COMPONENT_SWIZZLE_B,
        Diligent::TEXTURE_COMPONENT_SWIZZLE_G,
        Diligent::TEXTURE_COMPONENT_SWIZZLE_R,
        Diligent::TEXTURE_COMPONENT_SWIZZLE_A);
    img.texture->CreateView(srv_desc, &img.srv);
    if (!img.srv) {
      REXLOG_ERROR("NativeDevice: Failed to create swizzled SRV for imported texture");
      impl_->releaseImportedImage(img);
      return false;
    }
    if (tex_var) tex_var->Set(img.srv);

    img.w = width;
    img.h = height;
    REXLOG_INFO("NativeDevice: imported D3D12 shared texture ({}x{}, handle={:#x})",
                width, height, reinterpret_cast<uintptr_t>(shared_handle));
    img_it = impl_->imported_images.emplace(shared_handle, img).first;
  }
  const auto& imported = img_it->second;

  auto* rtv = impl_->swapchain->GetCurrentBackBufferRTV();
  auto* dsv = impl_->swapchain->GetDepthBufferDSV();
  impl_->context->SetRenderTargets(1, &rtv, dsv,
                                   Diligent::RESOURCE_STATE_TRANSITION_MODE_TRANSITION);

  const float clear_color[] = {0.0f, 0.0f, 0.0f, 1.0f};
  impl_->context->ClearRenderTarget(rtv, clear_color,
                                    Diligent::RESOURCE_STATE_TRANSITION_MODE_TRANSITION);

  impl_->setBlitViewport();

  impl_->context->SetPipelineState(impl_->blit_pipeline);
  impl_->context->CommitShaderResources(
      imported.srb, Diligent::RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
  Diligent::DrawAttribs da;
  da.NumVertices = 3;
  da.Flags = Diligent::DRAW_FLAG_VERIFY_ALL;
  impl_->context->Draw(da);

  const auto present_start = timings ? Clock::now() : Clock::time_point{};
  impl_->swapchain->Present(sync_interval);
  impl_->context->Flush();
  impl_->context->WaitForIdle();
  if (timings) {
    timings->upload_draw_cpu_ms =
        std::chrono::duration<double, std::milli>(present_start - upload_start).count();
    timings->present_cpu_ms =
        std::chrono::duration<double, std::milli>(Clock::now() - present_start).count();
  }
  return true;
#endif
}

void NativeDevice::resize(uint32_t width, uint32_t height) {
  if (!impl_->initialized) return;
  if (width == 0 || height == 0) return;
  impl_->swapchain->Resize(width, height,
                           Diligent::SURFACE_TRANSFORM_OPTIMAL);
}

void NativeDevice::shutdown() {
  if (impl_->context) {
    impl_->context->Flush();
    impl_->context->WaitForIdle();
    impl_->context->FinishFrame();
  }
  impl_->current_pipeline = nullptr;

  if (impl_->blit_srb) { impl_->blit_srb->Release(); impl_->blit_srb = nullptr; }
  if (impl_->blit_texture) { impl_->blit_texture->Release(); impl_->blit_texture = nullptr; }
  if (impl_->blit_sampler) { impl_->blit_sampler->Release(); impl_->blit_sampler = nullptr; }
  if (impl_->blit_pipeline) { impl_->blit_pipeline->Release(); impl_->blit_pipeline = nullptr; }
  impl_->blit_texture_w = 0;
  impl_->blit_texture_h = 0;

  for (auto& [handle, img] : impl_->imported_images) {
    impl_->releaseImportedImage(img);
  }
  impl_->imported_images.clear();
  impl_->ext_memory_supported = false;

  if (impl_->swapchain) { impl_->swapchain->Release(); impl_->swapchain = nullptr; }
  if (impl_->context) { impl_->context->Release(); impl_->context = nullptr; }
  if (impl_->device) { impl_->device->Release(); impl_->device = nullptr; }

  impl_->initialized = false;
}

bool NativeDevice::is_valid() const {
  return impl_ && impl_->initialized;
}

static Diligent::VALUE_TYPE toValueType(VertexFormat fmt) {
  switch (fmt) {
    case VertexFormat::Float32:
    case VertexFormat::Float32x2:
    case VertexFormat::Float32x3:
    case VertexFormat::Float32x4:
      return Diligent::VT_FLOAT32;
    case VertexFormat::UNorm8x4:
      return Diligent::VT_UINT8;
  }
  return Diligent::VT_FLOAT32;
}

static uint8_t componentCount(VertexFormat fmt) {
  switch (fmt) {
    case VertexFormat::Float32:   return 1;
    case VertexFormat::Float32x2: return 2;
    case VertexFormat::Float32x3: return 3;
    case VertexFormat::Float32x4: return 4;
    case VertexFormat::UNorm8x4:  return 4;
  }
  return 0;
}

static bool isNormalized(VertexFormat fmt) {
  return fmt == VertexFormat::UNorm8x4;
}

static Diligent::TEXTURE_FORMAT toTextureFormat(TextureFormat fmt) {
  switch (fmt) {
    case TextureFormat::RGBA8_UNorm:      return Diligent::TEX_FORMAT_RGBA8_UNORM;
    case TextureFormat::RGBA8_UNorm_sRGB: return Diligent::TEX_FORMAT_RGBA8_UNORM_SRGB;
    case TextureFormat::BGRA8_UNorm:      return Diligent::TEX_FORMAT_BGRA8_UNORM;
    case TextureFormat::BGRA8_UNorm_sRGB: return Diligent::TEX_FORMAT_BGRA8_UNORM_SRGB;
    case TextureFormat::R8_UNorm:         return Diligent::TEX_FORMAT_R8_UNORM;
    case TextureFormat::BC1_UNorm:        return Diligent::TEX_FORMAT_BC1_UNORM;
    case TextureFormat::BC3_UNorm:        return Diligent::TEX_FORMAT_BC3_UNORM;
    case TextureFormat::BC7_UNorm:        return Diligent::TEX_FORMAT_BC7_UNORM;
    default:                              return Diligent::TEX_FORMAT_RGBA8_UNORM;
  }
}

static Diligent::FILTER_TYPE toFilterType(TextureFilter f) {
  switch (f) {
    case TextureFilter::Point:       return Diligent::FILTER_TYPE_POINT;
    case TextureFilter::Linear:      return Diligent::FILTER_TYPE_LINEAR;
    case TextureFilter::Anisotropic: return Diligent::FILTER_TYPE_ANISOTROPIC;
    default:                         return Diligent::FILTER_TYPE_LINEAR;
  }
}

static Diligent::TEXTURE_ADDRESS_MODE toAddressMode(TextureAddress a) {
  switch (a) {
    case TextureAddress::Wrap:   return Diligent::TEXTURE_ADDRESS_WRAP;
    case TextureAddress::Clamp:  return Diligent::TEXTURE_ADDRESS_CLAMP;
    case TextureAddress::Mirror: return Diligent::TEXTURE_ADDRESS_MIRROR;
    default:                     return Diligent::TEXTURE_ADDRESS_WRAP;
  }
}

NativeBuffer* NativeDevice::createBuffer(const BufferDesc& desc,
                                          const void* initial_data) {
  if (!impl_->initialized) return nullptr;

  Diligent::BufferDesc bd;
  bd.Size = desc.size_bytes;

  if (desc.is_index_buffer) {
    bd.BindFlags = Diligent::BIND_INDEX_BUFFER;
  } else {
    bd.BindFlags = Diligent::BIND_VERTEX_BUFFER;
  }

  bd.Usage = Diligent::USAGE_IMMUTABLE;
  bd.CPUAccessFlags = Diligent::CPU_ACCESS_NONE;

  Diligent::BufferData bd_init;
  bd_init.pData = initial_data;
  bd_init.DataSize = desc.size_bytes;

  Diligent::IBuffer* buf = nullptr;
  impl_->device->CreateBuffer(bd, initial_data ? &bd_init : nullptr, &buf);
  if (!buf) {
    REXLOG_ERROR("NativeDevice: Failed to create buffer ({} bytes)", desc.size_bytes);
    return nullptr;
  }

  auto* handle = new NativeBuffer();
  handle->buffer = buf;
  return handle;
}

void NativeDevice::destroyBuffer(NativeBuffer* buffer) {
  if (!buffer) return;
  if (buffer->buffer) buffer->buffer->Release();
  delete buffer;
}

NativeBuffer* NativeDevice::createUniformBuffer(uint32_t size_bytes) {
  if (!impl_->initialized) return nullptr;

  Diligent::BufferDesc bd;
  bd.Size = size_bytes;
  bd.BindFlags = Diligent::BIND_UNIFORM_BUFFER;
  bd.Usage = Diligent::USAGE_DEFAULT;
  bd.CPUAccessFlags = Diligent::CPU_ACCESS_NONE;

  Diligent::IBuffer* buf = nullptr;
  impl_->device->CreateBuffer(bd, nullptr, &buf);
  if (!buf) {
    REXLOG_ERROR("NativeDevice: Failed to create uniform buffer ({} bytes)", size_bytes);
    return nullptr;
  }

  auto* handle = new NativeBuffer();
  handle->buffer = buf;
  return handle;
}

void NativeDevice::updateUniformBuffer(NativeBuffer* buffer, const void* data,
                                        uint32_t size_bytes) {
  if (!impl_->initialized || !buffer || !data) return;
  impl_->context->UpdateBuffer(buffer->buffer, 0, size_bytes, data,
                               Diligent::RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
}

void NativeDevice::bindUniformBuffer(uint8_t shader_stage, uint8_t binding_slot,
                                      NativeBuffer* buffer) {
  if (!impl_->initialized || !buffer || !impl_->current_pipeline) return;

  Diligent::SHADER_TYPE stage;
  switch (shader_stage) {
    case 0: stage = Diligent::SHADER_TYPE_VERTEX; break;
    case 1: stage = Diligent::SHADER_TYPE_PIXEL; break;
    case 2: stage = Diligent::SHADER_TYPE_VERTEX | Diligent::SHADER_TYPE_PIXEL; break;
    default: return;
  }

  static const char* kSlotNames[] = {
    "CameraCB",
    "MaterialCB",
    "ObjectCB",
    "SceneCB",
  };
  const char* name = (binding_slot < 4) ? kSlotNames[binding_slot] : nullptr;
  if (!name) return;

  auto* var = impl_->current_pipeline->GetStaticVariableByName(stage, name);
  if (var) {
    var->Set(buffer->buffer);
  }
}

NativeTexture* NativeDevice::createTexture(const TextureDesc& desc) {
  if (!impl_->initialized) return nullptr;

  Diligent::TextureDesc tex_desc;
  tex_desc.Type = Diligent::RESOURCE_DIM_TEX_2D;
  tex_desc.Width = desc.width;
  tex_desc.Height = desc.height;
  tex_desc.MipLevels = desc.mip_levels;
  tex_desc.Format = toTextureFormat(desc.format);
  tex_desc.BindFlags = Diligent::BIND_SHADER_RESOURCE;
  tex_desc.Usage = Diligent::USAGE_IMMUTABLE;
  tex_desc.Name = "NativeTexture";

  Diligent::TextureSubResData sub_res;
  sub_res.pData = desc.initial_data;
  sub_res.Stride = desc.width * 4;

  if (desc.format == TextureFormat::BC1_UNorm ||
      desc.format == TextureFormat::BC3_UNorm ||
      desc.format == TextureFormat::BC7_UNorm) {
    uint32_t block_size = (desc.format == TextureFormat::BC1_UNorm) ? 8 : 16;
    uint32_t blocks_x = (desc.width + 3) / 4;
    sub_res.Stride = blocks_x * block_size;
  } else if (desc.format == TextureFormat::R8_UNorm) {
    sub_res.Stride = desc.width;
  }

  Diligent::TextureData tex_data;
  tex_data.pContext = impl_->context;
  tex_data.NumSubresources = 1;
  tex_data.pSubResources = &sub_res;

  Diligent::ITexture* tex = nullptr;
  impl_->device->CreateTexture(tex_desc, desc.initial_data ? &tex_data : nullptr, &tex);
  if (!tex) {
    REXLOG_ERROR("NativeDevice: Failed to create texture ({}x{})",
                 desc.width, desc.height);
    return nullptr;
  }

  Diligent::ISampler* sampler = nullptr;
  Diligent::SamplerDesc sdesc;
  if (desc.use_custom_sampler) {
    sdesc.MinFilter = toFilterType(desc.sampler.min_filter);
    sdesc.MagFilter = toFilterType(desc.sampler.mag_filter);
    sdesc.MipFilter = toFilterType(desc.sampler.mip_filter);
    sdesc.AddressU = toAddressMode(desc.sampler.address_u);
    sdesc.AddressV = toAddressMode(desc.sampler.address_v);
    if (desc.sampler.max_anisotropy > 0) {
      sdesc.MinFilter = Diligent::FILTER_TYPE_ANISOTROPIC;
      sdesc.MagFilter = Diligent::FILTER_TYPE_ANISOTROPIC;
      sdesc.MipFilter = Diligent::FILTER_TYPE_ANISOTROPIC;
      sdesc.MaxAnisotropy = desc.sampler.max_anisotropy;
    }
  } else {
    sdesc.MinFilter = Diligent::FILTER_TYPE_LINEAR;
    sdesc.MagFilter = Diligent::FILTER_TYPE_LINEAR;
    sdesc.MipFilter = Diligent::FILTER_TYPE_LINEAR;
    sdesc.AddressU = Diligent::TEXTURE_ADDRESS_WRAP;
    sdesc.AddressV = Diligent::TEXTURE_ADDRESS_WRAP;
  }
  sdesc.Name = "NativeSampler";
  impl_->device->CreateSampler(sdesc, &sampler);

  auto* handle = new NativeTexture();
  handle->texture = tex;
  handle->sampler = sampler;
  return handle;
}

void NativeDevice::destroyTexture(NativeTexture* texture) {
  if (!texture) return;
  if (texture->sampler) texture->sampler->Release();
  if (texture->texture) texture->texture->Release();
  delete texture;
}

void NativeDevice::bindTexture(uint8_t shader_stage, uint8_t binding_slot,
                                NativeTexture* texture) {
  if (!impl_->initialized || !texture || !impl_->current_pipeline) return;

  Diligent::SHADER_TYPE stage;
  switch (shader_stage) {
    case 0: stage = Diligent::SHADER_TYPE_VERTEX; break;
    case 1: stage = Diligent::SHADER_TYPE_PIXEL; break;
    case 2: stage = Diligent::SHADER_TYPE_VERTEX | Diligent::SHADER_TYPE_PIXEL; break;
    default: return;
  }

  static const char* kTexSlotNames[] = {
    "g_Texture",
    "g_Texture2",
    "g_Texture3",
    "g_Texture4",
  };
  static const char* kSamplerSlotNames[] = {
    "g_Sampler",
    "g_Sampler2",
    "g_Sampler3",
    "g_Sampler4",
  };
  const char* tex_name = (binding_slot < 4) ? kTexSlotNames[binding_slot] : nullptr;
  const char* samp_name = (binding_slot < 4) ? kSamplerSlotNames[binding_slot] : nullptr;
  if (!tex_name) return;

  auto* sr_view = texture->texture->GetDefaultView(Diligent::TEXTURE_VIEW_SHADER_RESOURCE);
  if (sr_view) {
    auto* var = impl_->current_pipeline->GetStaticVariableByName(stage, tex_name);
    if (var) var->Set(sr_view);
  }

  if (texture->sampler && samp_name) {
    auto* samp_var = impl_->current_pipeline->GetStaticVariableByName(stage, samp_name);
    if (samp_var) samp_var->Set(texture->sampler);
  }
}

NativePipeline* NativeDevice::createPipeline(const PipelineDesc& desc) {
  if (!impl_->initialized) return nullptr;

  Diligent::IShader* vs = createShader(
      impl_->device, desc.vertex_shader_source, Diligent::SHADER_TYPE_VERTEX,
      "NativeVS");
  if (!vs) {
    REXLOG_ERROR("NativeDevice: Failed to compile vertex shader");
    return nullptr;
  }

  Diligent::IShader* ps = createShader(
      impl_->device, desc.pixel_shader_source, Diligent::SHADER_TYPE_PIXEL,
      "NativePS");
  if (!ps) {
    REXLOG_ERROR("NativeDevice: Failed to compile pixel shader");
    vs->Release();
    return nullptr;
  }

  std::vector<Diligent::LayoutElement> layout_elems;
  layout_elems.reserve(desc.vertex_element_count);
  for (uint32_t i = 0; i < desc.vertex_element_count; i++) {
    const auto& ve = desc.vertex_elements[i];
    Diligent::LayoutElement le;
    le.InputIndex = i;
    le.BufferSlot = ve.binding;
    le.NumComponents = componentCount(ve.format);
    le.ValueType = toValueType(ve.format);
    le.IsNormalized = isNormalized(ve.format) ? Diligent::True : Diligent::False;
    le.RelativeOffset = ve.offset;
    le.Stride = 0;
    layout_elems.push_back(le);
  }

  if (!layout_elems.empty()) {
    uint32_t max_offset = 0;
    for (uint32_t i = 0; i < desc.vertex_element_count; i++) {
      const auto& ve = desc.vertex_elements[i];
      uint32_t elem_size = 0;
      switch (ve.format) {
        case VertexFormat::Float32:   elem_size = 4; break;
        case VertexFormat::Float32x2: elem_size = 8; break;
        case VertexFormat::Float32x3: elem_size = 12; break;
        case VertexFormat::Float32x4: elem_size = 16; break;
        case VertexFormat::UNorm8x4:  elem_size = 4; break;
      }
      max_offset = (std::max)(max_offset, static_cast<uint32_t>(ve.offset) + elem_size);
    }
    for (auto& le : layout_elems) {
      le.Stride = max_offset;
    }
  }

  Diligent::GraphicsPipelineStateCreateInfo pso_ci;
  pso_ci.PSODesc.Name = "NativePipeline";
  pso_ci.PSODesc.PipelineType = Diligent::PIPELINE_TYPE_GRAPHICS;

  pso_ci.pVS = vs;
  pso_ci.pPS = ps;

  const auto& sc_desc = impl_->swapchain->GetDesc();
  pso_ci.GraphicsPipeline.NumRenderTargets = 1;
  pso_ci.GraphicsPipeline.RTVFormats[0] = sc_desc.ColorBufferFormat;
  pso_ci.GraphicsPipeline.DSVFormat = sc_desc.DepthBufferFormat;

  switch (desc.topology) {
    case PrimitiveTopology::TriangleList:
      pso_ci.GraphicsPipeline.PrimitiveTopology = Diligent::PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
      break;
    case PrimitiveTopology::TriangleStrip:
      pso_ci.GraphicsPipeline.PrimitiveTopology = Diligent::PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
      break;
    case PrimitiveTopology::LineList:
      pso_ci.GraphicsPipeline.PrimitiveTopology = Diligent::PRIMITIVE_TOPOLOGY_LINE_LIST;
      break;
  }

  pso_ci.GraphicsPipeline.DepthStencilDesc.DepthEnable = Diligent::False;

  auto& rt0 = pso_ci.GraphicsPipeline.BlendDesc.RenderTargets[0];
  rt0.BlendEnable = Diligent::False;
  rt0.RenderTargetWriteMask = Diligent::COLOR_MASK_ALL;

  pso_ci.GraphicsPipeline.InputLayout.LayoutElements =
      layout_elems.data();
  pso_ci.GraphicsPipeline.InputLayout.NumElements =
      static_cast<uint32_t>(layout_elems.size());

  pso_ci.PSODesc.ResourceLayout.DefaultVariableType =
      Diligent::SHADER_RESOURCE_VARIABLE_TYPE_STATIC;

  Diligent::IPipelineState* pso = nullptr;
  impl_->device->CreateGraphicsPipelineState(pso_ci, &pso);
  if (!pso) {
    REXLOG_ERROR("NativeDevice: Failed to create pipeline state");
    vs->Release();
    ps->Release();
    return nullptr;
  }

  vs->Release();
  ps->Release();

  auto* handle = new NativePipeline();
  handle->pipeline = pso;
  return handle;
}

void NativeDevice::destroyPipeline(NativePipeline* pipeline) {
  if (!pipeline) return;
  if (pipeline->pipeline) pipeline->pipeline->Release();
  delete pipeline;
}

void NativeDevice::bindPipeline(NativePipeline* pipeline) {
  if (!impl_->initialized || !pipeline) return;
  impl_->context->SetPipelineState(pipeline->pipeline);
  impl_->current_pipeline = pipeline->pipeline;
}

void NativeDevice::bindVertexBuffer(uint32_t binding, NativeBuffer* buffer,
                                     uint32_t stride) {
  if (!impl_->initialized || !buffer) return;
  Diligent::IBuffer* buf = buffer->buffer;
  uint64_t offset = 0;
  impl_->context->SetVertexBuffers(binding, 1, &buf, &offset,
                                   Diligent::RESOURCE_STATE_TRANSITION_MODE_TRANSITION,
                                   Diligent::SET_VERTEX_BUFFERS_FLAG_RESET);
}

void NativeDevice::bindIndexBuffer(NativeBuffer* buffer) {
  if (!impl_->initialized || !buffer) return;
  impl_->context->SetIndexBuffer(buffer->buffer, 0,
                                 Diligent::RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
}

void NativeDevice::draw(uint32_t vertex_count, uint32_t first_vertex) {
  if (!impl_->initialized) return;
  Diligent::DrawAttribs da;
  da.NumVertices = vertex_count;
  da.StartVertexLocation = first_vertex;
  da.Flags = Diligent::DRAW_FLAG_VERIFY_ALL;
  impl_->context->Draw(da);
}

void NativeDevice::drawIndexed(uint32_t index_count, uint32_t first_index,
                                uint32_t vertex_offset) {
  if (!impl_->initialized) return;
  Diligent::DrawIndexedAttribs da;
  da.NumIndices = index_count;
  da.FirstIndexLocation = first_index;
  da.BaseVertex = vertex_offset;
  da.IndexType = Diligent::VT_UINT16;
  da.Flags = Diligent::DRAW_FLAG_VERIFY_ALL;
  impl_->context->DrawIndexed(da);
}

}
