#include "dante_shaders.h"

#include "Common/interface/RefCntAutoPtr.hpp"
#include "Graphics/GraphicsEngine/interface/RenderDevice.h"
#include "Graphics/GraphicsEngine/interface/Shader.h"

#include <rex/graphics/pipeline/shader/spirv.h>
#include <rex/graphics/pipeline/shader/spirv_translator.h>
#include <rex/graphics/register_file.h>
#include <rex/graphics/registers.h>
#include <rex/graphics/util/draw.h>
#include <rex/logging/macros.h>
#include <rex/string/buffer.h>

#include <xxhash.h>

#include <algorithm>
#include <cstring>

#include <string>
#include <unordered_map>
#include <vector>

namespace native {
namespace {

namespace dl = Diligent;
namespace rg = rex::graphics;
using rg::reg::PA_CL_CLIP_CNTL;
using rg::reg::RB_COLORCONTROL;
using rg::reg::RB_SURFACE_INFO;
using rg::reg::SQ_CONTEXT_MISC;
using rg::reg::SQ_INTERPOLATOR_CNTL;
using rg::reg::SQ_PROGRAM_CNTL;
using rg::reg::VGT_DRAW_INITIATOR;
using Modification = rg::SpirvShaderTranslator::Modification;

// Features the DiligentCore Vulkan device is guaranteed to provide.
rg::SpirvShaderTranslator::Features TranslatorFeatures() {
  rg::SpirvShaderTranslator::Features features(false);
  features.spirv_version = spv::Spv_1_3;
  features.max_storage_buffer_range = UINT32_MAX;  // shared memory as one binding
  features.full_draw_index_uint32 = true;
  features.image_view_format_swizzle = true;
  return features;
}

// Resource names DiligentCore binds by: the VS and PS both name their float
// constants "xe_uniform_float_constants", and both stages may sample the same
// fetch constant, so the PS float buffer and the VS textures/samplers get
// distinct names.
std::string StageResourceName(const std::string& name, bool is_vertex) {
  // DiligentCore names uniform/storage blocks by their block type name.
  if (!is_vertex && (name == "xe_uniform_float_constants" || name == "XeFloatConstants")) {
    return name + "PS";
  }
  if (is_vertex && (name.rfind("xe_texture", 0) == 0 || name.rfind("xe_sampler", 0) == 0)) {
    return "vs_" + name;
  }
  return name;
}

// Prepares translator SPIR-V for DiligentCore: inserts "OpSource GLSL 450"
// after the module preamble (its reflection requires one) and applies
// StageResourceName to OpName strings.
std::vector<uint32_t> ForDiligent(const std::vector<uint8_t>& binary, bool is_vertex) {
  std::vector<uint32_t> in(binary.size() / 4);
  std::memcpy(in.data(), binary.data(), in.size() * 4);
  constexpr uint32_t kOpSource = 3, kOpName = 5;
  std::vector<uint32_t> out(in.begin(), in.begin() + std::min<size_t>(5, in.size()));
  bool source_done = false;
  size_t pos = 5;
  while (pos < in.size()) {
    uint32_t opcode = in[pos] & 0xFFFF;
    uint32_t count = in[pos] >> 16;
    if (!count || pos + count > in.size()) {
      out.insert(out.end(), in.begin() + pos, in.end());
      break;
    }
    bool preamble = opcode == 17 || opcode == 10 || opcode == 11 || opcode == 14 ||
                    opcode == 15 || opcode == 16 || opcode == 331;
    if (opcode == kOpSource) source_done = true;
    if (!preamble && !source_done) {
      out.insert(out.end(), {(3u << 16) | kOpSource, 2 /* GLSL */, 450});
      source_done = true;
    }
    if (opcode == kOpName && count >= 3) {
      const char* chars = reinterpret_cast<const char*>(&in[pos + 2]);
      std::string name(chars, strnlen(chars, (count - 2) * 4));
      std::string renamed = StageResourceName(name, is_vertex);
      if (renamed != name) {
        uint32_t string_words = uint32_t(renamed.size() / 4 + 1);
        std::vector<uint32_t> instr(2 + string_words, 0);
        instr[0] = ((2 + string_words) << 16) | kOpName;
        instr[1] = in[pos + 1];
        std::memcpy(&instr[2], renamed.data(), renamed.size());
        out.insert(out.end(), instr.begin(), instr.end());
        pos += count;
        continue;
      }
    }
    out.insert(out.end(), in.begin() + pos, in.begin() + pos + count);
    pos += count;
  }
  return out;
}

struct GuestShader {
  std::unique_ptr<rg::SpirvShader> shader;
  // Per modification: compiled shader, or null after a failure.
  std::unordered_map<uint64_t, dl::RefCntAutoPtr<dl::IShader>> compiled;
};

}  // namespace

struct ShaderCache::Impl {
  dl::IRenderDevice* device;
  rg::SpirvShaderTranslator translator{TranslatorFeatures(), false, false, false};
  rex::string::StringBuffer disasm_buffer;
  std::unordered_map<uint64_t, GuestShader> shaders;  // by ucode hash
  uint32_t translated = 0;
  uint32_t failed = 0;

