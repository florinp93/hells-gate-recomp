#include "dante_draws.h"

#include "dante_stats.h"
#include "dante_textures.h"

#include "Common/interface/RefCntAutoPtr.hpp"
#include "Graphics/GraphicsEngine/interface/Buffer.h"
#include "Graphics/GraphicsEngine/interface/BufferView.h"
#include "Graphics/GraphicsEngine/interface/DeviceContext.h"
#include "Graphics/GraphicsEngine/interface/PipelineState.h"
#include "Graphics/GraphicsEngine/interface/RenderDevice.h"
#include "Graphics/GraphicsEngine/interface/Sampler.h"
#include "Graphics/GraphicsEngine/interface/ShaderResourceBinding.h"
#include "Graphics/GraphicsEngine/interface/Texture.h"
#include "Graphics/GraphicsEngine/interface/TextureView.h"
#include "Graphics/GraphicsTools/interface/MapHelper.hpp"

#include <rex/graphics/pipeline/shader/spirv.h>
#include <rex/graphics/pipeline/shader/spirv_translator.h>
#include <rex/graphics/pipeline/texture/util.h>
#include <rex/graphics/register_file.h>
#include <rex/graphics/registers.h>
#include <rex/graphics/util/draw.h>
#include <rex/graphics/xenos.h>
#include <rex/logging/macros.h>

#include <xxhash.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <set>
#include <string>
#include <string_view>

#include <fmt/format.h>
#include <map>
#include <unordered_map>
#include <vector>

namespace native {
namespace {

namespace dl = Diligent;
namespace rg = rex::graphics;
namespace reg = rex::graphics::reg;
namespace xenos = rex::graphics::xenos;
using Translator = rg::SpirvShaderTranslator;

constexpr uint32_t kSharedMemorySize = 512u * 1024 * 1024;
constexpr uint32_t kIndexRingSize = 32u * 1024 * 1024;
// Guest physical memory is mirrored at 0xA0000000 in the virtual address space.
constexpr uint32_t kPhysicalMirror = 0xA0000000u;

const uint8_t* Physical(const uint8_t* base, uint32_t address) {
  return base + kPhysicalMirror + (address & 0x1FFFFFFFu);
}

struct PipelineKey {
  uint64_t vertex_key;
  uint64_t pixel_key;
  uint32_t topology;
  uint32_t color_format[4];
  uint32_t color_count;
  uint32_t depth_format;
  uint32_t color_mask;
  uint32_t depth_enable;
  uint32_t depth_write;
  uint32_t depth_func;
  uint32_t blend[4];          // RB_BLENDCONTROL0-3 & 0x1FFF1FFF, 0 = disabled
  uint32_t stencil_control;   // RB_DEPTHCONTROL stencil bits, 0 = disabled
  uint32_t stencil_masks;     // read mask | write mask << 8
  uint32_t cull;              // bit 0 cull front, bit 1 cull back, bit 2 front face clockwise
  int32_t depth_bias;         // constant depth bias (host units)
  uint32_t depth_bias_slope;  // slope-scaled depth bias (float bits)
  bool operator==(const PipelineKey&) const = default;
};

dl::BLEND_FACTOR BlendFactor(xenos::BlendFactor f, bool alpha) {
  switch (f) {
    case xenos::BlendFactor::kZero: return dl::BLEND_FACTOR_ZERO;
    case xenos::BlendFactor::kOne: return dl::BLEND_FACTOR_ONE;
    case xenos::BlendFactor::kSrcColor:
      return alpha ? dl::BLEND_FACTOR_SRC_ALPHA : dl::BLEND_FACTOR_SRC_COLOR;
    case xenos::BlendFactor::kOneMinusSrcColor:
      return alpha ? dl::BLEND_FACTOR_INV_SRC_ALPHA : dl::BLEND_FACTOR_INV_SRC_COLOR;
    case xenos::BlendFactor::kSrcAlpha: return dl::BLEND_FACTOR_SRC_ALPHA;
    case xenos::BlendFactor::kOneMinusSrcAlpha: return dl::BLEND_FACTOR_INV_SRC_ALPHA;
    case xenos::BlendFactor::kDstColor:
      return alpha ? dl::BLEND_FACTOR_DEST_ALPHA : dl::BLEND_FACTOR_DEST_COLOR;
    case xenos::BlendFactor::kOneMinusDstColor:
      return alpha ? dl::BLEND_FACTOR_INV_DEST_ALPHA : dl::BLEND_FACTOR_INV_DEST_COLOR;
    case xenos::BlendFactor::kDstAlpha: return dl::BLEND_FACTOR_DEST_ALPHA;
    case xenos::BlendFactor::kOneMinusDstAlpha: return dl::BLEND_FACTOR_INV_DEST_ALPHA;
    case xenos::BlendFactor::kConstantColor:
    case xenos::BlendFactor::kConstantAlpha: return dl::BLEND_FACTOR_BLEND_FACTOR;
    case xenos::BlendFactor::kOneMinusConstantColor:
    case xenos::BlendFactor::kOneMinusConstantAlpha: return dl::BLEND_FACTOR_INV_BLEND_FACTOR;
    case xenos::BlendFactor::kSrcAlphaSaturate:
      return alpha ? dl::BLEND_FACTOR_ONE : dl::BLEND_FACTOR_SRC_ALPHA_SAT;
    default: return dl::BLEND_FACTOR_ONE;
  }
}

dl::BLEND_OPERATION BlendOp(xenos::BlendOp op) {
  switch (op) {
    case xenos::BlendOp::kSubtract: return dl::BLEND_OPERATION_SUBTRACT;
    case xenos::BlendOp::kMin: return dl::BLEND_OPERATION_MIN;
    case xenos::BlendOp::kMax: return dl::BLEND_OPERATION_MAX;
    case xenos::BlendOp::kRevSubtract: return dl::BLEND_OPERATION_REV_SUBTRACT;
    default: return dl::BLEND_OPERATION_ADD;
  }
}

dl::STENCIL_OP StencilOp(xenos::StencilOp op) {
  switch (op) {
    case xenos::StencilOp::kZero: return dl::STENCIL_OP_ZERO;
    case xenos::StencilOp::kReplace: return dl::STENCIL_OP_REPLACE;
    case xenos::StencilOp::kIncrementClamp: return dl::STENCIL_OP_INCR_SAT;
    case xenos::StencilOp::kDecrementClamp: return dl::STENCIL_OP_DECR_SAT;
    case xenos::StencilOp::kInvert: return dl::STENCIL_OP_INVERT;
    case xenos::StencilOp::kIncrementWrap: return dl::STENCIL_OP_INCR_WRAP;
    case xenos::StencilOp::kDecrementWrap: return dl::STENCIL_OP_DECR_WRAP;
    default: return dl::STENCIL_OP_KEEP;
  }
}

// xenos::CompareFunction (never..always = 0..7) -> COMPARISON_FUNCTION (1..8).
dl::COMPARISON_FUNCTION Compare(xenos::CompareFunction f) {
  return dl::COMPARISON_FUNCTION(uint32_t(f) + 1);
}
struct PipelineKeyHash {
  size_t operator()(const PipelineKey& k) const { return XXH3_64bits(&k, sizeof(k)); }
};

// Texture / sampler variables of a pipeline, parsed from the translator's
// names: xe_texture{fc}_{2d|3d|cube}_{s|u}, xe_sampler{fc}_{mag}{min}{mip}[_a{N}].
struct TextureVariable {
  dl::IShaderResourceVariable* variable;
  uint32_t fetch_constant;
  char dimension;  // '2' (2D array), '3' (3D), 'c' (cube)
  bool is_signed;  // translator binding for signed components (_s)
};
struct SamplerVariable {
  dl::IShaderResourceVariable* variable;
  uint32_t fetch_constant;
  uint32_t mag, min, mip, aniso;
};

struct Pipeline {
  dl::RefCntAutoPtr<dl::IPipelineState> pso;
  dl::RefCntAutoPtr<dl::IShaderResourceBinding> srb;
  std::vector<TextureVariable> textures;
  std::vector<SamplerVariable> samplers;
  uint32_t used_fetch_mask = 0;
};

bool ParseSamplerName(std::string_view name, SamplerVariable& out) {
  size_t pos = name.find("xe_sampler");
  if (pos == std::string_view::npos) return false;
  pos += 10;
  uint32_t fc = 0;
  while (pos < name.size() && name[pos] >= '0' && name[pos] <= '9') fc = fc * 10 + (name[pos++] - '0');
  if (pos + 4 > name.size() || name[pos] != '_') return false;
  auto filter = [](char c) -> uint32_t { return c == 'p' ? 0 : c == 'l' ? 1 : c == 'b' ? 2 : 3; };
  out.fetch_constant = fc;
  out.mag = filter(name[pos + 1]);
  out.min = filter(name[pos + 2]);
  out.mip = filter(name[pos + 3]);
  out.aniso = 7;  // kUseFetchConst
  pos += 4;
  if (pos + 2 < name.size() + 1 && name.substr(pos, 2) == "_a") {
    uint32_t n = 0;
    for (pos += 2; pos < name.size() && name[pos] >= '0' && name[pos] <= '9'; ++pos) {
      n = n * 10 + (name[pos] - '0');
    }
    out.aniso = n ? uint32_t(__builtin_ctz(n)) + 1 : 0;
  }
  return true;
}

}  // namespace

struct DrawRenderer::Impl {
  dl::IRenderDevice* device;
  dl::IDeviceContext* context;
  TextureCache* textures;
  uint64_t frame = 0;

