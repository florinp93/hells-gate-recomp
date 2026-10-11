#include "vulkan_device.h"

#if defined(_WIN32)
#include <windows.h>
#endif

#include "Graphics/GraphicsEngineVulkan/interface/EngineFactoryVk.h"
#include "Graphics/GraphicsEngine/interface/RenderDevice.h"
#include "Graphics/GraphicsEngine/interface/DeviceContext.h"
#include "Graphics/GraphicsEngine/interface/SwapChain.h"
#include "Graphics/GraphicsEngine/interface/PipelineState.h"
#include "Graphics/GraphicsEngine/interface/Shader.h"
#include "Graphics/GraphicsEngine/interface/ShaderResourceBinding.h"
#include "Graphics/GraphicsEngine/interface/ShaderResourceVariable.h"
#include "Graphics/GraphicsEngine/interface/Texture.h"
#include "Graphics/GraphicsEngine/interface/TextureView.h"
#include "Graphics/GraphicsEngine/interface/Sampler.h"
#include "Graphics/GraphicsAccessories/interface/GraphicsAccessories.hpp"
#include "Primitives/interface/DebugOutput.h"
#include "Common/interface/RefCntAutoPtr.hpp"

#include <rex/logging/macros.h>

#include <glslang/Public/ShaderLang.h>
#include <glslang/Include/ResourceLimits.h>
#include <SPIRV/GlslangToSpv.h>

#include <algorithm>
#include <cmath>
#include <vector>

// DiligentCore is built without glslang (DILIGENT_NO_GLSLANG): its vendored
// glslang/SPIRV-Tools collide with the SDK's equivalents by target name.
// Shaders are compiled to SPIR-V here with the SDK's glslang instead.
// DefaultTBuiltInResource lives in StandAlone/ResourceLimits.cpp (compiled
// into this target); glslang's public headers only declare TBuiltInResource.
namespace glslang {
extern const TBuiltInResource DefaultTBuiltInResource;
}

namespace renderer {

struct VulkanDevice::Impl {
  Diligent::IEngineFactoryVk* factory = nullptr;
  Diligent::IRenderDevice* device = nullptr;
  Diligent::IDeviceContext* context = nullptr;
  Diligent::ISwapChain* swapchain = nullptr;
  bool initialized = false;

  Diligent::IPipelineState* blit_pipeline = nullptr;
  Diligent::ISampler* blit_sampler = nullptr;

  void* hwnd = nullptr;
  double display_aspect = 0.0;
  bool letterbox = true;
  uint32_t logged_width = 0;
  uint32_t logged_height = 0;
  double logged_aspect = -1.0;
  bool logged_letterbox = false;

