#include "dante_smaa.h"

#include "native_device.h"

#include "Common/interface/RefCntAutoPtr.hpp"
#include "Graphics/GraphicsEngine/interface/Buffer.h"
#include "Graphics/GraphicsEngine/interface/DeviceContext.h"
#include "Graphics/GraphicsEngine/interface/PipelineState.h"
#include "Graphics/GraphicsEngine/interface/RenderDevice.h"
#include "Graphics/GraphicsEngine/interface/Sampler.h"
#include "Graphics/GraphicsEngine/interface/ShaderResourceBinding.h"
#include "Graphics/GraphicsEngine/interface/Texture.h"
#include "Graphics/GraphicsEngine/interface/TextureView.h"
#include "Graphics/GraphicsTools/interface/MapHelper.hpp"

#include <rex/logging/macros.h>

#include <cstdint>
#include <string>
#include <unordered_map>

#include "../../thirdparty/smaa/Textures/AreaTex.h"
#include "../../thirdparty/smaa/Textures/SearchTex.h"

namespace native {
namespace {

namespace dl = Diligent;

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wc23-extensions"
constexpr char kSmaaSource[] = {
#embed "../../thirdparty/smaa/SMAA.hlsl"
    , 0};
#pragma clang diagnostic pop

// SMAA's custom shading-language hooks: separate textures and samplers.
constexpr char kPrelude[] = R"(#version 450
#define SMAA_CUSTOM_SL 1
#define SMAA_PRESET_HIGH 1
layout(std140, binding = 0) uniform SmaaConstants { vec4 g_RtMetrics; };
layout(binding = 1) uniform sampler g_Linear;
layout(binding = 2) uniform sampler g_Point;
#define SMAA_RT_METRICS g_RtMetrics
#define SMAATexture2D(tex) texture2D tex
#define SMAATexturePass2D(tex) tex
#define SMAASampleLevelZero(tex, coord) textureLod(sampler2D(tex, g_Linear), coord, 0.0)
#define SMAASampleLevelZeroPoint(tex, coord) textureLod(sampler2D(tex, g_Point), coord, 0.0)
#define SMAASampleLevelZeroOffset(tex, coord, offset) textureLodOffset(sampler2D(tex, g_Linear), coord, 0.0, offset)
#define SMAASample(tex, coord) texture(sampler2D(tex, g_Linear), coord)
#define SMAASamplePoint(tex, coord) texture(sampler2D(tex, g_Point), coord)
#define SMAASampleOffset(tex, coord, offset) texture(sampler2D(tex, g_Linear), coord, offset)
#define SMAAGather(tex, coord) textureGather(sampler2D(tex, g_Linear), coord)
#define SMAA_FLATTEN
#define SMAA_BRANCH
#define lerp(a, b, t) mix(a, b, t)
#define saturate(a) clamp(a, 0.0, 1.0)
#define mad(a, b, c) fma(a, b, c)
#define float2 vec2
#define float3 vec3
#define float4 vec4
#define int2 ivec2
#define int3 ivec3
#define int4 ivec4
#define bool2 bvec2
#define bool3 bvec3
#define bool4 bvec4
)";

// One triangle covering the target. The pixel shaders derive their texture
// coordinates from gl_FragCoord (top-left origin), so viewport orientation
// does not matter.
constexpr char kFullscreenVS[] = R"(#version 450
void main() {
  uint id = uint(gl_VertexIndex);
  gl_Position = vec4(id == 1u ? 3.0 : -1.0, id == 2u ? 3.0 : -1.0, 0.0, 1.0);
}
)";

constexpr char kEdgesPS[] = R"(
layout(binding = 3) uniform texture2D g_Color;
layout(location = 0) out vec2 out_edges;
void main() {
  vec2 uv = gl_FragCoord.xy * g_RtMetrics.xy;
  vec4 offset[3];
  SMAAEdgeDetectionVS(uv, offset);
  out_edges = SMAALumaEdgeDetectionPS(uv, offset, g_Color);
}
)";