  dl::RefCntAutoPtr<dl::IBuffer> shared_memory;
  // Uploaded ranges (start -> end, content hash), non-overlapping: an upload
  // drops every range it overlaps, whose GPU copy it partly overwrote.
  struct UploadedRange {
    uint32_t end;
    uint64_t hash;
    uint64_t checked_frame;
    uint64_t changed_frame;  // last frame the contents differed
  };
  // Ranges unchanged for this many frames are static: hashed once per frame.
  // Others (dynamic buffers rewritten during the frame) are hashed per draw.
  static constexpr uint64_t kStaticFrames = 60;
  std::map<uint32_t, UploadedRange> uploaded;
  dl::RefCntAutoPtr<dl::IBuffer> index_ring;
  uint32_t index_ring_offset = 0;
  std::vector<uint8_t> index_scratch;

  dl::RefCntAutoPtr<dl::IBuffer> system_constants;
  dl::RefCntAutoPtr<dl::IBuffer> float_vertex;
  dl::RefCntAutoPtr<dl::IBuffer> float_pixel;
  dl::RefCntAutoPtr<dl::IBuffer> bool_loop;
  dl::RefCntAutoPtr<dl::IBuffer> fetch;

  dl::RefCntAutoPtr<dl::ITexture> dummy_2d, dummy_3d, dummy_cube;
  // Fetch constants (dwords 1-2) already reported as having no texture.
  std::set<uint64_t> missing_textures;
  dl::RefCntAutoPtr<dl::ISampler> dummy_sampler;

  std::unordered_map<PipelineKey, Pipeline, PipelineKeyHash> pipelines;

  uint32_t draws = 0, skipped_points = 0, skipped_other = 0, pipeline_failures = 0;
  uint32_t skipped_targets = 0, skipped_viewport = 0, skipped_scissor = 0, skipped_pipeline = 0;
  uint64_t uploaded_bytes = 0;