  void SetBlitViewport() {
    const auto& desc = swapchain->GetDesc();
    uint32_t width = desc.Width;
    uint32_t height = desc.Height;
    if (letterbox && std::isfinite(display_aspect) && display_aspect > 0.0) {
      if (display_aspect > double(width) / height) {
        height = uint32_t(std::clamp(std::round(width / display_aspect), 1.0, double(height)));
      } else {
        width = uint32_t(std::clamp(std::round(height * display_aspect), 1.0, double(width)));
      }
    }
    const uint32_t x = (desc.Width - width) / 2;
    const uint32_t y = (desc.Height - height) / 2;
    Diligent::Viewport viewport;
    viewport.TopLeftX = float(x);
    viewport.TopLeftY = float(y);
    viewport.Width = float(width);
    viewport.Height = float(height);
    Diligent::Rect scissor{int32_t(x), int32_t(y), int32_t(x + width), int32_t(y + height)};
    context->SetViewports(1, &viewport, desc.Width, desc.Height);
    context->SetScissorRects(1, &scissor, desc.Width, desc.Height);
    if (logged_width != desc.Width || logged_height != desc.Height ||
        logged_aspect != display_aspect || logged_letterbox != letterbox) {
      REXLOG_INFO(
          "VulkanDevice: presentation window={}x{} target_aspect={:.6f} "
          "letterbox={} viewport=({},{}) {}x{}",
          desc.Width, desc.Height, display_aspect, letterbox, x, y, width, height);
      logged_width = desc.Width;
      logged_height = desc.Height;
      logged_aspect = display_aspect;
      logged_letterbox = letterbox;
    }
  }
};

VulkanDevice::VulkanDevice() : impl_(std::make_unique<Impl>()) {}

VulkanDevice::~VulkanDevice() { Shutdown(); }

bool VulkanDevice::Initialize(void* hwnd, uint32_t width, uint32_t height) {
  Shutdown();

  Diligent::SetDebugMessageCallback([](Diligent::DEBUG_MESSAGE_SEVERITY severity,
                                       const char* message, const char*, const char*, int) {
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
    REXLOG_ERROR("VulkanDevice: Failed to get Vulkan engine factory");
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

  impl_->hwnd = hwnd;
#if defined(_WIN32)
  RECT client;
  if (GetClientRect(static_cast<HWND>(hwnd), &client) && client.right > 0 && client.bottom > 0) {
    width = uint32_t(client.right);
    height = uint32_t(client.bottom);
  }
  REXLOG_INFO("VulkanDevice: hwnd={} client={}x{} swapchain {}x{} visible={}", fmt::ptr(hwnd),
              client.right, client.bottom, width, height,
              hwnd ? IsWindowVisible(static_cast<HWND>(hwnd)) : false);
#endif

  impl_->factory->CreateDeviceAndContextsVk(engine_ci, &impl_->device, &impl_->context);
  if (!impl_->device || !impl_->context) {
    REXLOG_ERROR("VulkanDevice: Failed to create Vulkan device/context");
    Shutdown();
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

  impl_->factory->CreateSwapChainVk(impl_->device, impl_->context, sc_desc, native_window,
                                    &impl_->swapchain);
  if (!impl_->swapchain) {
    REXLOG_ERROR("VulkanDevice: Failed to create Vulkan swapchain");
    Shutdown();
    return false;
  }

  if (!CreateBlitPipeline()) {
    Shutdown();
    return false;
  }

  impl_->initialized = true;
  REXLOG_INFO(
      "VulkanDevice: initialized, swap chain {}x{} {}", width, height,
      Diligent::GetTextureFormatAttribs(impl_->swapchain->GetDesc().ColorBufferFormat).Name);
  return true;
}

void VulkanDevice::BeginFrame() {
  if (!impl_->initialized) return;

  auto* rtv = impl_->swapchain->GetCurrentBackBufferRTV();
  auto* dsv = impl_->swapchain->GetDepthBufferDSV();
  impl_->context->SetRenderTargets(1, &rtv, dsv,
                                   Diligent::RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
}

void VulkanDevice::Clear(float r, float g, float b, float a) {
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

void VulkanDevice::Present(uint32_t sync_interval) {
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

static bool CompileGlslToSpirv(const char* source, EShLanguage stage,
                               std::vector<uint32_t>& out_spirv) {
  static const bool glslang_ready = [] { return glslang::InitializeProcess(); }();
  if (!glslang_ready) {
    REXLOG_ERROR("VulkanDevice: glslang InitializeProcess failed");
    return false;
  }

  glslang::TShader shader(stage);
  const char* strings[] = {source};
  shader.setStrings(strings, 1);
  shader.setEntryPoint("main");
  shader.setSourceEntryPoint("main");
  shader.setEnvInput(glslang::EShSourceGlsl, stage, glslang::EShClientVulkan, 450);
  shader.setEnvClient(glslang::EShClientVulkan, glslang::EShTargetVulkan_1_0);
  shader.setEnvTarget(glslang::EShTargetSpv, glslang::EShTargetSpv_1_0);

  const auto messages = EShMessages(EShMsgSpvRules | EShMsgVulkanRules);
  if (!shader.parse(&glslang::DefaultTBuiltInResource, 450, false, messages)) {
    REXLOG_ERROR("VulkanDevice: glslang parse failed: {}",
                 shader.getInfoLog() ? shader.getInfoLog() : "");
    return false;
  }
  glslang::TProgram program;
  program.addShader(&shader);
  if (!program.link(messages)) {
    REXLOG_ERROR("VulkanDevice: glslang link failed: {}",
                 program.getInfoLog() ? program.getInfoLog() : "");
    return false;
  }
  glslang::GlslangToSpv(*program.getIntermediate(stage), out_spirv);
  return !out_spirv.empty();
}

static Diligent::IShader* CreateShader(Diligent::IRenderDevice* device, const char* source,
                                       Diligent::SHADER_TYPE type, const char* name) {
  std::vector<uint32_t> spirv;
  if (!CompileGlslToSpirv(
          source, type == Diligent::SHADER_TYPE_VERTEX ? EShLangVertex : EShLangFragment, spirv)) {
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

bool VulkanDevice::CreateBlitPipeline() {
  if (!impl_->blit_pipeline) {
    Diligent::IShader* vs =
        CreateShader(impl_->device, kBlitVS, Diligent::SHADER_TYPE_VERTEX, "BlitVS");
    if (!vs) {
      REXLOG_ERROR("VulkanDevice: Failed to compile blit VS");
      return false;
    }

    Diligent::IShader* ps =
        CreateShader(impl_->device, kBlitPS, Diligent::SHADER_TYPE_PIXEL, "BlitPS");
    if (!ps) {
      REXLOG_ERROR("VulkanDevice: Failed to compile blit PS");
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
    pso_ci.GraphicsPipeline.PrimitiveTopology = Diligent::PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
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
        {Diligent::SHADER_TYPE_PIXEL, "g_Texture", Diligent::SHADER_RESOURCE_VARIABLE_TYPE_MUTABLE},
    };
    pso_ci.PSODesc.ResourceLayout.NumVariables = 1;
    pso_ci.PSODesc.ResourceLayout.Variables = vars;

    impl_->device->CreateGraphicsPipelineState(pso_ci, &impl_->blit_pipeline);
    vs->Release();
    ps->Release();

    if (!impl_->blit_pipeline) {
      REXLOG_ERROR("VulkanDevice: Failed to create blit pipeline");
      return false;
    }

    auto* samp_var =
        impl_->blit_pipeline->GetStaticVariableByName(Diligent::SHADER_TYPE_PIXEL, "g_Sampler");
    if (samp_var && impl_->blit_sampler)
      samp_var->Set(impl_->blit_sampler);
    else {
      REXLOG_ERROR("VulkanDevice: blit sampler static var not found or null sampler");
      return false;
    }

    REXLOG_INFO("VulkanDevice: blit pipeline created (GLSL->SPIRV, cull=none)");
  }
  return true;
}

bool VulkanDevice::PresentTexture(Diligent::ITextureView* srv, uint32_t sync_interval) {
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
  impl_->SetBlitViewport();
  impl_->context->SetPipelineState(impl_->blit_pipeline);
  impl_->context->CommitShaderResources(srb, Diligent::RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
  Diligent::DrawAttribs da;
  da.NumVertices = 3;
  impl_->context->Draw(da);
  srb->Release();
  impl_->swapchain->Present(sync_interval);
  return true;
}

Diligent::IPipelineState* VulkanDevice::CreateFullscreenPipeline(const char* name,
                                                                 const char* pixel_source,
                                                                 Diligent::TEXTURE_FORMAT format,
                                                                 bool scissor) {
  static constexpr char kFullscreenVS[] = R"(#version 450
void main() {
  uint id = uint(gl_VertexIndex);
  gl_Position = vec4(id == 1u ? 3.0 : -1.0, id == 2u ? 3.0 : -1.0, 0.0, 1.0);
}
)";
  Diligent::RefCntAutoPtr<Diligent::IShader> vs, ps;
  vs.Attach(CreateShader(impl_->device, kFullscreenVS, Diligent::SHADER_TYPE_VERTEX, name));
  ps.Attach(CreateShader(impl_->device, pixel_source, Diligent::SHADER_TYPE_PIXEL, name));
  if (!vs || !ps) return nullptr;
  Diligent::GraphicsPipelineStateCreateInfo ci;
  ci.PSODesc.Name = name;
  ci.PSODesc.ResourceLayout.DefaultVariableType = Diligent::SHADER_RESOURCE_VARIABLE_TYPE_DYNAMIC;
  ci.pVS = vs;
  ci.pPS = ps;
  ci.GraphicsPipeline.NumRenderTargets = 1;
  ci.GraphicsPipeline.RTVFormats[0] = format;
  ci.GraphicsPipeline.PrimitiveTopology = Diligent::PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
  ci.GraphicsPipeline.RasterizerDesc.CullMode = Diligent::CULL_MODE_NONE;
  ci.GraphicsPipeline.RasterizerDesc.ScissorEnable = scissor ? Diligent::True : Diligent::False;
  ci.GraphicsPipeline.DepthStencilDesc.DepthEnable = Diligent::False;
  Diligent::IPipelineState* pso = nullptr;
  impl_->device->CreateGraphicsPipelineState(ci, &pso);
  return pso;
}

Diligent::IRenderDevice* VulkanDevice::RenderDevice() const { return impl_->device; }
Diligent::IDeviceContext* VulkanDevice::Context() const { return impl_->context; }

void VulkanDevice::SetDisplayAspect(double aspect, bool letterbox) {
  if (!impl_->initialized) return;
  impl_->display_aspect = aspect;
  impl_->letterbox = letterbox;
}

bool VulkanDevice::UpdateWindowSize() {
  if (!impl_->initialized) return false;
#if defined(_WIN32)
  const auto hwnd = static_cast<HWND>(impl_->hwnd);
  RECT client;
  if (IsIconic(hwnd) || !GetClientRect(hwnd, &client) || client.right <= 0 || client.bottom <= 0)
    return false;
  const auto& desc = impl_->swapchain->GetDesc();
  if (desc.Width != uint32_t(client.right) || desc.Height != uint32_t(client.bottom)) {
    Resize(uint32_t(client.right), uint32_t(client.bottom));
    REXLOG_INFO("VulkanDevice: client resized to {}x{}", client.right, client.bottom);
  }
#endif
  return true;
}

void VulkanDevice::Resize(uint32_t width, uint32_t height) {
  if (!impl_->initialized) return;
  if (width == 0 || height == 0) return;
  impl_->swapchain->Resize(width, height, Diligent::SURFACE_TRANSFORM_OPTIMAL);
}

void VulkanDevice::Shutdown() {
  if (impl_->context) {
    impl_->context->Flush();
    impl_->context->WaitForIdle();
    impl_->context->FinishFrame();
  }
  if (impl_->blit_sampler) {
    impl_->blit_sampler->Release();
    impl_->blit_sampler = nullptr;
  }
  if (impl_->blit_pipeline) {
    impl_->blit_pipeline->Release();
    impl_->blit_pipeline = nullptr;
  }

  if (impl_->swapchain) {
    impl_->swapchain->Release();
    impl_->swapchain = nullptr;
  }
  if (impl_->context) {
    impl_->context->Release();
    impl_->context = nullptr;
  }
  if (impl_->device) {
    impl_->device->Release();
    impl_->device = nullptr;
  }

  impl_->initialized = false;
}

}  // namespace renderer