constexpr char kWeightsPS[] = R"(
layout(binding = 3) uniform texture2D g_Edges;
layout(binding = 4) uniform texture2D g_Area;
layout(binding = 5) uniform texture2D g_Search;
layout(location = 0) out vec4 out_weights;
void main() {
  vec2 uv = gl_FragCoord.xy * g_RtMetrics.xy;
  vec2 pixcoord;
  vec4 offset[3];
  SMAABlendingWeightCalculationVS(uv, pixcoord, offset);
  out_weights = SMAABlendingWeightCalculationPS(uv, pixcoord, offset, g_Edges, g_Area, g_Search,
                                                vec4(0.0));
}
)";

constexpr char kBlendPS[] = R"(
layout(binding = 3) uniform texture2D g_Color;
layout(binding = 4) uniform texture2D g_Weights;
layout(location = 0) out vec4 out_color;
void main() {
  vec2 uv = gl_FragCoord.xy * g_RtMetrics.xy;
  vec4 offset;
  SMAANeighborhoodBlendingVS(uv, offset);
  out_color = SMAANeighborhoodBlendingPS(uv, offset, g_Color, g_Weights);
}
)";

std::string PixelSource(const char* main) {
  return std::string(kPrelude) + kSmaaSource + main;
}

struct Pass {
  dl::RefCntAutoPtr<dl::IPipelineState> pso;
  dl::RefCntAutoPtr<dl::IShaderResourceBinding> srb;

  void Set(const char* name, dl::IDeviceObject* object) {
    if (auto* variable = srb->GetVariableByName(dl::SHADER_TYPE_PIXEL, name)) variable->Set(object);
  }
};

}  // namespace

struct SmaaPass::Impl {
  dante::NativeDevice* device = nullptr;
  bool failed = false;
  dl::RefCntAutoPtr<dl::IShader> vs;
  dl::RefCntAutoPtr<dl::IBuffer> constants;
  dl::RefCntAutoPtr<dl::ISampler> linear, point;
  dl::RefCntAutoPtr<dl::ITexture> area, search;
  Pass edges, weights;
  std::unordered_map<int, Pass> blend;  // by color format

  // Per target size and format.
  uint32_t width = 0, height = 0;
  dl::TEXTURE_FORMAT format = dl::TEX_FORMAT_UNKNOWN;
  dl::RefCntAutoPtr<dl::ITexture> edges_tex, weights_tex, copy_tex;

  dl::IRenderDevice* render_device() const { return device->renderDevice(); }

  bool MakePass(Pass& pass, const char* name, const char* main, dl::TEXTURE_FORMAT target) {
    std::string source = PixelSource(main);
    dl::RefCntAutoPtr<dl::IShader> ps;
    ps.Attach(device->createGlslShader(source.c_str(), true, name));
    if (!ps) return false;
    dl::GraphicsPipelineStateCreateInfo ci;
    ci.PSODesc.Name = name;
    ci.PSODesc.ResourceLayout.DefaultVariableType = dl::SHADER_RESOURCE_VARIABLE_TYPE_DYNAMIC;
    ci.pVS = vs;
    ci.pPS = ps;
    ci.GraphicsPipeline.NumRenderTargets = 1;
    ci.GraphicsPipeline.RTVFormats[0] = target;
    ci.GraphicsPipeline.PrimitiveTopology = dl::PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    ci.GraphicsPipeline.RasterizerDesc.CullMode = dl::CULL_MODE_NONE;
    ci.GraphicsPipeline.DepthStencilDesc.DepthEnable = dl::False;
    render_device()->CreateGraphicsPipelineState(ci, &pass.pso);
    if (!pass.pso) return false;
    pass.pso->CreateShaderResourceBinding(&pass.srb, true);
    pass.Set("SmaaConstants", constants);
    pass.Set("g_Linear", linear);
    pass.Set("g_Point", point);
    return true;
  }