  GuestShader& Load(rg::xenos::ShaderType type, const GuestShaderCode& code, uint64_t& hash) {
    hash = XXH3_64bits(code.ucode, code.dwords * sizeof(uint32_t));
    GuestShader& entry = shaders[hash];
    if (!entry.shader) {
      entry.shader = std::make_unique<rg::SpirvShader>(type, hash, code.ucode, code.dwords);
      entry.shader->AnalyzeUcode(disasm_buffer);
    }
    return entry;
  }

  dl::IShader* Compile(GuestShader& entry, uint64_t modification, uint64_t hash) {
    auto it = entry.compiled.find(modification);
    if (it != entry.compiled.end()) return it->second;
    dl::RefCntAutoPtr<dl::IShader> result;
    auto* translation = entry.shader->GetOrCreateTranslation(modification);
    if (!translation->is_translated()) translator.TranslateAnalyzedShader(*translation);
    const bool is_vertex = entry.shader->type() == rg::xenos::ShaderType::kVertex;
    if (translation->is_valid() && !translation->translated_binary().empty()) {
      dl::ShaderCreateInfo ci;
      ci.Desc.ShaderType = is_vertex ? dl::SHADER_TYPE_VERTEX : dl::SHADER_TYPE_PIXEL;
      ci.Desc.Name = is_vertex ? "XenosVS" : "XenosPS";
      ci.EntryPoint = "main";
      std::vector<uint32_t> spirv = ForDiligent(translation->translated_binary(), is_vertex);
      ci.ByteCode = spirv.data();
      ci.ByteCodeSize = spirv.size() * sizeof(uint32_t);
      device->CreateShader(ci, &result);
    }
    if (result) {
      ++translated;
    } else {
      ++failed;
      REXLOG_WARN("NATIVE-SH {} {:016X} mod {:016X} failed ({})", is_vertex ? "VS" : "PS", hash,
                  modification,
                  translation->is_valid() ? "Diligent shader creation" : "translation");
    }
    entry.compiled.emplace(modification, result);
    return result;
  }
};

ShaderCache::ShaderCache(dl::IRenderDevice* device) : impl_(std::make_unique<Impl>()) {
  impl_->device = device;
}

ShaderCache::~ShaderCache() = default;

// Modification selection follows the SDK Vulkan backend
// (VulkanPipelineCache::GetCurrent{Vertex,Pixel}ShaderModification) for the
// host render target path, without clip/cull distances.
bool ShaderCache::Prepare(const rg::RegisterFile& regs, const GuestShaderCode& vs_code,
                          const GuestShaderCode& ps_code, TranslatedShaders& out) {
  out = {};
  if (!vs_code.ucode || !vs_code.dwords) return false;
  uint64_t vs_hash = 0, ps_hash = 0;
  GuestShader& vs = impl_->Load(rg::xenos::ShaderType::kVertex, vs_code, vs_hash);
  GuestShader* ps = nullptr;
  if (ps_code.ucode && ps_code.dwords) {
    ps = &impl_->Load(rg::xenos::ShaderType::kPixel, ps_code, ps_hash);
  }

  auto program_cntl = regs.Get<SQ_PROGRAM_CNTL>();
  auto prim_type = regs.Get<VGT_DRAW_INITIATOR>().prim_type;
  uint32_t ps_param_gen_pos = UINT32_MAX;
  uint32_t interpolator_mask =
      ps ? (vs.shader->writes_interpolators() &
            ps->shader->GetInterpolatorInputMask(program_cntl, regs.Get<SQ_CONTEXT_MISC>(),
                                                ps_param_gen_pos))
         : 0;

  auto host_vs_type = prim_type == rg::xenos::PrimitiveType::kPointList
                          ? rg::Shader::HostVertexShaderType::kPointListAsTriangleStrip
                          : rg::Shader::HostVertexShaderType::kVertex;
  Modification vs_mod(impl_->translator.GetDefaultVertexShaderModification(
      vs.shader->GetDynamicAddressableRegisterCount(program_cntl.vs_num_reg), host_vs_type));
  vs_mod.vertex.interpolator_mask = interpolator_mask;
  vs_mod.vertex.tessellation_mode = 0;
  vs_mod.vertex.user_clip_plane_count = 0;
  vs_mod.vertex.user_clip_plane_cull = 0;
  vs_mod.vertex.point_ps_ucp_mode = regs.Get<PA_CL_CLIP_CNTL>().ps_ucp_mode;
  if (host_vs_type == rg::Shader::HostVertexShaderType::kPointListAsTriangleStrip) {
    vs_mod.vertex.output_point_parameters = uint32_t(ps_param_gen_pos != UINT32_MAX);
  } else {
    vs_mod.vertex.output_point_parameters =
        uint32_t((vs.shader->writes_point_size_edge_flag_kill_vertex() & 0b001) &&
                 prim_type == rg::xenos::PrimitiveType::kPointList);
  }
  vs_mod.vertex.vertex_kill_and = 0;

  out.vertex = impl_->Compile(vs, vs_mod.value, vs_hash);
  out.vertex_info = vs.shader.get();
  out.vertex_key = vs_hash ^ (vs_mod.value * 0x9E3779B97F4A7C15ull);

  if (ps) {
    Modification ps_mod(impl_->translator.GetDefaultPixelShaderModification(
        ps->shader->GetDynamicAddressableRegisterCount(program_cntl.ps_num_reg)));
    ps_mod.pixel.interpolator_mask = interpolator_mask;
    ps_mod.pixel.interpolators_centroid =
        interpolator_mask &
        ~rg::xenos::GetInterpolatorSamplingPattern(regs.Get<RB_SURFACE_INFO>().msaa_samples,
                                                   regs.Get<SQ_CONTEXT_MISC>().sc_sample_cntl,
                                                   regs.Get<SQ_INTERPOLATOR_CNTL>().sampling_pattern);
    if (ps_param_gen_pos < rg::xenos::kMaxInterpolators) {
      ps_mod.pixel.param_gen_enable = 1;
      ps_mod.pixel.param_gen_interpolator = ps_param_gen_pos;
      ps_mod.pixel.param_gen_point = uint32_t(prim_type == rg::xenos::PrimitiveType::kPointList);
    } else {
      ps_mod.pixel.param_gen_enable = 0;
      ps_mod.pixel.param_gen_interpolator = 0;
      ps_mod.pixel.param_gen_point = 0;
    }
    using DepthStencilMode = Modification::DepthStencilMode;
    ps_mod.pixel.depth_stencil_mode =
        ps->shader->implicit_early_z_write_allowed() &&
                (!ps->shader->writes_color_target(0) ||
                 !rg::draw_util::DoesCoverageDependOnAlpha(regs.Get<RB_COLORCONTROL>()))
            ? DepthStencilMode::kEarlyHint
            : DepthStencilMode::kNoModifiers;
    out.pixel = impl_->Compile(*ps, ps_mod.value, ps_hash);
    out.pixel_info = ps->shader.get();
    out.pixel_key = ps_hash ^ (ps_mod.value * 0x9E3779B97F4A7C15ull);
    if (!out.pixel) return false;
  }
  return out.vertex != nullptr;
}

void ShaderCache::LogStats() {
  REXLOG_INFO("NATIVE-SH guest shaders={} compiled={} failed={}", impl_->shaders.size(),
              impl_->translated, impl_->failed);
}

}  // namespace native