  bool Initialize();
  dl::IBuffer* CreateUniform(const char* name, uint32_t size);
  // always_check: data rewritten within a frame (inline vertices in the ring).
  void Upload(const uint8_t* base, uint32_t address, uint32_t size, bool always_check = false);
  Pipeline* GetPipeline(const PipelineKey& key, const DrawShaders& shaders);
  void BindStatic(Pipeline& pipeline, dl::SHADER_TYPE stage);
};

dl::IBuffer* DrawRenderer::Impl::CreateUniform(const char* name, uint32_t size) {
  dl::BufferDesc desc;
  desc.Name = name;
  desc.Size = size;
  desc.BindFlags = dl::BIND_UNIFORM_BUFFER;
  desc.Usage = dl::USAGE_DYNAMIC;
  desc.CPUAccessFlags = dl::CPU_ACCESS_WRITE;
  dl::IBuffer* buffer = nullptr;
  device->CreateBuffer(desc, nullptr, &buffer);
  return buffer;
}

bool DrawRenderer::Impl::Initialize() {
  dl::BufferDesc shared;
  shared.Name = "XenosSharedMemory";
  shared.Size = kSharedMemorySize;
  shared.BindFlags = dl::BIND_SHADER_RESOURCE | dl::BIND_UNORDERED_ACCESS;
  shared.Usage = dl::USAGE_DEFAULT;
  shared.Mode = dl::BUFFER_MODE_RAW;
  shared.ElementByteStride = 4;
  device->CreateBuffer(shared, nullptr, &shared_memory);

  dl::BufferDesc index;
  index.Name = "XenosIndexRing";
  index.Size = kIndexRingSize;
  index.BindFlags = dl::BIND_INDEX_BUFFER;
  index.Usage = dl::USAGE_DEFAULT;
  device->CreateBuffer(index, nullptr, &index_ring);

  system_constants.Attach(CreateUniform("XenosSystemConstants", sizeof(Translator::SystemConstants)));
  float_vertex.Attach(CreateUniform("XenosFloatVS", 256 * 16));
  float_pixel.Attach(CreateUniform("XenosFloatPS", 256 * 16));
  bool_loop.Attach(CreateUniform("XenosBoolLoop", (8 + 32) * 4));
  fetch.Attach(CreateUniform("XenosFetch", 6 * 32 * 4));

  // Missing / unsupported textures read zero, like the SDK's null images.
  const uint32_t white[6] = {};
  auto make_dummy = [&](dl::RESOURCE_DIMENSION dim, uint32_t layers,
                        dl::RefCntAutoPtr<dl::ITexture>& out) {
    dl::TextureDesc desc;
    desc.Name = "XenosDummyTexture";
    desc.Type = dim;
    desc.Width = 1;
    desc.Height = 1;
    if (dim == dl::RESOURCE_DIM_TEX_3D) {
      desc.Depth = 1;
    } else {
      desc.ArraySize = layers;
    }
    desc.Format = dl::TEX_FORMAT_RGBA8_UNORM;
    desc.BindFlags = dl::BIND_SHADER_RESOURCE;
    desc.Usage = dl::USAGE_IMMUTABLE;
    std::vector<dl::TextureSubResData> subresources(layers, dl::TextureSubResData{white, 4});
    dl::TextureData data{subresources.data(), layers};
    device->CreateTexture(desc, &data, &out);
  };
  make_dummy(dl::RESOURCE_DIM_TEX_2D_ARRAY, 1, dummy_2d);
  make_dummy(dl::RESOURCE_DIM_TEX_3D, 1, dummy_3d);
  make_dummy(dl::RESOURCE_DIM_TEX_CUBE, 6, dummy_cube);
  dl::SamplerDesc sampler;
  device->CreateSampler(sampler, &dummy_sampler);

  bool ok = shared_memory && index_ring && system_constants && float_vertex && float_pixel &&
            bool_loop && fetch && dummy_2d && dummy_3d && dummy_cube && dummy_sampler;
  if (!ok) REXLOG_ERROR("NATIVE-DRAW: resource creation failed");
  return ok;
}

void DrawRenderer::Impl::Upload(const uint8_t* base, uint32_t address, uint32_t size,
                                bool always_check) {
  if (!size || address >= kSharedMemorySize) return;
  size = std::min(size, kSharedMemorySize - address);
  uint32_t end = address + size;
  auto it = uploaded.find(address);
  if (!always_check && it != uploaded.end() && it->second.end == end &&
      it->second.checked_frame == frame && frame - it->second.changed_frame > kStaticFrames) {
    return;
  }
  const uint8_t* data = Physical(base, address);
  uint64_t hash = XXH3_64bits(data, size);
  if (it != uploaded.end() && it->second.end == end && it->second.hash == hash) {
    it->second.checked_frame = frame;
    return;
  }
  it = uploaded.upper_bound(address);
  if (it != uploaded.begin() && std::prev(it)->second.end > address) --it;
  while (it != uploaded.end() && it->first < end) it = uploaded.erase(it);
  uploaded.emplace(address, UploadedRange{end, hash, frame, frame});
  context->UpdateBuffer(shared_memory, address, size, data,
                        dl::RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
  uploaded_bytes += size;
}

// Binds everything that is the same object for every draw (textures start as dummies).
void DrawRenderer::Impl::BindStatic(Pipeline& pipeline, dl::SHADER_TYPE stage) {
  dl::IShaderResourceBinding* srb = pipeline.srb;
  for (uint32_t i = 0, n = srb->GetVariableCount(stage); i < n; ++i) {
    dl::IShaderResourceVariable* var = srb->GetVariableByIndex(stage, i);
    dl::ShaderResourceDesc desc;
    var->GetResourceDesc(desc);
    std::string_view name = desc.Name;
    // Block names (DiligentCore reflects uniform/storage blocks by type name).
    if (name == "XeSharedMemory" || name == "xe_shared_memory") {
      var->Set(shared_memory->GetDefaultView(desc.Type == dl::SHADER_RESOURCE_TYPE_BUFFER_UAV
                                                 ? dl::BUFFER_VIEW_UNORDERED_ACCESS
                                                 : dl::BUFFER_VIEW_SHADER_RESOURCE));
    } else if (name == "XeSystemConstants") {
      var->Set(system_constants);
    } else if (name == "XeFloatConstants") {
      var->Set(float_vertex);
    } else if (name == "XeFloatConstantsPS") {
      var->Set(float_pixel);
    } else if (name == "XeBoolLoopConstants") {
      var->Set(bool_loop);
    } else if (name == "XeFetchConstants") {
      var->Set(fetch);
    } else if (name.find("xe_texture") != std::string_view::npos) {
      dl::ITexture* dummy = name.find("_3d_") != std::string_view::npos     ? dummy_3d.RawPtr()
                            : name.find("_cube_") != std::string_view::npos ? dummy_cube.RawPtr()
                                                                            : dummy_2d.RawPtr();
      var->Set(dummy->GetDefaultView(dl::TEXTURE_VIEW_SHADER_RESOURCE));
      size_t digits = name.find("xe_texture") + 10;
      uint32_t fc = 0;
      while (digits < name.size() && name[digits] >= '0' && name[digits] <= '9') {
        fc = fc * 10 + (name[digits++] - '0');
      }
      char dimension = dummy == dummy_3d.RawPtr() ? '3' : dummy == dummy_cube.RawPtr() ? 'c' : '2';
      bool is_signed = name.size() >= 2 && name.substr(name.size() - 2) == "_s";
      pipeline.textures.push_back({var, fc, dimension, is_signed});
      pipeline.used_fetch_mask |= 1u << (fc & 31);
    } else if (name.find("xe_sampler") != std::string_view::npos) {
      var->Set(dummy_sampler);
      SamplerVariable sampler{var};
      if (ParseSamplerName(name, sampler)) pipeline.samplers.push_back(sampler);
    } else {
      REXLOG_WARN("NATIVE-DRAW: unknown shader resource '{}'", name);
    }
  }
}

Pipeline* DrawRenderer::Impl::GetPipeline(const PipelineKey& key, const DrawShaders& shaders) {
  auto it = pipelines.find(key);
  if (it != pipelines.end()) return it->second.pso ? &it->second : nullptr;
  Pipeline& p = pipelines[key];
  dl::GraphicsPipelineStateCreateInfo ci;
  ci.PSODesc.Name = "XenosDraw";
  ci.PSODesc.PipelineType = dl::PIPELINE_TYPE_GRAPHICS;
  // Dynamic: textures and samplers change per draw.
  ci.PSODesc.ResourceLayout.DefaultVariableType = dl::SHADER_RESOURCE_VARIABLE_TYPE_DYNAMIC;
  ci.pVS = shaders.vertex;
  ci.pPS = shaders.pixel;
  auto& gp = ci.GraphicsPipeline;
  gp.NumRenderTargets = uint8_t(key.color_count);
  gp.BlendDesc.IndependentBlendEnable = dl::True;
  for (uint32_t i = 0; i < key.color_count; ++i) {
    gp.RTVFormats[i] = dl::TEXTURE_FORMAT(key.color_format[i]);
    auto& rt = gp.BlendDesc.RenderTargets[i];
    rt.RenderTargetWriteMask = dl::COLOR_MASK((key.color_mask >> (i * 4)) & 0xF);
    if (key.blend[i]) {
      reg::RB_BLENDCONTROL b;
      b.value = key.blend[i];
      rt.BlendEnable = dl::True;
      rt.SrcBlend = BlendFactor(b.color_srcblend, false);
      rt.DestBlend = BlendFactor(b.color_destblend, false);
      rt.BlendOp = BlendOp(b.color_comb_fcn);
      rt.SrcBlendAlpha = BlendFactor(b.alpha_srcblend, true);
      rt.DestBlendAlpha = BlendFactor(b.alpha_destblend, true);
      rt.BlendOpAlpha = BlendOp(b.alpha_comb_fcn);
    }
  }
  gp.DSVFormat = dl::TEXTURE_FORMAT(key.depth_format);
  gp.PrimitiveTopology = dl::PRIMITIVE_TOPOLOGY(key.topology);
  // Final framebuffer coordinates match the SDK Vulkan backend (the Y flip
  // DiligentCore applies is undone in the NDC transform), so are its rules:
  // front face clockwise if PA_SU_SC_MODE_CNTL.face.
  gp.RasterizerDesc.CullMode = (key.cull & 1)   ? dl::CULL_MODE_FRONT
                               : (key.cull & 2) ? dl::CULL_MODE_BACK
                                                : dl::CULL_MODE_NONE;
  gp.RasterizerDesc.FrontCounterClockwise = (key.cull & 4) ? dl::False : dl::True;
  // Polygon offset (SDK Vulkan host render target path).
  gp.RasterizerDesc.DepthBias = key.depth_bias;
  std::memcpy(&gp.RasterizerDesc.SlopeScaledDepthBias, &key.depth_bias_slope, sizeof(float));
  gp.RasterizerDesc.ScissorEnable = dl::True;
  gp.DepthStencilDesc.DepthEnable = key.depth_enable ? dl::True : dl::False;
  gp.DepthStencilDesc.DepthWriteEnable = key.depth_write ? dl::True : dl::False;
  gp.DepthStencilDesc.DepthFunc = dl::COMPARISON_FUNCTION(key.depth_func);
  if (key.stencil_control) {
    reg::RB_DEPTHCONTROL dc;
    dc.value = key.stencil_control;
    auto& ds = gp.DepthStencilDesc;
    ds.StencilEnable = dl::True;
    ds.StencilReadMask = uint8_t(key.stencil_masks & 0xFF);
    ds.StencilWriteMask = uint8_t(key.stencil_masks >> 8);
    ds.FrontFace = {StencilOp(dc.stencilfail), StencilOp(dc.stencilzfail),
                    StencilOp(dc.stencilzpass), Compare(dc.stencilfunc)};
    ds.BackFace = dc.backface_enable
                      ? dl::StencilOpDesc{StencilOp(dc.stencilfail_bf), StencilOp(dc.stencilzfail_bf),
                                          StencilOp(dc.stencilzpass_bf), Compare(dc.stencilfunc_bf)}
                      : ds.FrontFace;
  }
  device->CreateGraphicsPipelineState(ci, &p.pso);
  ++g_dante_stats.pipelines_created;
  if (!p.pso) {
    if (++pipeline_failures <= 8) {
      REXLOG_WARN("NATIVE-DRAW: pipeline failed VS {:016X} PS {:016X} topology {}",
                  key.vertex_key, key.pixel_key, key.topology);
    }
    return nullptr;
  }
  p.pso->CreateShaderResourceBinding(&p.srb, true);
  BindStatic(p, dl::SHADER_TYPE_VERTEX);
  if (shaders.pixel) BindStatic(p, dl::SHADER_TYPE_PIXEL);
  return &p;
}

DrawRenderer::DrawRenderer(dl::IRenderDevice* device, dl::IDeviceContext* context,
                           TextureCache* textures)
    : impl_(std::make_unique<Impl>()) {
  impl_->device = device;
  impl_->context = context;
  impl_->textures = textures;
  impl_->Initialize();
}

DrawRenderer::~DrawRenderer() = default;

void DrawRenderer::SetFrame(uint64_t frame) { impl_->frame = frame; }

bool DrawRenderer::Draw(const rg::RegisterFile& regs, const DrawShaders& shaders,
                        const DrawTargets& targets, const uint8_t* base, bool log,
                        uint32_t volatile_fetch) {
  Impl& m = *impl_;
  if (!m.shared_memory || !shaders.vertex || !shaders.vertex_info) return false;
  if (!targets.width || !targets.height) {
    ++m.skipped_targets;
    return false;
  }

  auto initiator = regs.Get<reg::VGT_DRAW_INITIATOR>();
  uint32_t count = initiator.num_indices;
  bool indexed = initiator.source_select == xenos::SourceSelect::kDMA;
  if (!count) return false;

  // Topology; fans and quad lists become triangle lists via index rewriting.
  dl::PRIMITIVE_TOPOLOGY topology;
  bool fan = false, quads = false, points = false;
  switch (initiator.prim_type) {
    case xenos::PrimitiveType::kLineList: topology = dl::PRIMITIVE_TOPOLOGY_LINE_LIST; break;
    case xenos::PrimitiveType::kLineStrip: topology = dl::PRIMITIVE_TOPOLOGY_LINE_STRIP; break;
    case xenos::PrimitiveType::kTriangleList: topology = dl::PRIMITIVE_TOPOLOGY_TRIANGLE_LIST; break;
    case xenos::PrimitiveType::kTriangleStrip: topology = dl::PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP; break;
    case xenos::PrimitiveType::kTriangleFan:
      topology = dl::PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
      fan = true;
      break;
    case xenos::PrimitiveType::kQuadList:
      topology = dl::PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
      quads = true;
      break;
    case xenos::PrimitiveType::kPointList:
      // The translated VS (kPointListAsTriangleStrip) expands each point:
      // host index = point << 2 | corner, drawn as restarted 4-vertex strips.
      topology = dl::PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
      points = true;
      break;
    default:
      ++m.skipped_other;
      return false;
  }

  // Vertex data the shader fetches, uploaded raw (big-endian) to the shared memory mirror.
  std::string vertex_log;
  for (const auto& binding : shaders.vertex_info->vertex_bindings()) {
    uint32_t d0 = regs.values[rg::XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0 + binding.fetch_constant * 2];
    uint32_t d1 = regs.values[rg::XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0 + binding.fetch_constant * 2 + 1];
    m.Upload(base, d0 & 0xFFFFFFFCu, ((d1 >> 2) & 0xFFFFFF) * 4,
             binding.fetch_constant == volatile_fetch);
    if (log) {
      vertex_log += fmt::format(" vf{}={:08X}+{}/{}", binding.fetch_constant, d0 & 0xFFFFFFFCu,
                                ((d1 >> 2) & 0xFFFFFF) * 4, binding.stride_words * 4);
    }
  }

  // Indices: guest DMA indices (byte-swapped, reset index mapped to the host
  // restart value) or generated for fans / quad lists, written to the ring.
  bool index32 = indexed && initiator.index_size == xenos::IndexFormat::kInt32;
  std::vector<uint32_t> source;
  if (indexed) {
    uint32_t dma_base = regs.values[rg::XE_GPU_REG_VGT_DMA_BASE];
    auto dma_size = regs.Get<reg::VGT_DMA_SIZE>();
    const uint8_t* src = Physical(base, dma_base & ~(index32 ? 3u : 1u));
    // Primitive reset only applies to strips (as in the SDK primitive processor).
    bool reset_enabled = regs.Get<reg::PA_SU_SC_MODE_CNTL>().multi_prim_ib_ena &&
                         (topology == dl::PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP ||
                          topology == dl::PRIMITIVE_TOPOLOGY_LINE_STRIP) &&
                         !points;
    uint32_t reset = regs.Get<reg::VGT_MULTI_PRIM_IB_RESET_INDX>().reset_indx;
    source.reserve(count);
    for (uint32_t i = 0; i < count; ++i) {
      uint32_t v;
      if (index32) {
        uint32_t raw;
        std::memcpy(&raw, src + i * 4, 4);
        v = xenos::GpuSwap(raw, dma_size.swap_mode);
        if (reset_enabled && (v & 0xFFFFFF) == reset) v = 0xFFFFFFFFu;
      } else {
        uint16_t raw;
        std::memcpy(&raw, src + i * 2, 2);
        v = xenos::GpuSwap(raw, dma_size.swap_mode);
        if (reset_enabled && v == (reset & 0xFFFF)) v = 0xFFFF;
      }
      source.push_back(v);
    }
  }
  std::vector<uint32_t> indices;
  uint32_t point_index_load_address = 0;
  xenos::Endian point_index_endian = xenos::Endian::kNone;
  if (points) {
    indices.reserve(size_t(count) * 5);
    for (uint32_t i = 0; i < count; ++i) {
      if (i) indices.push_back(0xFFFFFFFFu);
      indices.insert(indices.end(), {i << 2, (i << 2) | 1, (i << 2) | 2, (i << 2) | 3});
    }
    if (indexed) {
      uint32_t dma_base = regs.values[rg::XE_GPU_REG_VGT_DMA_BASE];
      uint32_t index_bytes = index32 ? 4 : 2;
      point_index_load_address = dma_base & ~(index_bytes - 1);
      point_index_endian = regs.Get<reg::VGT_DMA_SIZE>().swap_mode;
      if (!index32 && point_index_endian != xenos::Endian::kNone &&
          point_index_endian != xenos::Endian::k8in16) {
        point_index_endian = point_index_endian == xenos::Endian::k8in32 ? xenos::Endian::k8in16
                                                                         : xenos::Endian::kNone;
      }
      m.Upload(base, point_index_load_address, count * index_bytes, true);
    }
    index32 = true;
  } else if (fan || quads) {
    auto vertex = [&](uint32_t i) { return indexed ? source[i] : i; };
    if (fan) {
      for (uint32_t i = 2; i < count; ++i) {
        indices.insert(indices.end(), {vertex(0), vertex(i - 1), vertex(i)});
      }
    } else {
      for (uint32_t q = 0; q + 3 < count; q += 4) {
        indices.insert(indices.end(), {vertex(q), vertex(q + 1), vertex(q + 2), vertex(q),
                                       vertex(q + 2), vertex(q + 3)});
      }
    }
    index32 = true;
  } else if (indexed) {
    indices = std::move(source);
  }
  uint32_t index_offset = 0;
  dl::VALUE_TYPE index_type = index32 ? dl::VT_UINT32 : dl::VT_UINT16;
  if (!indices.empty()) {
    uint32_t stride = index32 ? 4 : 2;
    uint32_t bytes = uint32_t(indices.size()) * stride;
    m.index_scratch.resize(bytes);
    for (size_t i = 0; i < indices.size(); ++i) {
      if (index32) {
        std::memcpy(&m.index_scratch[i * 4], &indices[i], 4);
      } else {
        uint16_t v = uint16_t(indices[i]);
        std::memcpy(&m.index_scratch[i * 2], &v, 2);
      }
    }
    if (bytes > kIndexRingSize) return false;
    if (m.index_ring_offset + bytes > kIndexRingSize) m.index_ring_offset = 0;
    index_offset = m.index_ring_offset;
    m.context->UpdateBuffer(m.index_ring, index_offset, bytes, m.index_scratch.data(),
                            dl::RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
    m.index_ring_offset = (index_offset + bytes + 3) & ~3u;
  }

  // Constants (SDK Vulkan backend layout).
  {
    dl::MapHelper<uint8_t> map(m.context, m.float_vertex, dl::MAP_WRITE, dl::MAP_FLAG_DISCARD);
    uint8_t* out = map;
    const auto& bitmap = shaders.vertex_info->constant_register_map().float_bitmap;
    for (uint32_t i = 0; i < 4; ++i) {
      uint64_t bits = bitmap[i];
      while (bits) {
        uint32_t c = uint32_t(__builtin_ctzll(bits));
        bits &= bits - 1;
        std::memcpy(out, &regs.values[rg::XE_GPU_REG_SHADER_CONSTANT_000_X + (i << 8) + (c << 2)], 16);
        out += 16;
      }
    }
  }
  if (shaders.pixel_info) {
    dl::MapHelper<uint8_t> map(m.context, m.float_pixel, dl::MAP_WRITE, dl::MAP_FLAG_DISCARD);
    uint8_t* out = map;
    const auto& bitmap = shaders.pixel_info->constant_register_map().float_bitmap;
    for (uint32_t i = 0; i < 4; ++i) {
      uint64_t bits = bitmap[i];
      while (bits) {
        uint32_t c = uint32_t(__builtin_ctzll(bits));
        bits &= bits - 1;
        std::memcpy(out, &regs.values[rg::XE_GPU_REG_SHADER_CONSTANT_256_X + (i << 8) + (c << 2)], 16);
        out += 16;
      }
    }
  }
  {
    dl::MapHelper<uint8_t> map(m.context, m.bool_loop, dl::MAP_WRITE, dl::MAP_FLAG_DISCARD);
    std::memcpy(map, &regs.values[rg::XE_GPU_REG_SHADER_CONSTANT_BOOL_000_031], (8 + 32) * 4);
  }
  {
    dl::MapHelper<uint8_t> map(m.context, m.fetch, dl::MAP_WRITE, dl::MAP_FLAG_DISCARD);
    std::memcpy(map, &regs.values[rg::XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0], 6 * 32 * 4);
  }

  // Viewport and system constants (host render target subset of
  // VulkanCommandProcessor::UpdateSystemConstantValues).
  auto depth_control = rg::draw_util::GetNormalizedDepthControl(regs);
  rg::draw_util::ViewportInfo viewport;
  const float scale_x = targets.scale_x, scale_y = targets.scale_y;
  auto host_x = [&](uint32_t v) { return uint32_t(std::lround(double(v) * scale_x)); };
  auto host_y = [&](uint32_t v) { return uint32_t(std::lround(double(v) * scale_y)); };
  // Guest pixels, scaled below (the NDC transform does not depend on the scale).
  rg::draw_util::GetHostViewportInfo(regs, 1, 1, false, 16384, 16384, true, depth_control, false,
                                     true, shaders.pixel_info && shaders.pixel_info->writes_depth(),
                                     viewport);
  for (uint32_t i = 0; i < 2; ++i) {
    auto scale_axis = [&](uint32_t v) { return i ? host_y(v) : host_x(v); };
    uint32_t end = scale_axis(viewport.xy_offset[i] + viewport.xy_extent[i]);
    viewport.xy_offset[i] = scale_axis(viewport.xy_offset[i]);
    viewport.xy_extent[i] = end - viewport.xy_offset[i];
  }
  if (!viewport.xy_extent[0] || !viewport.xy_extent[1]) {
    ++m.skipped_viewport;
    return false;
  }
  // Uploaded after the textures are bound (textures_resolution_scaled).
  Translator::SystemConstants sc;
  std::memset(&sc, 0, sizeof(sc));
  {
    auto vte = regs.Get<reg::PA_CL_VTE_CNTL>();
    auto colorcontrol = regs.Get<reg::RB_COLORCONTROL>();
    uint32_t flags = 0;
    if (vte.vtx_xy_fmt) flags |= Translator::kSysFlag_XYDividedByW;
    if (vte.vtx_z_fmt) flags |= Translator::kSysFlag_ZDividedByW;
    if (vte.vtx_w0_fmt) flags |= Translator::kSysFlag_WNotReciprocal;
    if (!points && (topology == dl::PRIMITIVE_TOPOLOGY_TRIANGLE_LIST ||
                    topology == dl::PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP)) {
      flags |= Translator::kSysFlag_PrimitivePolygonal;
    }
    if (points && indexed) {
      flags |= Translator::kSysFlag_ComputeOrPrimitiveVertexIndexLoad;
      if (initiator.index_size == xenos::IndexFormat::kInt32) {
        flags |= Translator::kSysFlag_ComputeOrPrimitiveVertexIndexLoad32Bit;
      }
    }
    if (rg::draw_util::IsPrimitiveLine(regs)) flags |= Translator::kSysFlag_PrimitiveLine;
    if (regs.Get<reg::RB_DEPTH_INFO>().depth_format == xenos::DepthRenderTargetFormat::kD24FS8) {
      flags |= Translator::kSysFlag_DepthFloat24;
    }
    auto alpha_func = colorcontrol.alpha_test_enable ? colorcontrol.alpha_func
                                                     : xenos::CompareFunction::kAlways;
    flags |= uint32_t(alpha_func) << Translator::kSysFlag_AlphaPassIfLess_Shift;
    for (uint32_t i = 0; i < 4; ++i) {
      auto info = regs.Get<reg::RB_COLOR_INFO>(reg::RB_COLOR_INFO::rt_register_indices[i]);
      if (info.color_format == xenos::ColorRenderTargetFormat::k_8_8_8_8_GAMMA) {
        flags |= Translator::kSysFlag_ConvertColor0ToGamma << i;
      }
      float scale;
      int32_t bias = info.color_exp_bias;
      uint32_t bits = 0x3F800000u + uint32_t(bias << 23);
      std::memcpy(&scale, &bits, 4);
      sc.color_exp_bias[i] = scale;
    }
    sc.flags = flags;
    // Indices are swapped on upload, except guest indices points load in the VS.
    sc.vertex_index_endian = point_index_endian;
    sc.vertex_index_load_address = point_index_load_address;
    if (points) {
      auto minmax = regs.Get<reg::PA_SU_POINT_MINMAX>();
      auto size = regs.Get<reg::PA_SU_POINT_SIZE>();
      sc.point_vertex_diameter_min = float(minmax.min_size) * (2.0f / 16.0f);
      sc.point_vertex_diameter_max = float(minmax.max_size) * (2.0f / 16.0f);
      sc.point_constant_diameter[0] = float(size.width) * (2.0f / 16.0f);
      sc.point_constant_diameter[1] = float(size.height) * (2.0f / 16.0f);
      sc.point_screen_diameter_to_ndc_radius[0] = scale_x / std::max(viewport.xy_extent[0], 1u);
      sc.point_screen_diameter_to_ndc_radius[1] = scale_y / std::max(viewport.xy_extent[1], 1u);
    }
    sc.vertex_base_index = int32_t(regs.values[rg::XE_GPU_REG_VGT_INDX_OFFSET]);
    sc.vertex_index_min = regs.values[rg::XE_GPU_REG_VGT_MIN_VTX_INDX];
    sc.vertex_index_max = regs.values[rg::XE_GPU_REG_VGT_MAX_VTX_INDX];
    for (uint32_t i = 0; i < 3; ++i) {
      sc.ndc_scale[i] = viewport.ndc_scale[i];
      sc.ndc_offset[i] = viewport.ndc_offset[i];
    }
    // DiligentCore flips Vulkan Y with a negative viewport height; undo it.
    sc.ndc_scale[1] = -sc.ndc_scale[1];
    sc.ndc_offset[1] = -sc.ndc_offset[1];
    std::memcpy(&sc.alpha_test_reference, &regs.values[rg::XE_GPU_REG_RB_ALPHA_REF], 4);
    sc.alpha_to_mask =
        colorcontrol.alpha_to_mask_enable ? (colorcontrol.value >> 24) | (1u << 8) : 0;
    // Post-swizzle signs (incl. gamma) of every fetch constant a shader samples.
    uint32_t used = 0;
    for (const auto* info : {shaders.vertex_info, shaders.pixel_info}) {
      if (!info) continue;
      for (const auto& binding : info->GetTextureBindingsAfterTranslation()) {
        used |= 1u << binding.fetch_constant;
      }
    }
    while (used) {
      uint32_t fc = uint32_t(__builtin_ctz(used));
      used &= used - 1;
      rex::graphics::xenos::xe_gpu_texture_fetch_t fetch;
      std::memcpy(&fetch, &regs.values[rg::XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0 + fc * 6],
                  sizeof(fetch));
      sc.texture_swizzled_signs[fc >> 2] |= uint32_t(rg::texture_util::SwizzleSigns(fetch))
                                            << (8 * (fc & 3));
    }
  }

  // Pipeline.
  PipelineKey key{};
  key.vertex_key = shaders.vertex_key;
  key.pixel_key = shaders.pixel_key;
  key.topology = topology;
  key.color_count = targets.color_count;
  for (uint32_t i = 0; i < targets.color_count; ++i) key.color_format[i] = targets.color_format[i];
  key.depth_format = targets.depth ? targets.depth_format : 0;
  // Only the render targets the pixel shader writes (none without one), as the SDK.
  key.color_mask = shaders.pixel_info ? rg::draw_util::GetNormalizedColorMask(
                                            regs, shaders.pixel_info->writes_color_targets())
                                      : 0;
  key.depth_enable = targets.depth && depth_control.z_enable;
  key.depth_write = targets.depth && depth_control.z_write_enable;
  key.depth_func = uint32_t(depth_control.zfunc) + 1;  // xenos -> COMPARISON_FUNCTION
  for (uint32_t i = 0; i < key.color_count; ++i) {
    static constexpr uint32_t kBlendRegs[4] = {rg::XE_GPU_REG_RB_BLENDCONTROL0,
                                               rg::XE_GPU_REG_RB_BLENDCONTROL1,
                                               rg::XE_GPU_REG_RB_BLENDCONTROL2,
                                               rg::XE_GPU_REG_RB_BLENDCONTROL3};
    uint32_t blend = regs.values[kBlendRegs[i]] & 0x1FFF1FFF;
    // 1 * src + 0 * dst for color and alpha = blending off.
    key.blend[i] = blend == 0x00010001 ? 0 : blend;
  }
  auto stencil_refmask = regs.Get<reg::RB_STENCILREFMASK>();
  if (targets.depth && depth_control.stencil_enable) {
    key.stencil_control = depth_control.value & 0xFFFFFF81u;  // stencil enable + funcs/ops
    key.stencil_masks = stencil_refmask.stencilmask | (stencil_refmask.stencilwritemask << 8);
  }
  if (rg::draw_util::IsPrimitivePolygonal(regs)) {
    auto mode_cntl = regs.Get<reg::PA_SU_SC_MODE_CNTL>();
    if (mode_cntl.cull_front && mode_cntl.cull_back) {
      ++m.skipped_other;
      return false;  // both faces culled: nothing is drawn
    }
    key.cull = (mode_cntl.cull_front ? 1u : 0u) | (mode_cntl.cull_back ? 2u : 0u) |
               (mode_cntl.face ? 4u : 0u);
  }
  if (targets.depth) {
    float slope = 0.0f, constant = 0.0f;
    rg::draw_util::GetPreferredFacePolygonOffset(regs, rg::draw_util::IsPrimitivePolygonal(regs),
                                                 slope, constant);
    constant *= regs.Get<reg::RB_DEPTH_INFO>().depth_format ==
                        xenos::DepthRenderTargetFormat::kD24S8
                    ? rg::draw_util::kD3D10PolygonOffsetFactorUnorm24
                    : rg::draw_util::kD3D10PolygonOffsetFactorFloat24;
    slope *= xenos::kPolygonOffsetScaleSubpixelUnit * std::max(scale_x, scale_y);
    key.depth_bias = int32_t(std::lround(constant));
    std::memcpy(&key.depth_bias_slope, &slope, sizeof(float));
  }
  Pipeline* pipeline = m.GetPipeline(key, shaders);
  if (!pipeline) {
    ++m.skipped_pipeline;
    return false;
  }

  // Textures and samplers of this draw.
  const uint32_t* fetch_base = &regs.values[rg::XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0];
  std::string texture_log;
  // Video frames (VP6 decoded to Y/U/V k_8 planes, drawn full screen): 16:9
  // content that must not be widened by an anamorphic (ultrawide) scale.
  uint32_t video_planes = 0;
  bool video_luma = false;
  for (const TextureVariable& t : pipeline->textures) {
    const uint32_t* fetch = fetch_base + (t.fetch_constant & 31) * 6;
    char source = '-';
    // The signed binding is only sampled for signed components.
    xenos::xe_gpu_texture_fetch_t fetch_constant;
    std::memcpy(&fetch_constant, fetch, sizeof(fetch_constant));
    bool want_signed =
        t.is_signed &&
        rg::texture_util::IsAnySignSigned(rg::texture_util::SwizzleSigns(fetch_constant));
    dl::ITextureView* view =
        t.dimension == '3'   ? m.textures->GetView3D(fetch, base, m.frame, &source, want_signed)
        : t.dimension == 'c' ? m.textures->GetViewCube(fetch, base, m.frame, &source, want_signed)
                             : m.textures->GetView2D(fetch, base, m.frame, &source, want_signed);
    if (log) {
      texture_log += fmt::format(" t{}{}={:08X}:{}x{}:f{}{}", t.fetch_constant,
                                 t.is_signed ? (want_signed ? "s" : "s~") : "",
                                 fetch[1] & 0xFFFFF000u, (fetch[2] & 0x1FFF) + 1,
                                 ((fetch[2] >> 13) & 0x1FFF) + 1, fetch[1] & 0x3F, source);
    }
    dl::ITexture* dummy = t.dimension == '3'   ? m.dummy_3d.RawPtr()
                          : t.dimension == 'c' ? m.dummy_cube.RawPtr()
                                               : m.dummy_2d.RawPtr();
    // 3D fetches also get a 2D (stacked) binding; the shader picks one by the
    // fetch constant's dimension, so the other one is unused.
    bool unused_binding =
        (t.dimension == '3') != (fetch_constant.dimension == xenos::DataDimension::k3D);
    if (!view && !unused_binding &&
        m.missing_textures.insert((uint64_t(fetch[1]) << 32) | fetch[2]).second &&
        m.missing_textures.size() <= 64) {
      REXLOG_INFO("NATIVE-TEX no texture for fetch {} ({}): {:08X} {:08X} {:08X} {:08X} {:08X} {:08X}",
                  t.fetch_constant, t.dimension, fetch[0], fetch[1], fetch[2], fetch[3], fetch[4],
                  fetch[5]);
    }
    if (fetch_constant.format == xenos::TextureFormat::k_8 && !t.is_signed) {
      ++video_planes;
      video_luma |= fetch_constant.size_2d.width + 1 >= 640;
    }
    if (view && source == 'R' && (scale_x != 1.0f || scale_y != 1.0f)) {
      sc.textures_resolution_scaled |= 1u << (t.fetch_constant & 31);
    }
    t.variable->Set(view ? view : dummy->GetDefaultView(dl::TEXTURE_VIEW_SHADER_RESOURCE));
  }
  for (const SamplerVariable& s : pipeline->samplers) {
    const uint32_t* fetch = fetch_base + (s.fetch_constant & 31) * 6;
    s.variable->Set(m.textures->GetSampler(fetch, s.mag, s.min, s.mip, s.aniso));
  }
  {
    dl::MapHelper<Translator::SystemConstants> map(m.context, m.system_constants, dl::MAP_WRITE,
                                                   dl::MAP_FLAG_DISCARD);
    *map = sc;
  }

  dl::ITextureView* rtvs[4] = {};
  for (uint32_t i = 0; i < targets.color_count; ++i) rtvs[i] = targets.color[i];
  m.context->SetRenderTargets(targets.color_count, rtvs, targets.depth,
                              dl::RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
  dl::Viewport vp;
  vp.TopLeftX = float(viewport.xy_offset[0]);
  vp.TopLeftY = float(viewport.xy_offset[1]);
  vp.Width = float(viewport.xy_extent[0]);
  vp.Height = float(viewport.xy_extent[1]);
  vp.MinDepth = viewport.z_min;
  vp.MaxDepth = viewport.z_max;
  if (video_planes >= 3 && video_luma && scale_x != scale_y) {
    // Squeeze around the render target's centre back to the 16:9 shape:
    // pillarbox on wider aspects, letterbox on narrower ones.
    if (scale_x > scale_y) {
      float squeeze = scale_y / scale_x;
      float center = 0.5f * float(host_x(targets.width));
      vp.TopLeftX = center + (vp.TopLeftX - center) * squeeze;
      vp.Width *= squeeze;
    } else {
      float squeeze = scale_x / scale_y;
      float center = 0.5f * float(host_y(targets.height));
      vp.TopLeftY = center + (vp.TopLeftY - center) * squeeze;
      vp.Height *= squeeze;
    }
  }
  const uint32_t host_width = host_x(targets.width), host_height = host_y(targets.height);
  m.context->SetViewports(1, &vp, host_width, host_height);
  rg::draw_util::Scissor scissor;
  rg::draw_util::GetScissor(regs, scissor);
  dl::Rect rect{int32_t(host_x(scissor.offset[0])), int32_t(host_y(scissor.offset[1])),
                int32_t(std::min(host_x(scissor.offset[0] + scissor.extent[0]), host_width)),
                int32_t(std::min(host_y(scissor.offset[1] + scissor.extent[1]), host_height))};
  if (rect.right <= rect.left || rect.bottom <= rect.top) {
    ++m.skipped_scissor;
    return false;
  }
  m.context->SetScissorRects(1, &rect, host_width, host_height);
  m.context->SetPipelineState(pipeline->pso);
  if (key.stencil_control) m.context->SetStencilRef(stencil_refmask.stencilref);
  float blend_constant[4];
  std::memcpy(blend_constant, &regs.values[rg::XE_GPU_REG_RB_BLEND_RED], sizeof(blend_constant));
  m.context->SetBlendFactors(blend_constant);
  m.context->CommitShaderResources(pipeline->srb, dl::RESOURCE_STATE_TRANSITION_MODE_TRANSITION);

  if (!indices.empty()) {
    m.context->SetIndexBuffer(m.index_ring, index_offset,
                              dl::RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
    dl::DrawIndexedAttribs attribs;
    attribs.NumIndices = uint32_t(indices.size());
    attribs.IndexType = index_type;
    m.context->DrawIndexed(attribs);
  } else {
    dl::DrawAttribs attribs;
    attribs.NumVertices = count;
    m.context->Draw(attribs);
  }
  ++m.draws;
  if (log) {
    REXLOG_INFO(
        "NATIVE-DRAW prim={} count={} {} idx={} vp=({},{} {}x{} z {:.3f}..{:.3f}) "
        "ndc=({:.4f},{:.4f},{:.4f})+({:.4f},{:.4f},{:.4f}) scissor=({},{})-({},{}) rt={}x{} "
        "colors={} depth={} zfunc={} ztest={} mask={:04X} blend={:08X} vs={:016X} ps={:016X}{}{}",
        uint32_t(initiator.prim_type), count, indexed ? "dma" : "auto", indices.size(),
        viewport.xy_offset[0], viewport.xy_offset[1], viewport.xy_extent[0],
        viewport.xy_extent[1], viewport.z_min, viewport.z_max, viewport.ndc_scale[0],
        viewport.ndc_scale[1], viewport.ndc_scale[2], viewport.ndc_offset[0],
        viewport.ndc_offset[1], viewport.ndc_offset[2], rect.left, rect.top, rect.right,
        rect.bottom, targets.width, targets.height, targets.color_count,
        targets.depth != nullptr, key.depth_func, key.depth_enable, key.color_mask, key.blend[0],
        shaders.vertex_info->ucode_data_hash(),
        shaders.pixel_info ? shaders.pixel_info->ucode_data_hash() : 0, vertex_log, texture_log);
  }
  return true;
}

void DrawRenderer::LogStats() {
  REXLOG_INFO(
      "NATIVE-DRAW draws={} pipelines={} pipeline_failures={} skipped: points={} other={} "
      "targets={} viewport={} scissor={} pipeline={} uploaded={} KB",
      impl_->draws, impl_->pipelines.size(), impl_->pipeline_failures, impl_->skipped_points,
      impl_->skipped_other, impl_->skipped_targets, impl_->skipped_viewport,
      impl_->skipped_scissor, impl_->skipped_pipeline, impl_->uploaded_bytes / 1024);
}

}  // namespace native