  dl::RefCntAutoPtr<dl::ITexture> MakeLookup(const char* name, uint32_t w, uint32_t h,
                                             dl::TEXTURE_FORMAT fmt, const void* data,
                                             uint32_t pitch) {
    dl::TextureDesc desc;
    desc.Name = name;
    desc.Type = dl::RESOURCE_DIM_TEX_2D;
    desc.Width = w;
    desc.Height = h;
    desc.Format = fmt;
    desc.BindFlags = dl::BIND_SHADER_RESOURCE;
    desc.Usage = dl::USAGE_IMMUTABLE;
    dl::TextureSubResData sub;
    sub.pData = data;
    sub.Stride = pitch;
    dl::TextureData init{&sub, 1};
    dl::RefCntAutoPtr<dl::ITexture> texture;
    render_device()->CreateTexture(desc, &init, &texture);
    return texture;
  }

  bool Init() {
    vs.Attach(device->createGlslShader(kFullscreenVS, false, "SmaaVS"));
    dl::BufferDesc cb;
    cb.Name = "SmaaConstants";
    cb.Size = 16;
    cb.Usage = dl::USAGE_DYNAMIC;
    cb.BindFlags = dl::BIND_UNIFORM_BUFFER;
    cb.CPUAccessFlags = dl::CPU_ACCESS_WRITE;
    render_device()->CreateBuffer(cb, nullptr, &constants);
    dl::SamplerDesc linear_desc;
    linear_desc.MinFilter = linear_desc.MagFilter = linear_desc.MipFilter = dl::FILTER_TYPE_LINEAR;
    linear_desc.AddressU = linear_desc.AddressV = dl::TEXTURE_ADDRESS_CLAMP;
    render_device()->CreateSampler(linear_desc, &linear);
    dl::SamplerDesc point_desc = linear_desc;
    point_desc.MinFilter = point_desc.MagFilter = point_desc.MipFilter = dl::FILTER_TYPE_POINT;
    render_device()->CreateSampler(point_desc, &point);
    area = MakeLookup("SmaaArea", AREATEX_WIDTH, AREATEX_HEIGHT, dl::TEX_FORMAT_RG8_UNORM,
                      areaTexBytes, AREATEX_PITCH);
    search = MakeLookup("SmaaSearch", SEARCHTEX_WIDTH, SEARCHTEX_HEIGHT, dl::TEX_FORMAT_R8_UNORM,
                        searchTexBytes, SEARCHTEX_PITCH);
    if (!vs || !constants || !linear || !point || !area || !search) return false;
    if (!MakePass(edges, "SmaaEdges", kEdgesPS, dl::TEX_FORMAT_RG8_UNORM)) return false;
    if (!MakePass(weights, "SmaaWeights", kWeightsPS, dl::TEX_FORMAT_RGBA8_UNORM)) return false;
    weights.Set("g_Area", area->GetDefaultView(dl::TEXTURE_VIEW_SHADER_RESOURCE));
    weights.Set("g_Search", search->GetDefaultView(dl::TEXTURE_VIEW_SHADER_RESOURCE));
    return true;
  }

  dl::RefCntAutoPtr<dl::ITexture> MakeTarget(const char* name, dl::TEXTURE_FORMAT fmt,
                                             dl::BIND_FLAGS bind) {
    dl::TextureDesc desc;
    desc.Name = name;
    desc.Type = dl::RESOURCE_DIM_TEX_2D;
    desc.Width = width;
    desc.Height = height;
    desc.Format = fmt;
    desc.BindFlags = bind;
    desc.Usage = dl::USAGE_DEFAULT;
    dl::RefCntAutoPtr<dl::ITexture> texture;
    render_device()->CreateTexture(desc, nullptr, &texture);
    return texture;
  }

  bool Prepare(const dl::TextureDesc& color) {
    if (color.Width == width && color.Height == height && color.Format == format) return true;
    width = color.Width;
    height = color.Height;
    format = color.Format;
    const auto rt = dl::BIND_RENDER_TARGET | dl::BIND_SHADER_RESOURCE;
    edges_tex = MakeTarget("SmaaEdgesTex", dl::TEX_FORMAT_RG8_UNORM, rt);
    weights_tex = MakeTarget("SmaaWeightsTex", dl::TEX_FORMAT_RGBA8_UNORM, rt);
    copy_tex = MakeTarget("SmaaColorCopy", format, dl::BIND_SHADER_RESOURCE);
    if (!edges_tex || !weights_tex || !copy_tex) {
      width = height = 0;
      return false;
    }
    if (!blend.count(format)) {
      if (!MakePass(blend[format], "SmaaBlend", kBlendPS, format)) {
        blend.erase(format);
        return false;
      }
    }
    return true;
  }

  void Run(dl::IDeviceContext* context, Pass& pass, dl::ITexture* target, bool clear) {
    dl::ITextureView* rtv = target->GetDefaultView(dl::TEXTURE_VIEW_RENDER_TARGET);
    context->SetRenderTargets(1, &rtv, nullptr, dl::RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
    if (clear) {
      const float zero[4] = {};
      context->ClearRenderTarget(rtv, zero, dl::RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
    }
    dl::Viewport vp;
    vp.Width = float(width);
    vp.Height = float(height);
    context->SetViewports(1, &vp, width, height);
    context->SetPipelineState(pass.pso);
    context->CommitShaderResources(pass.srb, dl::RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
    dl::DrawAttribs draw;
    draw.NumVertices = 3;
    context->Draw(draw);
  }
};

SmaaPass::SmaaPass(dante::NativeDevice* device) : impl_(std::make_unique<Impl>()) {
  impl_->device = device;
  if (!impl_->Init()) {
    impl_->failed = true;
    REXLOG_ERROR("SMAA: initialization failed, anti-aliasing disabled");
  }
}

SmaaPass::~SmaaPass() = default;

dl::ITexture* SmaaPass::Edges() const { return impl_->edges_tex; }

bool SmaaPass::Apply(dl::ITexture* color) {
  Impl& m = *impl_;
  if (m.failed || !color) return false;
  const dl::TextureDesc& desc = color->GetDesc();
  if (!m.Prepare(desc)) {
    REXLOG_WARN("SMAA: no pass for a {}x{} target (format {})", desc.Width, desc.Height,
                int(desc.Format));
    return false;
  }
  dl::IDeviceContext* context = m.device->immediateContext();
  {
    dl::MapHelper<float> map(context, m.constants, dl::MAP_WRITE, dl::MAP_FLAG_DISCARD);
    map[0] = 1.0f / float(m.width);
    map[1] = 1.0f / float(m.height);
    map[2] = float(m.width);
    map[3] = float(m.height);
  }
  dl::CopyTextureAttribs copy(color, dl::RESOURCE_STATE_TRANSITION_MODE_TRANSITION, m.copy_tex,
                              dl::RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
  context->CopyTexture(copy);
  dl::ITextureView* copy_srv = m.copy_tex->GetDefaultView(dl::TEXTURE_VIEW_SHADER_RESOURCE);

  m.edges.Set("g_Color", copy_srv);
  m.Run(context, m.edges, m.edges_tex, true);

  m.weights.Set("g_Edges", m.edges_tex->GetDefaultView(dl::TEXTURE_VIEW_SHADER_RESOURCE));
  m.Run(context, m.weights, m.weights_tex, true);

  Pass& blend = m.blend[m.format];
  blend.Set("g_Color", copy_srv);
  blend.Set("g_Weights", m.weights_tex->GetDefaultView(dl::TEXTURE_VIEW_SHADER_RESOURCE));
  m.Run(context, blend, color, false);

  context->SetRenderTargets(0, nullptr, nullptr, dl::RESOURCE_STATE_TRANSITION_MODE_NONE);
  return true;
}

}  // namespace native
