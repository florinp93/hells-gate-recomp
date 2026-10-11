#include "renderer.h"

#include "button_glyphs.h"
#include "draw_renderer.h"
#include "guest_gpu_state.h"
#include "png_dump.h"
#include "render_stats.h"
#include "shader_cache.h"
#include "smaa_pass.h"
#include "swizzle.h"
#include "texture_cache.h"
#include "vulkan_device.h"

#include "Common/interface/RefCntAutoPtr.hpp"
#include "Graphics/GraphicsEngine/interface/Buffer.h"
#include "Graphics/GraphicsEngine/interface/DeviceContext.h"
#include "Graphics/GraphicsEngine/interface/Fence.h"
#include "Graphics/GraphicsEngine/interface/PipelineState.h"
#include "Graphics/GraphicsEngine/interface/RenderDevice.h"
#include "Graphics/GraphicsEngine/interface/Sampler.h"
#include "Graphics/GraphicsEngine/interface/ShaderResourceBinding.h"
#include "Graphics/GraphicsEngine/interface/Texture.h"
#include "Graphics/GraphicsEngine/interface/TextureView.h"
#include "Graphics/GraphicsTools/interface/MapHelper.hpp"

#include <rex/cvar.h>
#include <rex/graphics/pipeline/texture/util.h>
#include <rex/graphics/register_file.h>
#include <rex/graphics/registers.h>
#include <rex/logging/macros.h>
#include <rex/ui/window.h>

#include <fmt/format.h>
#include <xxhash.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

REXCVAR_DEFINE_INT32(dante_debug_view, 0, "Diagnostics",
                     "renderer=dante: 0 presents the front buffer, N presents the N-th "
                     "color host render target (F11 cycles)")
    .range(0, 64);

REXCVAR_DEFINE_INT32(dante_resolution_scale, 1, "Graphics",
                     "renderer=dante: render targets at N times the game's resolution "
                     "(2: 1280x720 -> 2560x1440); read at startup")
    .range(1, 4);

REXCVAR_DEFINE_STRING(dante_resolution, "", "Graphics",
                      "renderer=dante: render resolution, 'WxH' (e.g. 1920x1080, 3440x1440) or "
                      "'auto' (window size). Wider than 16:9 sets the game's display aspect. "
                      "Overrides dante_resolution_scale; read at startup");

REXCVAR_DEFINE_INT32(dante_shadow_scale, 2, "Graphics",
                     "renderer=dante: shadow map resolution relative to the frame's (1 = "
                     "original, 2 or 4 = sharper shadows); read at startup")
    .range(1, 4);

REXCVAR_DEFINE_BOOL(dante_smaa, true, "Graphics",
                    "renderer=dante: SMAA anti-aliasing of the presented frame");

REXCVAR_DEFINE_BOOL(dante_depth_float, false, "Diagnostics",
                    "renderer=dante: 32-bit float depth for every depth buffer, as on GPUs "
                    "without 24-bit depth (AMD); read at startup");

REXCVAR_DEFINE_INT32(dante_replay_frames, 180, "Diagnostics",
                     "renderer=dante: keep the last N presented frames (640x360) for an "
                     "instant replay; 0 disables")
    .range(0, 1200);

REXCVAR_DEFINE_BOOL(dante_replay_dump, false, "Diagnostics",
                    "renderer=dante: write the instant replay to frame_dump/ (Home)");

REXCVAR_DEFINE_INT32(dante_record_frames, 0, "Diagnostics",
                     "renderer=dante: save the next N presented frames as PNG to "
                     "frame_dump/record/ (F5 starts / stops a 3600 frame recording)")
    .range(0, 1000000);

namespace renderer {
namespace {

using namespace guest;
namespace dl = Diligent;

// Surface object (docs/NATIVE_D3D_MAP.md): +0x18 RB_SURFACE_INFO value,
// +0x1C RB_COLOR_INFO / RB_DEPTH_INFO value, +0x34 size in EDRAM tiles.
struct SurfaceDesc {
  uint32_t base_tile = 0;
  uint32_t pitch = 0;
  uint32_t format = 0;
  bool depth = false;
  uint32_t width = 0;
  uint32_t height = 0;
};

// Host render target identity: what the EDRAM region holds.
struct RenderTargetKey {
  uint32_t base_tile;
  uint32_t pitch;
  uint32_t format;
  bool depth;
  bool operator==(const RenderTargetKey&) const = default;
};
struct RenderTargetKeyHash {
  size_t operator()(const RenderTargetKey& k) const {
    return (size_t(k.base_tile) << 32) ^ (size_t(k.pitch) << 16) ^ (k.format << 1) ^ k.depth;
  }
};

// Host pixels per guest pixel of a target or resolve texture.
struct Scale {
  float x = 1.0f, y = 1.0f;
  bool operator==(const Scale&) const = default;
  uint32_t HostX(uint32_t size) const { return uint32_t(std::lround(double(size) * x)); }
  uint32_t HostY(uint32_t size) const { return uint32_t(std::lround(double(size) * y)); }
};

struct RenderTarget {
  uint32_t width = 0;
  uint32_t height = 0;
  dl::TEXTURE_FORMAT format = dl::TEX_FORMAT_UNKNOWN;
  dl::RefCntAutoPtr<dl::ITexture> texture;
  Scale scale;
  bool guest_float_depth = false;                    // guest D24FS8
  dl::RefCntAutoPtr<dl::ITextureView> stencil_view;  // depth: stencil SRV, on demand
};

// Native stand-in for a guest texture written by Resolve; no copy goes back to
// guest memory. The game resolves different sizes/formats to the same address
// within a frame, so the key is the full texture description.
struct ResolveTargetKey {
  uint32_t address;
  uint32_t width;
  uint32_t height;
  uint32_t guest_format;
  // 0 for 2D destinations, 1 + slice for a slice of a 3D texture.
  uint32_t slice = 0;
  bool operator==(const ResolveTargetKey&) const = default;
};
struct ResolveTargetKeyHash {
  size_t operator()(const ResolveTargetKey& k) const {
    return (size_t(k.address) << 24) ^ (size_t(k.width) << 12) ^ k.height ^
           (size_t(k.guest_format) << 40) ^ (size_t(k.slice) << 48);
  }
};
struct ResolveTarget {
  uint32_t width = 0;
  uint32_t height = 0;
  uint32_t guest_format = 0;
  dl::TEXTURE_FORMAT format = dl::TEX_FORMAT_UNKNOWN;
  dl::RefCntAutoPtr<dl::ITexture> texture;
  // Last resolve had RB_COPY_DEST_INFO.copy_dest_swap: guest memory holds
  // red and blue exchanged relative to the host texture.
  bool rb_swap = false;
  // Write-back to guest memory: staging copy, fence value it waits for (0 =
  // none pending) and the destination fetch constant.
  dl::RefCntAutoPtr<dl::ITexture> staging;
  uint64_t readback_fence = 0;
  uint32_t readback_fetch[6] = {};
  bool written_back = false;
  // Destination is slice `slice` of a 3D texture `volume_depth` deep (0: 2D).
  uint32_t volume_depth = 0, slice = 0;
  Scale scale;
  // Depth resolves (k_24_8) the game also samples as k_8_8_8_8: the guest
  // memory holds the raw D24S8 words, so those reads get this RGBA8 copy
  // (R = stencil, G / B / A = depth bits 0:7 / 8:15 / 16:23).
  bool depth_bits_wanted = false;
  dl::RefCntAutoPtr<dl::ITexture> depth_bits;
};

// Last non-zero value written to a register in (start, end], or ~0u (Resolve
// resets the copy registers to 0 after the copy).
uint32_t FindRegisterWrite(const uint8_t* base, uint32_t start, uint32_t end, uint32_t reg) {
  uint32_t value = ~0u;
  if (!IsPlausibleAddress(start) || !IsPlausibleAddress(end) || end <= start ||
      end - start > 0x40000)
    return value;
  for (uint32_t p = start + 4; p <= end;) {
    uint32_t header = Load32(base, p);
    uint32_t type = header >> 30;
    uint32_t count = type == 2 ? 0 : type == 1 ? 2 : ((header >> 16) & 0x3FFF) + 1;
    if (p + count * 4 > end) break;
    if (type == 0) {
      uint32_t first = header & 0x7FFF;
      bool one_reg = (header >> 15) & 1;
      for (uint32_t i = 0; i < count; ++i) {
        uint32_t v = Load32(base, p + 4 + i * 4);
        if ((one_reg ? first : first + i) == reg && v) value = v;
      }
    } else if (type == 3 && ((header >> 8) & 0x7F) == 0x2D && count >= 2) {
      // SET_CONSTANT, type 4 = registers from 0x2000.
      uint32_t d1 = Load32(base, p + 4);
      if (((d1 >> 16) & 0xFF) == 4) {
        uint32_t first = 0x2000 + (d1 & 0x7FF);
        for (uint32_t i = 1; i < count; ++i) {
          uint32_t v = Load32(base, p + 4 + i * 4);
          if (first + i - 1 == reg && v) value = v;
        }
      }
    }
    p += 4 + count * 4;
  }
  return value;
}

std::mutex g_mutex;
// Host render targets and resolve targets are round(guest size * scale).
float g_scale_x = 1.0f, g_scale_y = 1.0f;
// Render and resolve targets created since the last swap (replay notes).
uint32_t g_swap_new_targets = 0;

struct RenderScale {
  float x = 1.0f, y = 1.0f;
  double display_aspect = 16.0 / 9.0;
};

// Render scale from dante_resolution ('WxH' / 'auto') or dante_resolution_scale.
// The picture aspect is ultrawide_target_aspect, or the resolution's when 0;
// the picture fills the largest box of that aspect within the resolution.
// 4:3 (or narrower) uses the game's own standard-definition mode, chosen from
// a 640x480 video mode: UI laid out for 4:3, 3D still rendered into the 16:9
// 1280x720 targets and squeezed to 4:3 like the console's scaler. Wider than
// 16:9 sets the game's display aspect. Both are rendered anamorphically.
// Aspects between 4:3 and 16:9 stay 16:9, letterboxed.
RenderScale ComputeRenderScale(rex::ui::Window* window) {
  std::string mode = REXCVAR_GET(dante_resolution);
  uint32_t width = 0, height = 0;
  if (mode == "auto") {
    if (window) {
      width = window->GetActualPhysicalWidth();
      height = window->GetActualPhysicalHeight();
    }
  } else if (!mode.empty()) {
    std::sscanf(mode.c_str(), "%ux%u", &width, &height);
  }
  if (!mode.empty() && !(width && height)) {
    REXLOG_WARN("DanteDevice: dante_resolution '{}' not understood", mode);
  }
  RenderScale scale;
  double aspect = rex::cvar::Query<double>("ultrawide_target_aspect");
  if (aspect <= 0.0) aspect = width && height ? double(width) / height : 16.0 / 9.0;
  // Height of the picture's box within the resolution.
  auto box_height = [&](double box_aspect) {
    if (width && height) return float(std::min(double(height), width / box_aspect));
    return 720.0f * float(std::clamp(REXCVAR_GET(dante_resolution_scale), 1, 4));
  };
  if (aspect <= 4.0 / 3.0 + 0.01) {
    rex::cvar::SetFlagByName("video_mode_width", "640");
    rex::cvar::SetFlagByName("video_mode_height", "480");
    rex::cvar::SetFlagByName("ultrawide_target_aspect", "0");
    scale.y = std::max(1.0f, box_height(4.0 / 3.0) / 720.0f);
    scale.x = std::max(1.0f, scale.y * 0.75f);
    scale.display_aspect = 4.0 / 3.0;
  } else if (aspect > 16.0 / 9.0 + 0.01) {
    rex::cvar::SetFlagByName("ultrawide_target_aspect", fmt::format("{:.4f}", aspect));
    scale.y = std::max(1.0f, box_height(aspect) / 720.0f);
    scale.x = std::max(1.0f, float(720.0 * scale.y * aspect) / 1280.0f);
    scale.display_aspect = aspect;
  } else {
    rex::cvar::SetFlagByName("ultrawide_target_aspect", "0");
    scale.x = scale.y = std::max(1.0f, box_height(16.0 / 9.0) / 720.0f);
  }
  return scale;
}

std::unique_ptr<VulkanDevice> g_device;
uint64_t g_frame = 0;
bool g_log_this_frame = false;
// First frame of an F8 capture: every resolve output and the front buffer are
// written to FrameDumpDirectory() as PNGs.
bool g_dump_this_frame = false;
uint32_t g_dump_seq = 0;
uint32_t g_logged_run_draws = 0;
RenderTargetKey g_log_draw_target{};
std::unordered_map<RenderTargetKey, RenderTarget, RenderTargetKeyHash> g_render_targets;
// Depth targets drawn without color targets (shadow maps): rendered at
// ShadowMapScale(), above the frame's resolution.
std::unordered_set<RenderTargetKey, RenderTargetKeyHash> g_shadow_maps;
float g_shadow_map_factor = 1.0f;

Scale ShadowMapScale() {
  float s = std::max(g_scale_x, g_scale_y) * g_shadow_map_factor;
  return {s, s};
}
std::unordered_map<ResolveTargetKey, ResolveTarget, ResolveTargetKeyHash> g_resolve_targets;
std::set<uint64_t> g_reported_conversions;
std::unique_ptr<ShaderCache> g_shader_cache;
std::unique_ptr<TextureCache> g_texture_cache;
std::unique_ptr<DrawRenderer> g_draw_renderer;
std::unique_ptr<SmaaPass> g_smaa;
std::unique_ptr<ButtonGlyphs> g_button_glyphs;
std::function<GlyphStyle()> g_glyph_style_source;
std::function<KeyBindings()> g_glyph_bindings_source;
std::unique_ptr<rex::graphics::RegisterFile> g_register_file;
uint32_t g_draws_without_shaders = 0;
uint32_t g_draws_unparsed = 0;

// Last BeginVertices: drawn at EndVertices, or by the enclosing DrawVerticesUP.
struct InlineDraw {
  bool pending = false;
  CallArgs args{};
  uint32_t prim = 0;
  uint32_t count = 0;
  uint32_t stride = 0;
  uint32_t data = 0;
};
InlineDraw g_pending_inline_draw;

// VGT_DRAW_INITIATOR of an inline draw: auto-indexed (source select 2).
uint32_t InlineDrawInitiator(const InlineDraw& draw) {
  return (draw.count << 16) | (2u << 6) | (draw.prim & 0x3F);
}

bool Is64bpp(uint32_t color_format) {
  return color_format == 5 || color_format == 7 || color_format == 15;
}

dl::TEXTURE_FORMAT ColorFormat(uint32_t format) {
  switch (format) {
    case 0:  // 8_8_8_8
    case 1:  // 8_8_8_8_GAMMA
      return dl::TEX_FORMAT_RGBA8_UNORM;
    case 2:   // 2_10_10_10
    case 10:  // 2_10_10_10_AS_10_10_10_10
      return dl::TEX_FORMAT_RGB10A2_UNORM;
    case 4:  // 16_16 (fixed -32..32)
    case 6:  // 16_16_FLOAT
      return dl::TEX_FORMAT_RG16_FLOAT;
    case 3:   // 2_10_10_10_FLOAT
    case 5:   // 16_16_16_16 (fixed -32..32)
    case 7:   // 16_16_16_16_FLOAT
    case 12:  // 2_10_10_10_FLOAT_AS_16_16_16_16
      return dl::TEX_FORMAT_RGBA16_FLOAT;
    case 14: return dl::TEX_FORMAT_R32_FLOAT;
    case 15: return dl::TEX_FORMAT_RG32_FLOAT;
    default: return dl::TEX_FORMAT_RGBA8_UNORM;
  }
}

// Guest D24S8 uses host D24S8 when the GPU can render to and sample it (AMD
// can't), otherwise D32S8 like D24FS8. The 2^24 depth bias factor suits both.
bool g_use_d24_depth = true;

dl::TEXTURE_FORMAT DepthFormat(uint32_t format) {
  return format == 1 || !g_use_d24_depth ? dl::TEX_FORMAT_D32_FLOAT_S8X24_UINT
                                         : dl::TEX_FORMAT_D24_UNORM_S8_UINT;
}

// Guest texture format (fetch constant dword1 & 0x3F) as a resolve destination.
dl::TEXTURE_FORMAT ResolveTargetFormat(uint32_t format) {
  switch (format) {
    case 2:  // k_8
      return dl::TEX_FORMAT_R8_UNORM;
    case 6:  // k_8_8_8_8
      return dl::TEX_FORMAT_RGBA8_UNORM;
    case 7:  // k_2_10_10_10
      return dl::TEX_FORMAT_RGB10A2_UNORM;
    case 25:  // k_16_16
    case 31:  // k_16_16_FLOAT
      return dl::TEX_FORMAT_RG16_FLOAT;
    case 26:  // k_16_16_16_16
    case 32:  // k_16_16_16_16_FLOAT
      return dl::TEX_FORMAT_RGBA16_FLOAT;
    case 22:  // k_24_8 (depth)
    case 23:  // k_24_8_FLOAT
    case 36:  // k_32_FLOAT
      return dl::TEX_FORMAT_R32_FLOAT;
    case 37:  // k_32_32_FLOAT
      return dl::TEX_FORMAT_RG32_FLOAT;
    default: return dl::TEX_FORMAT_RGBA8_UNORM;
  }
}

bool ReadSurface(const uint8_t* base, uint32_t surface, bool depth, SurfaceDesc& out) {
  if (!IsPlausibleAddress(surface)) return false;
  uint32_t surface_info = Load32(base, surface + 0x18);
  uint32_t info = Load32(base, surface + 0x1C);
  uint32_t tiles = Load32(base, surface + 0x34);
  out.depth = depth;
  out.base_tile = info & 0xFFF;
  out.format = (info >> 16) & 0xF;
  out.pitch = surface_info & 0x3FFF;
  if (!out.pitch) return false;
  // EDRAM tiles are 80x16 samples at 32bpp.
  uint32_t tiles_per_row = (out.pitch + 79) / 80 * ((!depth && Is64bpp(out.format)) ? 2 : 1);
  out.width = out.pitch;
  out.height = tiles_per_row ? tiles / tiles_per_row * 16 : 0;
  if (!out.height) out.height = 16;
  return true;
}

// New textures start with undefined contents; guest memory starts zeroed.
void ClearNewTexture(dl::ITexture* texture, bool depth) {
  auto* context = g_device->Context();
  if (depth) {
    context->ClearDepthStencil(texture->GetDefaultView(dl::TEXTURE_VIEW_DEPTH_STENCIL),
                               dl::CLEAR_DEPTH_FLAG | dl::CLEAR_STENCIL_FLAG, 1.0f, 0,
                               dl::RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
  } else {
    const float zero[4] = {};
    context->ClearRenderTarget(texture->GetDefaultView(dl::TEXTURE_VIEW_RENDER_TARGET), zero,
                               dl::RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
  }
}

RenderTarget* GetRenderTarget(const SurfaceDesc& s) {
  RenderTargetKey key{s.base_tile, s.pitch, s.format, s.depth};
  Scale scale{g_scale_x, g_scale_y};
  if (g_shadow_maps.count(key)) {
    scale = ShadowMapScale();
    // Within the device's largest texture.
    float limit = 16384.0f / float(std::max(s.width, s.height));
    scale.x = scale.y = std::min(scale.x, limit);
  }
  RenderTarget& rt = g_render_targets[key];
  if (rt.texture && rt.width == s.width && rt.height >= s.height && rt.scale == scale) return &rt;
  dl::TextureDesc desc;
  desc.Name = s.depth ? "EdramDepth" : "EdramColor";
  desc.Type = dl::RESOURCE_DIM_TEX_2D;
  uint32_t height = std::max(s.height, rt.height);
  desc.Width = scale.HostX(s.width);
  desc.Height = scale.HostY(height);
  desc.MipLevels = 1;
  desc.Format = s.depth ? DepthFormat(s.format) : ColorFormat(s.format);
  desc.BindFlags =
      (s.depth ? dl::BIND_DEPTH_STENCIL : dl::BIND_RENDER_TARGET) | dl::BIND_SHADER_RESOURCE;
  desc.Usage = dl::USAGE_DEFAULT;
  rt.texture.Release();
  g_device->RenderDevice()->CreateTexture(desc, nullptr, &rt.texture);
  if (!rt.texture) {
    REXLOG_ERROR("DanteDevice: failed to create host RT base={} pitch={} fmt={} depth={}",
                 s.base_tile, s.pitch, s.format, s.depth);
    g_render_targets.erase(key);
    return nullptr;
  }
  ClearNewTexture(rt.texture, s.depth);
  ++g_swap_new_targets;
  rt.width = s.width;
  rt.height = height;
  rt.format = desc.Format;
  rt.scale = scale;
  rt.guest_float_depth = s.depth && s.format == 1;
  REXLOG_INFO("NATIVE-RT new {} RT: edram base={} pitch={} fmt={} -> {}x{} (host {}x{})",
              s.depth ? "depth" : "color", s.base_tile, s.pitch, s.format, rt.width, rt.height,
              desc.Width, desc.Height);
  return &rt;
}

// `scale` is the scale of the render target being resolved.
ResolveTarget* GetResolveTarget(uint32_t address, uint32_t width, uint32_t height,
                                uint32_t guest_format, uint32_t slice, Scale scale) {
  ResolveTargetKey key{address, width, height, guest_format, slice};
  ResolveTarget& t = g_resolve_targets[key];
  if (t.texture && t.scale == scale) return &t;
  dl::TextureDesc desc;
  desc.Name = "ResolveTarget";
  desc.Type = dl::RESOURCE_DIM_TEX_2D;
  desc.Width = scale.HostX(width);
  desc.Height = scale.HostY(height);
  desc.MipLevels = 1;
  desc.Format = ResolveTargetFormat(guest_format);
  desc.BindFlags = dl::BIND_SHADER_RESOURCE | dl::BIND_RENDER_TARGET;
  desc.Usage = dl::USAGE_DEFAULT;
  t.texture.Release();
  t.depth_bits.Release();
  g_device->RenderDevice()->CreateTexture(desc, nullptr, &t.texture);
  if (!t.texture) {
    REXLOG_ERROR("DanteDevice: failed to create resolve target {:08X} {}x{} fmt={}", address, width,
                 height, guest_format);
    g_resolve_targets.erase(key);
    return nullptr;
  }
  ClearNewTexture(t.texture, false);
  ++g_swap_new_targets;
  t.width = width;
  t.height = height;
  t.guest_format = guest_format;
  t.format = desc.Format;
  t.scale = scale;
  REXLOG_INFO("NATIVE-RT new resolve target {:08X} {}x{} guest fmt={}", address, width, height,
              guest_format);
  return &t;
}

// Full-screen passes that read a depth target texel by texel into a colour
// texture, offset by g_SourceOffset (source texel - destination pixel).
struct DepthPass {
  dl::RefCntAutoPtr<dl::IPipelineState> pso;
  dl::RefCntAutoPtr<dl::IShaderResourceBinding> srb;
  dl::RefCntAutoPtr<dl::IBuffer> params;
  bool failed = false;
};

bool EnsureDepthPass(DepthPass& d, const char* name, const char* pixel_source,
                     dl::TEXTURE_FORMAT format) {
  if (d.pso || d.failed) return d.pso;
  d.failed = true;
  d.pso.Attach(g_device->CreateFullscreenPipeline(name, pixel_source, format, true));
  if (!d.pso) return false;
  d.pso->CreateShaderResourceBinding(&d.srb, true);
  dl::BufferDesc params;
  params.Name = name;
  params.Size = 16;
  params.BindFlags = dl::BIND_UNIFORM_BUFFER;
  params.Usage = dl::USAGE_DYNAMIC;
  params.CPUAccessFlags = dl::CPU_ACCESS_WRITE;
  g_device->RenderDevice()->CreateBuffer(params, nullptr, &d.params);
  dl::SamplerDesc point;
  point.MinFilter = point.MagFilter = point.MipFilter = dl::FILTER_TYPE_POINT;
  dl::RefCntAutoPtr<dl::ISampler> sampler;
  g_device->RenderDevice()->CreateSampler(point, &sampler);
  if (!d.params || !sampler) return false;
  d.srb->GetVariableByName(dl::SHADER_TYPE_PIXEL, "g_Point")->Set(sampler);
  d.srb->GetVariableByName(dl::SHADER_TYPE_PIXEL, "CopyParams")->Set(d.params);
  d.failed = false;
  return true;
}

// Runs the pass on depth texels [x1, x2) x [y1, y2), written to (dst_x, dst_y)
// of `dest`. The caller has bound the depth inputs.
void RunDepthPass(DepthPass& d, dl::ITexture* dest, uint32_t x1, uint32_t y1, uint32_t x2,
                  uint32_t y2, uint32_t dst_x, uint32_t dst_y) {
  auto* context = g_device->Context();
  {
    dl::MapHelper<int32_t> map(context, d.params, dl::MAP_WRITE, dl::MAP_FLAG_DISCARD);
    map[0] = int32_t(x1) - int32_t(dst_x);
    map[1] = int32_t(y1) - int32_t(dst_y);
    map[2] = map[3] = 0;
  }
  dl::ITextureView* rtv = dest->GetDefaultView(dl::TEXTURE_VIEW_RENDER_TARGET);
  context->SetRenderTargets(1, &rtv, nullptr, dl::RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
  const auto& dest_desc = dest->GetDesc();
  dl::Viewport vp;
  vp.Width = float(dest_desc.Width);
  vp.Height = float(dest_desc.Height);
  context->SetViewports(1, &vp, dest_desc.Width, dest_desc.Height);
  dl::Rect rect{int32_t(dst_x), int32_t(dst_y), int32_t(dst_x + (x2 - x1)),
                int32_t(dst_y + (y2 - y1))};
  context->SetScissorRects(1, &rect, dest_desc.Width, dest_desc.Height);
  context->SetPipelineState(d.pso);
  context->CommitShaderResources(d.srb, dl::RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
  dl::DrawAttribs draw;
  draw.NumVertices = 3;
  context->Draw(draw);
}

// Resolve of a depth surface into a sampled depth texture (k_24_8 /
// k_24_8_FLOAT -> R32_FLOAT, depth in red).
DepthPass g_depth_copy;

constexpr char kDepthCopyPS[] = R"(#version 450
layout(binding = 0) uniform texture2D g_Depth;
layout(binding = 1) uniform sampler g_Point;
layout(std140, binding = 2) uniform CopyParams { ivec4 g_SourceOffset; };
layout(location = 0) out float out_depth;
void main() {
  ivec2 texel = ivec2(gl_FragCoord.xy) + g_SourceOffset.xy;
  out_depth = texelFetch(sampler2D(g_Depth, g_Point), texel, 0).r;
}
)";

void CopyDepth(RenderTarget& depth, dl::ITexture* dest, uint32_t x1, uint32_t y1, uint32_t x2,
               uint32_t y2, uint32_t dst_x, uint32_t dst_y) {
  DepthPass& d = g_depth_copy;
  if (!EnsureDepthPass(d, "DepthCopy", kDepthCopyPS, dl::TEX_FORMAT_R32_FLOAT)) return;
  d.srb->GetVariableByName(dl::SHADER_TYPE_PIXEL, "g_Depth")
      ->Set(depth.texture->GetDefaultView(dl::TEXTURE_VIEW_SHADER_RESOURCE));
  RunDepthPass(d, dest, x1, y1, x2, y2, dst_x, dst_y);
}

// Raw D24S8 words of a depth surface as RGBA8 (see ResolveTarget::depth_bits).
DepthPass g_depth_bits;

constexpr char kDepthBitsPS[] = R"(#version 450
layout(binding = 0) uniform texture2D g_Depth;
layout(binding = 1) uniform utexture2D g_Stencil;
layout(binding = 2) uniform sampler g_Point;
layout(std140, binding = 3) uniform CopyParams { ivec4 g_SourceOffset; };
layout(location = 0) out vec4 out_bits;
void main() {
  ivec2 texel = ivec2(gl_FragCoord.xy) + g_SourceOffset.xy;
  float depth = texelFetch(sampler2D(g_Depth, g_Point), texel, 0).r;
  uint stencil = texelFetch(usampler2D(g_Stencil, g_Point), texel, 0).r;
  uint bits = uint(clamp(depth, 0.0, 1.0) * 16777215.0 + 0.5);
  out_bits = vec4(uvec4(stencil & 0xFFu, bits & 0xFFu, (bits >> 8) & 0xFFu, bits >> 16)) / 255.0;
}
)";

void CopyDepthBits(RenderTarget& depth, dl::ITexture* dest, uint32_t x1, uint32_t y1, uint32_t x2,
                   uint32_t y2, uint32_t dst_x, uint32_t dst_y) {
  DepthPass& d = g_depth_bits;
  if (!EnsureDepthPass(d, "DepthBits", kDepthBitsPS, dl::TEX_FORMAT_RGBA8_UNORM)) return;
  if (!depth.stencil_view) {
    dl::TextureViewDesc view;
    view.ViewType = dl::TEXTURE_VIEW_SHADER_RESOURCE;
    view.Format = depth.format == dl::TEX_FORMAT_D24_UNORM_S8_UINT
                      ? dl::TEX_FORMAT_X24_TYPELESS_G8_UINT
                      : dl::TEX_FORMAT_X32_TYPELESS_G8X24_UINT;
    depth.texture->CreateView(view, &depth.stencil_view);
    if (!depth.stencil_view) return;
  }
  d.srb->GetVariableByName(dl::SHADER_TYPE_PIXEL, "g_Depth")
      ->Set(depth.texture->GetDefaultView(dl::TEXTURE_VIEW_SHADER_RESOURCE));
  d.srb->GetVariableByName(dl::SHADER_TYPE_PIXEL, "g_Stencil")->Set(depth.stencil_view);
  RunDepthPass(d, dest, x1, y1, x2, y2, dst_x, dst_y);
}

void ClearColor(RenderTarget& rt, const float color[4]) {
  auto* rtv = rt.texture->GetDefaultView(dl::TEXTURE_VIEW_RENDER_TARGET);
  g_device->Context()->ClearRenderTarget(rtv, color, dl::RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
}

void ClearDepth(RenderTarget& rt, bool depth, bool stencil, float z, uint8_t stencil_value) {
  dl::CLEAR_DEPTH_STENCIL_FLAGS flags = dl::CLEAR_DEPTH_FLAG_NONE;
  if (depth) flags |= dl::CLEAR_DEPTH_FLAG;
  if (stencil) flags |= dl::CLEAR_STENCIL_FLAG;
  auto* dsv = rt.texture->GetDefaultView(dl::TEXTURE_VIEW_DEPTH_STENCIL);
  g_device->Context()->ClearDepthStencil(dsv, flags, z, stencil_value,
                                         dl::RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
}

// Draw counts are logged when the next clear/resolve/swap or RT change ends a run.
void FlushDrawLog() {
  if (!g_log_this_frame || !g_logged_run_draws) return;
  REXLOG_INFO("NATIVE-RT f{} draws={} on RT0 base={} pitch={} fmt={}", g_frame, g_logged_run_draws,
              g_log_draw_target.base_tile, g_log_draw_target.pitch, g_log_draw_target.format);
  g_logged_run_draws = 0;
}

// Vertex shader microcode last loaded by a program flush (guest physical, dwords).
struct LoadedShader {
  uint32_t address = 0, dwords = 0;
};
LoadedShader g_loaded_vs;
// Pixel shader microcode of the last draw with a D3D pixel shader set.
GuestShaderCode g_last_ps;
// Index buffer of the last submitted draw (checked by the state verifier).
uint32_t g_last_index_base = 0, g_last_index_count = 0, g_last_index_format = 0;
bool g_in_program_flush = false;
// Draws and resolves since the last swap (frame recorder log).
uint32_t g_swap_draws = 0, g_swap_resolves = 0, g_swap_dest_info_missing = 0;
uint32_t g_flush_start = 0;

// Small resolves and resolves into 3D textures are written back to guest
// memory, one frame late: the game reads some on the CPU (64x8 k_32_FLOAT scene
// luminance -> exposure) and samples 3D ones (colour grading LUTs rendered
// slice by slice), which the texture cache loads from guest memory.
constexpr uint32_t kReadbackMaxTexels = 4096;
dl::RefCntAutoPtr<dl::IFence> g_readback_fence;
uint64_t g_readback_value = 0;

void QueueReadback(ResolveTarget& t, const uint8_t* base, uint32_t dest) {
  using rex::graphics::xenos::TextureFormat;
  if ((t.guest_format != uint32_t(TextureFormat::k_32_FLOAT) &&
       t.guest_format != uint32_t(TextureFormat::k_8_8_8_8)) ||
      (!t.volume_depth && t.width * t.height > kReadbackMaxTexels) || t.readback_fence) {
    return;
  }
  auto* device = g_device->RenderDevice();
  if (!g_readback_fence) {
    dl::FenceDesc desc;
    desc.Name = "ResolveReadback";
    device->CreateFence(desc, &g_readback_fence);
    if (!g_readback_fence) return;
  }
  if (!t.staging) {
    dl::TextureDesc desc;
    desc.Name = "ResolveReadback";
    desc.Type = dl::RESOURCE_DIM_TEX_2D;
    desc.Width = t.scale.HostX(t.width);
    desc.Height = t.scale.HostY(t.height);
    desc.Format = t.format;
    desc.Usage = dl::USAGE_STAGING;
    desc.CPUAccessFlags = dl::CPU_ACCESS_READ;
    device->CreateTexture(desc, nullptr, &t.staging);
    if (!t.staging) return;
  }
  auto* context = g_device->Context();
  dl::CopyTextureAttribs copy(t.texture, dl::RESOURCE_STATE_TRANSITION_MODE_TRANSITION, t.staging,
                              dl::RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
  context->CopyTexture(copy);
  context->EnqueueSignal(g_readback_fence, ++g_readback_value);
  t.readback_fence = g_readback_value;
  for (uint32_t i = 0; i < 6; ++i) t.readback_fetch[i] = Load32(base, dest + 0x1C + i * 4);
}

// Copies finished readbacks into guest memory (tiling and endianness of the
// destination fetch constant).
void FinishReadbacks(const uint8_t* base) {
  if (!g_readback_fence) return;
  uint64_t completed = g_readback_fence->GetCompletedValue();
  auto* context = g_device->Context();
  uint8_t* memory = const_cast<uint8_t*>(PhysicalPtr(base, 0));
  for (auto& [key, t] : g_resolve_targets) {
    if (!t.readback_fence || t.readback_fence > completed) continue;
    t.readback_fence = 0;
    rex::graphics::xenos::xe_gpu_texture_fetch_t fetch;
    std::memcpy(&fetch, t.readback_fetch, sizeof(fetch));
    uint32_t pitch = std::max(uint32_t(fetch.pitch) << 5, t.width);
    dl::MappedTextureSubresource mapped;
    context->MapTextureSubresource(t.staging, 0, 0, dl::MAP_READ, dl::MAP_FLAG_DO_NOT_WAIT, nullptr,
                                   mapped);
    if (!mapped.pData) continue;
    // Scaled targets: the centre texel of each guest texel.
    for (uint32_t y = 0; y < t.height; ++y) {
      const auto* row = reinterpret_cast<const uint32_t*>(
          static_cast<const uint8_t*>(mapped.pData) +
          size_t(std::min(uint32_t((y + 0.5f) * t.scale.y), t.scale.HostY(t.height) - 1)) *
              mapped.Stride);
      for (uint32_t x = 0; x < t.width; ++x) {
        uint32_t offset;
        if (t.volume_depth) {
          offset = fetch.tiled ? uint32_t(rex::graphics::texture_util::GetTiledOffset3D(
                                     int32_t(x), int32_t(y), int32_t(t.slice), pitch, t.height, 2))
                               : ((t.slice * ((t.height + 31) & ~31u) + y) * pitch + x) * 4;
        } else {
          offset = fetch.tiled ? uint32_t(rex::graphics::texture_util::GetTiledOffset2D(
                                     int32_t(x), int32_t(y), pitch, 2))
                               : (y * pitch + x) * 4;
        }
        uint32_t texel =
            row[std::min(uint32_t((x + 0.5f) * t.scale.x), t.scale.HostX(t.width) - 1)];
        if (t.rb_swap)
          texel = (texel & 0xFF00FF00u) | ((texel >> 16) & 0xFF) | ((texel & 0xFF) << 16);
        uint32_t value = rex::graphics::xenos::GpuSwap(texel, fetch.endianness);
        std::memcpy(memory + key.address + offset, &value, 4);
      }
    }
    context->UnmapTextureSubresource(t.staging, 0, 0);
    if (!t.written_back) {
      t.written_back = true;
      REXLOG_INFO(
          "NATIVE-RT resolve {:08X} {}x{} written back to guest memory (tiled={} endian={})",
          key.address, t.width, t.height, bool(fetch.tiled), uint32_t(fetch.endianness));
    }
  }
}

// Parses the PM4 packets written in (start, end] (end = last dword written).
void ScanProgramFlush(const uint8_t* base, uint32_t start, uint32_t end) {
  if (!IsPlausibleAddress(start) || !IsPlausibleAddress(end) || end <= start ||
      end - start > 0x40000)
    return;
  uint32_t p = start + 4;
  LoadedShader found;
  while (p <= end) {
    uint32_t header = Load32(base, p);
    uint32_t type = header >> 30;
    uint32_t count = type == 2 ? 0 : type == 1 ? 2 : ((header >> 16) & 0x3FFF) + 1;
    if (p + count * 4 > end) return;
    if (type == 3) {
      uint32_t opcode = (header >> 8) & 0x7F;
      if (opcode == 0x27 && count >= 2) {  // IM_LOAD: [addr | type] [start << 16 | size]
        uint32_t d1 = Load32(base, p + 4), d2 = Load32(base, p + 8);
        if ((d1 & 3) == 0) found = {d1 & ~3u, d2 & 0xFFFF};
      } else if (opcode == 0x2B && count >= 2) {  // IM_LOAD_IMMEDIATE: [type] [size] ucode
        uint32_t d1 = Load32(base, p + 4), d2 = Load32(base, p + 8);
        if ((d1 & 3) == 0) found = {GpuAddress(p + 12), d2 & 0xFFFF};
      }
    }
    p += 4 + count * 4;
  }
  if (found.dwords) g_loaded_vs = found;
}

}  // namespace

uint32_t CommandBufferWritePtr(uint32_t dev, const uint8_t* base) {
  return IsPlausibleAddress(dev) ? Load32(base, dev + kDevCommandBufferWritePtr) : 0;
}

void BeforeProgramFlush(uint32_t dev, const uint8_t* base) {
  if (!g_device) return;
  g_in_program_flush = true;
  g_flush_start = Load32(base, dev + kDevCommandBufferWritePtr);
}

namespace {
struct SegmentSwitch {
  uint32_t old_ptr = 0, new_ptr = 0;
};
constexpr uint32_t kSegmentSwitchHistory = 64;
SegmentSwitch g_segment_switches[kSegmentSwitchHistory];
uint32_t g_segment_switch_count = 0;
}  // namespace

void RecordSegmentSwitch(uint32_t old_ptr, uint32_t new_ptr) {
  g_segment_switches[g_segment_switch_count % kSegmentSwitchHistory] = {old_ptr, new_ptr};
  ++g_segment_switch_count;
}

uint32_t SegmentSwitchCount() { return g_segment_switch_count; }

void AfterCommandBufferReserve(uint32_t write_ptr) {
  if (g_in_program_flush) g_flush_start = write_ptr;
}

void AfterProgramFlush(uint32_t dev, const uint8_t* base) {
  if (!g_in_program_flush) return;
  g_in_program_flush = false;
  ScanProgramFlush(base, g_flush_start, Load32(base, dev + kDevCommandBufferWritePtr));
}

namespace {

// The VS microcode the GPU runs is the one the last program flush loaded (D3D
// patches the fetches per declaration, see docs/NATIVE_D3D_MAP.md). Before the
// first flush: variant v bound to the declaration whose id (decl+48) is in
// VS[416*v + 40], ucode VS[+0x20] + blk[+872] with blk = VS + VS[+896 + 8*v].
GuestShaderCode VertexShaderCode(const uint8_t* base, uint32_t dev) {
  if (g_loaded_vs.dwords && g_loaded_vs.dwords <= 0x4000) {
    return {reinterpret_cast<const uint32_t*>(PhysicalPtr(base, g_loaded_vs.address)),
            g_loaded_vs.dwords};
  }
  uint32_t vs = Load32(base, dev + kDevVertexShader);
  uint32_t decl = Load32(base, dev + kDevVertexDecl);
  if (!IsPlausibleAddress(vs)) return {};
  uint32_t decl_id = IsPlausibleAddress(decl) ? Load32(base, decl + 48) : 0;
  uint32_t variant = 0;
  for (uint32_t v = 0; v < 2; ++v) {
    if (Load32(base, vs + 896 + 8 * v) && Load32(base, vs + 416 * v + 40) == decl_id) {
      variant = v;
      break;
    }
  }
  uint32_t blk = vs + Load32(base, vs + 896 + 8 * variant);
  uint32_t ucode = Load32(base, vs + 0x20) + Load32(base, blk + 872);
  uint32_t size = Load32(base, blk + 876);
  if (!IsPlausibleAddress(ucode) || !size || size > 0x10000) return {};
  return {reinterpret_cast<const uint32_t*>(guest::Ptr(base, ucode)), size / 4};
}

// Pixel shader: ucode = PS[+0x18] + blk[+40], size blk[+44], blk = PS + PS[+0x40].
// With a null D3D pixel shader nothing is loaded, so in color + depth mode the
// GPU still runs the previous one (the SDK backends use the active shader).
GuestShaderCode PixelShaderCode(const uint8_t* base, uint32_t dev) {
  if (g_register_file && g_register_file->Get<rex::graphics::reg::RB_MODECONTROL>().edram_mode !=
                             rex::graphics::xenos::EdramMode::kColorDepth) {
    return {};
  }
  uint32_t ps = Load32(base, dev + kDevPixelShader);
  if (!IsPlausibleAddress(ps)) return g_last_ps;
  uint32_t blk = ps + Load32(base, ps + 0x40);
  uint32_t ucode = Load32(base, ps + 0x18) + Load32(base, blk + 40);
  uint32_t size = Load32(base, blk + 44);
  if (!IsPlausibleAddress(ucode) || !size || size > 0x10000) return {};
  g_last_ps = {reinterpret_cast<const uint32_t*>(guest::Ptr(base, ucode)), size / 4};
  return g_last_ps;
}

// The draw packet ends what the D3D call wrote to the command buffer (dev+48
// is its last dword). PM4 type-3 DRAW_INDX (0x22): [hdr][viz][initiator][dma
// base][dma size] when indexed, [hdr][viz][initiator] otherwise; DRAW_INDX_2
// (0x36): [hdr][initiator].
bool ParseDrawPacket(const uint8_t* base, uint32_t dev, uint32_t& initiator, uint32_t& dma_base,
                     uint32_t& dma_size) {
  uint32_t last = Load32(base, dev + kDevCommandBufferWritePtr);
  if (!IsPlausibleAddress(last)) return false;
  auto opcode = [&](uint32_t address) {
    uint32_t header = Load32(base, address);
    return (header >> 30) == 3 ? (header >> 8) & 0x7F : 0xFFu;
  };
  if (opcode(last - 16) == 0x22 && ((Load32(base, last - 8) >> 6) & 3) == 0) {
    initiator = Load32(base, last - 8);
    dma_base = Load32(base, last - 4);
    dma_size = Load32(base, last);
    return true;
  }
  if (opcode(last - 8) == 0x22 || opcode(last - 4) == 0x36) {
    initiator = Load32(base, last);
    dma_base = dma_size = 0;
    return true;
  }
  return false;
}

// Host render targets for the bound surfaces (RT0..3 up to the first gap, depth).
DrawTargets BoundTargets(const uint8_t* base, uint32_t dev) {
  DrawTargets t;
  t.scale_x = g_scale_x;
  t.scale_y = g_scale_y;
  for (uint32_t i = 0; i < 4; ++i) {
    SurfaceDesc s;
    if (!ReadSurface(base, Load32(base, dev + kDevRenderTargets + i * 4), false, s)) break;
    RenderTarget* rt = GetRenderTarget(s);
    if (!rt) break;
    t.color[i] = rt->texture->GetDefaultView(dl::TEXTURE_VIEW_RENDER_TARGET);
    t.color_format[i] = rt->format;
    t.color_count = i + 1;
    if (!t.width) {
      t.width = rt->width;
      t.height = rt->height;
    }
  }
  SurfaceDesc ds;
  if (ReadSurface(base, Load32(base, dev + kDevDepthStencil), true, ds)) {
    RenderTargetKey key{ds.base_tile, ds.pitch, ds.format, true};
    if (!t.color_count && ds.width == ds.height && g_shadow_map_factor != 1.0f &&
        g_shadow_maps.insert(key).second) {
      REXLOG_INFO("NATIVE-RT shadow map: depth base={} pitch={} {}x{}", ds.base_tile, ds.pitch,
                  ds.width, ds.height);
    }
    if (RenderTarget* rt = GetRenderTarget(ds)) {
      if (!t.color_count) {
        t.scale_x = rt->scale.x;
        t.scale_y = rt->scale.y;
      } else if (!(rt->scale == Scale{g_scale_x, g_scale_y})) {
        static bool reported = false;
        if (!reported) {
          reported = true;
          REXLOG_WARN("NATIVE-RT shadow map base={} used with color targets", ds.base_tile);
        }
      }
      t.depth = rt->texture->GetDefaultView(dl::TEXTURE_VIEW_DEPTH_STENCIL);
      t.depth_format = rt->format;
      if (!t.width) {
        t.width = rt->width;
        t.height = rt->height;
      }
    }
  }
  return t;
}

// Draws natively what the D3D call just submitted.
void SubmitDraw(uint32_t dev, uint32_t initiator, uint32_t dma_base, uint32_t dma_size,
                const InlineDraw* inline_draw, const uint8_t* base) {
  ++g_swap_draws;
  GuestRegisters state;
  CaptureDrawState(dev, base, state);
  uint32_t index = 0;
  for (const auto& range : kShadowRanges) {
    for (uint32_t i = 0; i < range.count; ++i)
      g_register_file->values[range.reg + i] = state[index++];
  }
  g_register_file->values[rex::graphics::XE_GPU_REG_VGT_DRAW_INITIATOR] = initiator;
  g_register_file->values[rex::graphics::XE_GPU_REG_VGT_DMA_BASE] = dma_base;
  g_register_file->values[rex::graphics::XE_GPU_REG_VGT_DMA_SIZE] = dma_size;
  {
    rex::graphics::reg::VGT_DRAW_INITIATOR init;
    init.value = initiator;
    bool indexed = init.source_select == rex::graphics::xenos::SourceSelect::kDMA;
    uint32_t index_bytes = init.index_size == rex::graphics::xenos::IndexFormat::kInt32 ? 4 : 2;
    g_last_index_base = indexed ? dma_base & ~(index_bytes - 1) : 0;
    g_last_index_count = indexed ? init.num_indices : 0;
    g_last_index_format = uint32_t(init.index_size);
  }
  if (inline_draw) {
    // Stream 0 = vertex fetch constant 95, written to the ring, not shadowed.
    uint32_t words = (inline_draw->count * inline_draw->stride + 3) / 4;
    uint32_t fetch = rex::graphics::XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0 + 95 * 2;
    g_register_file->values[fetch] = GpuAddress(inline_draw->data) | 3;
    g_register_file->values[fetch + 1] = (words << 2) | 2;  // k8in32
  }
  TranslatedShaders shaders;
  if (!g_shader_cache->Prepare(*g_register_file, VertexShaderCode(base, dev),
                               PixelShaderCode(base, dev), shaders)) {
    ++g_draws_without_shaders;
    return;
  }
  g_draw_renderer->Draw(*g_register_file, shaders, BoundTargets(base, dev), base, g_log_this_frame,
                        inline_draw ? 95u : ~0u);
}

// Instant replay: the last presented frames, downscaled, kept on the GPU; Home
// writes them with per-frame notes to frame_dump/replay_<frame>/.
constexpr uint32_t kReplayWidth = 640, kReplayHeight = 360;
constexpr char kReplayPS[] = R"(#version 450
layout(binding = 0) uniform texture2D g_Source;
layout(binding = 1) uniform sampler g_Linear;
layout(location = 0) out vec4 out_color;
void main() {
  out_color = texture(sampler2D(g_Source, g_Linear), gl_FragCoord.xy / vec2(640.0, 360.0));
}
)";

struct Replay {
  dl::RefCntAutoPtr<dl::IPipelineState> pso;
  dl::RefCntAutoPtr<dl::IShaderResourceBinding> srb;
  std::vector<dl::RefCntAutoPtr<dl::ITexture>> frames;
  std::vector<std::string> notes;
  std::vector<bool> has_image;
  uint32_t next = 0, count = 0;
  bool failed = false;
};
Replay g_replay;

bool EnsureReplay(uint32_t capacity) {
  Replay& r = g_replay;
  if (r.failed) return false;
  if (!r.pso) {
    r.failed = true;
    r.pso.Attach(
        g_device->CreateFullscreenPipeline("Replay", kReplayPS, dl::TEX_FORMAT_RGBA8_UNORM));
    if (!r.pso) return false;
    r.pso->CreateShaderResourceBinding(&r.srb, true);
    dl::SamplerDesc linear;
    linear.MinFilter = linear.MagFilter = linear.MipFilter = dl::FILTER_TYPE_LINEAR;
    dl::RefCntAutoPtr<dl::ISampler> sampler;
    g_device->RenderDevice()->CreateSampler(linear, &sampler);
    if (!sampler) return false;
    r.srb->GetVariableByName(dl::SHADER_TYPE_PIXEL, "g_Linear")->Set(sampler);
    r.failed = false;
  }
  if (r.frames.size() != capacity) {
    r.frames.assign(capacity, {});
    r.notes.assign(capacity, {});
    r.has_image.assign(capacity, false);
    r.next = r.count = 0;
  }
  return true;
}

// Keeps the presented image (view: what the swap chain shows) and its notes.
void ReplayCapture(dl::ITextureView* view, std::string note) {
  int32_t capacity = REXCVAR_GET(dante_replay_frames);
  if (capacity <= 0 || !EnsureReplay(uint32_t(capacity))) return;
  Replay& r = g_replay;
  uint32_t slot = r.next;
  r.next = (r.next + 1) % uint32_t(capacity);
  r.count = std::min(r.count + 1, uint32_t(capacity));
  r.notes[slot] = std::move(note);
  r.has_image[slot] = view != nullptr;
  if (!view) return;
  auto& texture = r.frames[slot];
  if (!texture) {
    dl::TextureDesc desc;
    desc.Name = "ReplayFrame";
    desc.Type = dl::RESOURCE_DIM_TEX_2D;
    desc.Width = kReplayWidth;
    desc.Height = kReplayHeight;
    desc.Format = dl::TEX_FORMAT_RGBA8_UNORM;
    desc.BindFlags = dl::BIND_RENDER_TARGET | dl::BIND_SHADER_RESOURCE;
    g_device->RenderDevice()->CreateTexture(desc, nullptr, &texture);
    if (!texture) {
      r.has_image[slot] = false;
      return;
    }
  }
  auto* context = g_device->Context();
  dl::ITextureView* rtv = texture->GetDefaultView(dl::TEXTURE_VIEW_RENDER_TARGET);
  context->SetRenderTargets(1, &rtv, nullptr, dl::RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
  dl::Viewport vp;
  vp.Width = float(kReplayWidth);
  vp.Height = float(kReplayHeight);
  context->SetViewports(1, &vp, kReplayWidth, kReplayHeight);
  r.srb->GetVariableByName(dl::SHADER_TYPE_PIXEL, "g_Source")->Set(view);
  context->SetPipelineState(r.pso);
  context->CommitShaderResources(r.srb, dl::RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
  dl::DrawAttribs draw;
  draw.NumVertices = 3;
  context->Draw(draw);
  context->SetRenderTargets(0, nullptr, nullptr, dl::RESOURCE_STATE_TRANSITION_MODE_NONE);
}

void ReplayDump() {
  Replay& r = g_replay;
  if (!r.count) {
    REXLOG_WARN("NATIVE-REPLAY: nothing recorded (dante_replay_frames=0?)");
    return;
  }
  auto dir = FrameDumpDirectory() / fmt::format("replay_f{}", g_frame);
  std::error_code ec;
  std::filesystem::create_directories(dir, ec);
  std::string index;
  uint32_t capacity = uint32_t(r.frames.size());
  uint32_t first = (r.next + capacity - r.count) % capacity;
  for (uint32_t i = 0; i < r.count; ++i) {
    uint32_t slot = (first + i) % capacity;
    index += fmt::format("{:03} {}\n", i, r.notes[slot]);
    if (r.has_image[slot] && r.frames[slot]) {
      DumpTexturePng(g_device->RenderDevice(), g_device->Context(), r.frames[slot],
                     dir / fmt::format("{:03}.png", i));
    }
  }
  if (std::FILE* f = std::fopen((dir / "index.txt").string().c_str(), "wb")) {
    std::fwrite(index.data(), 1, index.size(), f);
    std::fclose(f);
  }
  REXLOG_INFO("NATIVE-REPLAY: {} frames written to {}", r.count, dir.string());
}

// Performance log: a summary every 30 s and a line per slow frame, with the
// shaders and pipelines compiled in it (stutter vs GPU load).
constexpr double kPerfWindowSeconds = 30.0;
constexpr float kSlowFrameMs = 50.0f;
constexpr uint32_t kSlowFramesLoggedPerWindow = 20;

struct PerfWindow {
  std::chrono::steady_clock::time_point start{}, last_swap{};
  std::vector<float> frame_ms;
  uint32_t slow_frames = 0;
  RenderStats at_start{}, at_last_swap{};
};
PerfWindow g_perf;

void PerfOnSwap(uint32_t draws, uint32_t resolves) {
  using Clock = std::chrono::steady_clock;
  PerfWindow& p = g_perf;
  const RenderStats& stats = g_render_stats;
  Clock::time_point now = Clock::now();
  if (p.start == Clock::time_point{}) {
    p.start = p.last_swap = now;
    p.at_start = p.at_last_swap = stats;
    return;
  }
  float ms = std::chrono::duration<float, std::milli>(now - p.last_swap).count();
  p.last_swap = now;
  p.frame_ms.push_back(ms);
  uint64_t shaders = stats.shaders_compiled - p.at_last_swap.shaders_compiled;
  uint64_t pipelines = stats.pipelines_created - p.at_last_swap.pipelines_created;
  p.at_last_swap = stats;
  if (ms > kSlowFrameMs && ++p.slow_frames <= kSlowFramesLoggedPerWindow) {
    REXLOG_INFO(
        "PERF slow frame f{}: {:.1f} ms, {} shaders and {} pipelines compiled, {} draws, "
        "{} resolves",
        g_frame, ms, shaders, pipelines, draws, resolves);
  }
  double seconds = std::chrono::duration<double>(now - p.start).count();
  if (seconds < kPerfWindowSeconds) return;
  std::vector<float> sorted = p.frame_ms;
  std::sort(sorted.begin(), sorted.end(), std::greater<float>());
  // 1% low: the average frame rate of the slowest 1% of frames.
  size_t slowest = std::max<size_t>(1, sorted.size() / 100);
  double slowest_ms = 0.0;
  for (size_t i = 0; i < slowest; ++i) slowest_ms += sorted[i];
  slowest_ms /= double(slowest);
  REXLOG_INFO(
      "PERF last {:.0f} s: {} frames, avg {:.1f} fps, 1% low {:.1f} fps, worst {:.1f} ms, "
      "{} frames over {:.0f} ms; compiled {} shaders, {} pipelines (total {}, {})",
      seconds, sorted.size(), sorted.size() / seconds, 1000.0 / slowest_ms, sorted.front(),
      p.slow_frames, kSlowFrameMs, stats.shaders_compiled - p.at_start.shaders_compiled,
      stats.pipelines_created - p.at_start.pipelines_created, stats.shaders_compiled,
      stats.pipelines_created);
  p.start = now;
  p.frame_ms.clear();
  p.slow_frames = 0;
  p.at_start = stats;
}

// Views of front buffer textures with the front buffer's swizzle.
std::unordered_map<uint64_t, dl::RefCntAutoPtr<dl::ITextureView>> g_front_views;

dl::ITextureView* FrontBufferView(dl::ITexture* texture, uint32_t swizzle) {
  uint64_t key = uint64_t(reinterpret_cast<uintptr_t>(texture)) ^ (uint64_t(swizzle) << 52);
  auto it = g_front_views.find(key);
  if (it != g_front_views.end()) return it->second;
  dl::TextureViewDesc desc;
  desc.ViewType = dl::TEXTURE_VIEW_SHADER_RESOURCE;
  desc.TextureDim = dl::RESOURCE_DIM_TEX_2D;
  desc.Swizzle = ComponentMapping(swizzle);
  dl::RefCntAutoPtr<dl::ITextureView> view;
  texture->CreateView(desc, &view);
  g_front_views.emplace(key, view);
  return view ? view.RawPtr() : texture->GetDefaultView(dl::TEXTURE_VIEW_SHADER_RESOURCE);
}

void PresentTestPattern() {
  float phase = float(g_frame % 240) / 240.0f * 6.2831853f;
  g_device->BeginFrame();
  g_device->Clear(0.3f + 0.3f * std::sin(phase), 0.3f + 0.3f * std::sin(phase + 2.0943951f),
                  0.3f + 0.3f * std::sin(phase + 4.1887902f), 1.0f);
  g_device->Present(0);
}

}  // namespace

bool Start(rex::ui::Window* window) {
  std::lock_guard lock(g_mutex);
  if (g_device) return true;
  void* hwnd = window ? window->GetNativeWindowHandle() : nullptr;
  if (!hwnd) {
    REXLOG_ERROR("DanteDevice: no game window");
    return false;
  }
  window->SetPresenter(nullptr);
  auto device = std::make_unique<VulkanDevice>();
  if (!device->Initialize(hwnd, 1280, 720)) {
    REXLOG_ERROR("DanteDevice: native device initialization failed");
    return false;
  }
  const dl::GraphicsAdapterInfo& adapter = device->RenderDevice()->GetAdapterInfo();
  const dl::RenderDeviceInfo& device_info = device->RenderDevice()->GetDeviceInfo();
  REXLOG_INFO("DanteDevice: GPU {} (vendor {:04X} device {:04X}, {} MB VRAM), Vulkan {}.{}",
              adapter.Description, adapter.VendorId, adapter.DeviceId,
              // CPU-visible VRAM (Resizable BAR) is reported as unified memory.
              (adapter.Memory.LocalMemory + adapter.Memory.UnifiedMemory) >> 20,
              device_info.APIVersion.Major, device_info.APIVersion.Minor);
  RenderScale scale = ComputeRenderScale(window);
  device->SetDisplayAspect(scale.display_aspect, true);
  g_device = std::move(device);
  g_scale_x = scale.x;
  g_scale_y = scale.y;
  g_shadow_map_factor = float(std::clamp(REXCVAR_GET(dante_shadow_scale), 1, 4));
  constexpr dl::BIND_FLAGS kDepthUse = dl::BIND_DEPTH_STENCIL | dl::BIND_SHADER_RESOURCE;
  const auto& d24 =
      g_device->RenderDevice()->GetTextureFormatInfoExt(dl::TEX_FORMAT_D24_UNORM_S8_UINT);
  g_use_d24_depth = (d24.BindFlags & kDepthUse) == kDepthUse && !REXCVAR_GET(dante_depth_float);
  REXLOG_INFO("DanteDevice: depth buffers {}",
              g_use_d24_depth ? "D24S8" : "D32S8 (no 24-bit depth on this GPU or forced)");
  Scale target_scale{g_scale_x, g_scale_y};
  REXLOG_INFO("DanteDevice: resolution scale {:.3f} x {:.3f} ({}x{}, display aspect {:.4f})",
              g_scale_x, g_scale_y, target_scale.HostX(1280), target_scale.HostY(720),
              scale.display_aspect);
  g_shader_cache = std::make_unique<ShaderCache>(g_device->RenderDevice(), g_scale_x, g_scale_y);
  g_texture_cache = std::make_unique<TextureCache>(
      g_device->RenderDevice(), g_device->Context(),
      [](uint32_t address, uint32_t width, uint32_t height, uint32_t format,
         bool& rb_swap) -> dl::ITexture* {
        auto it = g_resolve_targets.find(ResolveTargetKey{address, width, height, format});
        if (it != g_resolve_targets.end()) {
          rb_swap = it->second.rb_swap;
          return it->second.texture.RawPtr();
        }
        // k_8_8_8_8 over a depth resolve (k_24_8): its raw words, from the
        // next resolve on.
        if (format != uint32_t(rex::graphics::xenos::TextureFormat::k_8_8_8_8)) return nullptr;
        it = g_resolve_targets.find(ResolveTargetKey{
            address, width, height, uint32_t(rex::graphics::xenos::TextureFormat::k_24_8)});
        if (it == g_resolve_targets.end()) return nullptr;
        it->second.depth_bits_wanted = true;
        rb_swap = false;
        return it->second.depth_bits.RawPtr();
      });
  g_draw_renderer = std::make_unique<DrawRenderer>(g_device->RenderDevice(), g_device->Context(),
                                                   g_texture_cache.get());
  g_smaa = std::make_unique<SmaaPass>(g_device.get());
  g_button_glyphs = std::make_unique<ButtonGlyphs>(g_device->RenderDevice());
  g_texture_cache->SetReplacer([](uint64_t hash, const TextureCache::LoadedTexture* loaded) {
    return g_button_glyphs ? g_button_glyphs->Replace(hash, loaded) : nullptr;
  });
  g_register_file = std::make_unique<rex::graphics::RegisterFile>();
  g_frame = 0;
  REXLOG_INFO("DanteDevice: presenting on the game window (SDK presenter detached)");
  return true;
}

void Stop() {
  std::lock_guard lock(g_mutex);
  if (!g_device) return;
  g_render_targets.clear();
  g_shadow_maps.clear();
  g_resolve_targets.clear();
  g_readback_fence.Release();
  g_readback_value = 0;
  g_replay = {};
  g_depth_copy = {};
  g_depth_bits = {};
  g_front_views.clear();
  g_loaded_vs = {};
  g_last_ps = {};
  g_smaa.reset();
  g_draw_renderer.reset();
  g_button_glyphs.reset();
  g_texture_cache.reset();
  g_shader_cache.reset();
  g_device->Shutdown();
  g_device.reset();
  REXLOG_INFO("DanteDevice: stopped after {} frames", g_frame);
}

void LastIndexBuffer(uint32_t& base, uint32_t& count, uint32_t& format) {
  base = g_last_index_base;
  count = g_last_index_count;
  format = g_last_index_format;
}

void BoundShaderHashes(uint32_t dev, const uint8_t* base, uint64_t& vs, uint64_t& ps) {
  GuestShaderCode v = VertexShaderCode(base, dev), p = PixelShaderCode(base, dev);
  vs = v.ucode ? XXH3_64bits(v.ucode, v.dwords * 4) : 0;
  ps = p.ucode ? XXH3_64bits(p.ucode, p.dwords * 4) : 0;
}

// Clear(count, pRects, flags, color, Z, stencil): flags TARGET0..3 = bits 0-3,
// ZBUFFER = 0x10, STENCIL = 0x20; color is D3DCOLOR (ARGB).
void AfterClear(const CallArgs& a, const uint8_t* base) {
  std::lock_guard lock(g_mutex);
  if (!g_device) return;
  FlushDrawLog();
  uint32_t dev = a.r[0];
  uint32_t flags = a.r[3];
  uint32_t c = a.r[4];
  const float color[4] = {float((c >> 16) & 0xFF) / 255.0f, float((c >> 8) & 0xFF) / 255.0f,
                          float(c & 0xFF) / 255.0f, float(c >> 24) / 255.0f};
  for (uint32_t i = 0; i < 4; ++i) {
    if (!(flags & (1u << i))) continue;
    SurfaceDesc s;
    if (!ReadSurface(base, Load32(base, dev + kDevRenderTargets + i * 4), false, s)) continue;
    if (RenderTarget* rt = GetRenderTarget(s)) ClearColor(*rt, color);
    if (g_log_this_frame) {
      REXLOG_INFO("NATIVE-RT f{} clear RT{} base={} pitch={} fmt={} color={:08X}", g_frame, i,
                  s.base_tile, s.pitch, s.format, c);
    }
  }
  if (flags & 0x30) {
    SurfaceDesc s;
    if (ReadSurface(base, Load32(base, dev + kDevDepthStencil), true, s)) {
      if (RenderTarget* rt = GetRenderTarget(s)) {
        ClearDepth(*rt, flags & 0x10, flags & 0x20, float(a.f1), uint8_t(a.r[5]));
      }
      if (g_log_this_frame) {
        REXLOG_INFO("NATIVE-RT f{} clear depth base={} pitch={} z={:.3f} stencil={}", g_frame,
                    s.base_tile, s.pitch, a.f1, a.r[5] & 0xFF);
      }
    }
  }
}

// Resolve(flags, pSourceRect, pDestTexture, pDestPoint, level, slice,
// pClearColor, ClearZ, ...): flags & 7 = source (RT0-3, 4 = depth),
// 0x100 / 0x200 = clear color / depth-stencil afterwards.
void AfterResolve(const CallArgs& a, const uint8_t* base) {
  std::lock_guard lock(g_mutex);
  if (!g_device) return;
  FlushDrawLog();
  ++g_swap_resolves;
  uint32_t dev = a.r[0];
  uint32_t flags = a.r[1];
  uint32_t source = flags & 7;
  bool from_depth = source == 4;
  SurfaceDesc s;
  uint32_t surface =
      Load32(base, dev + (from_depth ? kDevDepthStencil : kDevRenderTargets + source * 4));
  if (!ReadSurface(base, surface, from_depth, s)) return;
  RenderTarget* rt = GetRenderTarget(s);
  if (!rt) return;

  uint32_t x1 = 0, y1 = 0, x2 = s.width, y2 = s.height;
  if (IsPlausibleAddress(a.r[2])) {
    x1 = Load32(base, a.r[2]);
    y1 = Load32(base, a.r[2] + 4);
    x2 = Load32(base, a.r[2] + 8);
    y2 = Load32(base, a.r[2] + 12);
  }
  uint32_t dst_x = 0, dst_y = 0;
  if (IsPlausibleAddress(a.r[4])) {
    dst_x = Load32(base, a.r[4]);
    dst_y = Load32(base, a.r[4] + 4);
  }

  uint32_t dest = a.r[3];
  if (IsPlausibleAddress(dest)) {
    // Fetch constant at +0x1C: dword1 = base | format, dword2 = size.
    uint32_t dword1 = Load32(base, dest + 0x20);
    uint32_t dword2 = Load32(base, dest + 0x24);
    // Texture objects hold the guest virtual address; fetch constants (what
    // sampling sees) hold the physical one.
    uint32_t address = GpuAddress(dword1 & 0xFFFFF000u);
    uint32_t guest_format = dword1 & 0x3F;
    uint32_t width = (dword2 & 0x1FFF) + 1;
    uint32_t height = ((dword2 >> 13) & 0x1FFF) + 1;
    uint32_t volume_depth = 0, slice = 0;
    rex::graphics::xenos::xe_gpu_texture_fetch_t dest_fetch;
    uint32_t dest_dwords[6];
    for (uint32_t i = 0; i < 6; ++i) dest_dwords[i] = Load32(base, dest + 0x1C + i * 4);
    std::memcpy(&dest_fetch, dest_dwords, sizeof(dest_fetch));
    if (dest_fetch.dimension == rex::graphics::xenos::DataDimension::k3D) {
      width = dest_fetch.size_3d.width + 1;
      height = dest_fetch.size_3d.height + 1;
      volume_depth = dest_fetch.size_3d.depth + 1;
      slice = std::min(a.r[6], volume_depth - 1);
    }
    ResolveTarget* t = GetResolveTarget(address, width, height, guest_format,
                                        volume_depth ? slice + 1 : 0, rt->scale);
    if (t) {
      t->volume_depth = volume_depth;
      t->slice = slice;
    }
    // RB_COPY_DEST_INFO from the Resolve's packets, which may span several
    // command buffer segments.
    uint32_t dest_info = ~0u;
    uint32_t span_start = a.write_ptr_before;
    uint32_t switches = g_segment_switch_count - a.segment_switches_before;
    if (switches <= kSegmentSwitchHistory) {
      for (uint32_t i = a.segment_switches_before; i != g_segment_switch_count; ++i) {
        const SegmentSwitch& sw = g_segment_switches[i % kSegmentSwitchHistory];
        uint32_t v = FindRegisterWrite(base, span_start, sw.old_ptr, 0x231B);
        if (v != ~0u) dest_info = v;
        span_start = sw.new_ptr;
      }
      uint32_t v = FindRegisterWrite(base, span_start,
                                     Load32(base, dev + kDevCommandBufferWritePtr), 0x231B);
      if (v != ~0u) dest_info = v;
    }
    // Not found (more switches than the history keeps): keep the target's
    // previous setting; the game resolves each target the same way.
    bool rb_swap = dest_info != ~0u ? bool((dest_info >> 24) & 1) : t && t->rb_swap;
    if (dest_info == ~0u) ++g_swap_dest_info_missing;
    const char* result = "skipped";
    if (t && !from_depth && t->format == rt->format) {
      x2 = std::min({x2, rt->width, x1 + (t->width - std::min(dst_x, t->width))});
      y2 = std::min({y2, rt->height, y1 + (t->height - std::min(dst_y, t->height))});
      if (x2 > x1 && y2 > y1) {
        dl::Box box(rt->scale.HostX(x1), rt->scale.HostX(x2), rt->scale.HostY(y1),
                    rt->scale.HostY(y2));
        dl::CopyTextureAttribs copy(rt->texture, dl::RESOURCE_STATE_TRANSITION_MODE_TRANSITION,
                                    t->texture, dl::RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
        copy.pSrcBox = &box;
        copy.DstX = t->scale.HostX(dst_x);
        copy.DstY = t->scale.HostY(dst_y);
        // The source is usually still bound as the render target.
        g_device->Context()->SetRenderTargets(0, nullptr, nullptr,
                                              dl::RESOURCE_STATE_TRANSITION_MODE_NONE);
        g_device->Context()->CopyTexture(copy);
        t->rb_swap = rb_swap;
        result = rb_swap ? "copied(rb swap)" : "copied";
        QueueReadback(*t, base, dest);
      }
    } else if (t && from_depth && t->format == dl::TEX_FORMAT_R32_FLOAT) {
      x2 = std::min({x2, rt->width, x1 + (t->width - std::min(dst_x, t->width))});
      y2 = std::min({y2, rt->height, y1 + (t->height - std::min(dst_y, t->height))});
      if (x2 > x1 && y2 > y1) {
        CopyDepth(*rt, t->texture, rt->scale.HostX(x1), rt->scale.HostY(y1), rt->scale.HostX(x2),
                  rt->scale.HostY(y2), t->scale.HostX(dst_x), t->scale.HostY(dst_y));
        result = "depth-copied";
        if (t->depth_bits_wanted && !rt->guest_float_depth) {
          const auto& desc = t->texture->GetDesc();
          if (!t->depth_bits) {
            dl::TextureDesc bits;
            bits.Name = "ResolveDepthBits";
            bits.Type = dl::RESOURCE_DIM_TEX_2D;
            bits.Width = desc.Width;
            bits.Height = desc.Height;
            bits.Format = dl::TEX_FORMAT_RGBA8_UNORM;
            bits.BindFlags = dl::BIND_SHADER_RESOURCE | dl::BIND_RENDER_TARGET;
            g_device->RenderDevice()->CreateTexture(bits, nullptr, &t->depth_bits);
            if (t->depth_bits) ClearNewTexture(t->depth_bits, false);
          }
          if (t->depth_bits) {
            CopyDepthBits(*rt, t->depth_bits, rt->scale.HostX(x1), rt->scale.HostY(y1),
                          rt->scale.HostX(x2), rt->scale.HostY(y2), t->scale.HostX(dst_x),
                          t->scale.HostY(dst_y));
          }
        }
      }
    } else if (t) {
      // Other format conversions need a shader copy.
      uint64_t pair = (uint64_t(from_depth) << 40) | (uint64_t(s.format) << 32) | guest_format;
      if (g_reported_conversions.insert(pair).second) {
        REXLOG_INFO("NATIVE-RT resolve needs conversion: {} fmt={} -> guest fmt={}",
                    from_depth ? "depth" : "color", s.format, guest_format);
      }
    }
    if (g_dump_this_frame && t) {
      DumpTexturePng(
          g_device->RenderDevice(), g_device->Context(), t->texture,
          FrameDumpDirectory() / fmt::format("f{}_{:02}_{}_{:08X}_{}x{}_g{}.png", g_frame,
                                             ++g_dump_seq, from_depth ? "depth" : "color", address,
                                             width, height, guest_format));
    }
    if (g_log_this_frame) {
      REXLOG_INFO(
          "NATIVE-RT f{} resolve {} base={} pitch={} fmt={} rect=({},{})-({},{}) -> {:08X} "
          "{}x{} fmt={} at ({},{}) {} dest_info={:08X}",
          g_frame, from_depth ? "depth" : "color", s.base_tile, s.pitch, s.format, x1, y1, x2, y2,
          address, width, height, guest_format, dst_x, dst_y, result, dest_info);
    }
  }

  if ((flags & 0x100) && !from_depth && IsPlausibleAddress(a.r[7])) {
    float color[4];
    for (uint32_t i = 0; i < 4; ++i) {
      uint32_t bits = Load32(base, a.r[7] + i * 4);
      std::memcpy(&color[i], &bits, 4);
    }
    ClearColor(*rt, color);
  }
  if (flags & 0x200) {
    SurfaceDesc ds;
    if (ReadSurface(base, Load32(base, dev + kDevDepthStencil), true, ds)) {
      if (RenderTarget* depth_rt = GetRenderTarget(ds))
        ClearDepth(*depth_rt, true, true, float(a.f1), 0);
    }
  }
}

void BeforeDraw(uint32_t device, const uint8_t* base) {
  if (!g_log_this_frame) return;
  std::lock_guard lock(g_mutex);
  SurfaceDesc s;
  ReadSurface(base, Load32(base, device + kDevRenderTargets), false, s);
  RenderTargetKey key{s.base_tile, s.pitch, s.format, false};
  if (!(key == g_log_draw_target)) FlushDrawLog();
  g_log_draw_target = key;
  ++g_logged_run_draws;
}

void AfterDraw(const CallArgs& a, DrawKind kind, const uint8_t* base) {
  std::lock_guard lock(g_mutex);
  if (!g_device || !g_shader_cache || !g_draw_renderer) return;
  if (kind == DrawKind::kInline) return;  // drawn at EndVertices
  uint32_t dev = a.r[0];
  if (kind == DrawKind::kInlineUP) {
    // DrawVerticesUP(prim, count, pData, stride): its nested BeginVertices
    // provided the ring copy of the data.
    if (!g_pending_inline_draw.pending) return;
    g_pending_inline_draw.pending = false;
    SubmitDraw(dev, InlineDrawInitiator(g_pending_inline_draw), 0, 0, &g_pending_inline_draw, base);
    return;
  }
  uint32_t initiator, dma_base, dma_size;
  if (!ParseDrawPacket(base, dev, initiator, dma_base, dma_size)) {
    ++g_draws_unparsed;
    return;
  }
  SubmitDraw(dev, initiator, dma_base, dma_size, nullptr, base);
}

void AfterBeginVertices(const CallArgs& a, uint32_t data) {
  std::lock_guard lock(g_mutex);
  g_pending_inline_draw.pending = true;
  g_pending_inline_draw.args = a;
  g_pending_inline_draw.prim = a.r[1];
  g_pending_inline_draw.count = a.r[2];
  g_pending_inline_draw.stride = a.r[3];
  g_pending_inline_draw.data = data;
}

void AfterEndVertices(const uint8_t* base) {
  std::lock_guard lock(g_mutex);
  if (!g_device || !g_draw_renderer || !g_pending_inline_draw.pending) return;
  g_pending_inline_draw.pending = false;
  SubmitDraw(g_pending_inline_draw.args.r[0], InlineDrawInitiator(g_pending_inline_draw), 0, 0,
             &g_pending_inline_draw, base);
}

void SetButtonGlyphStyleSource(std::function<GlyphStyle()> source) {
  std::lock_guard lock(g_mutex);
  g_glyph_style_source = std::move(source);
}

void SetButtonGlyphBindingsSource(std::function<KeyBindings()> source) {
  std::lock_guard lock(g_mutex);
  g_glyph_bindings_source = std::move(source);
}

// Swap(pFrontBuffer, ...): present the native texture resolved to the front buffer.
void AfterSwap(const CallArgs& a, const uint8_t* base, bool log_next_frame) {
  std::lock_guard lock(g_mutex);
  if (!g_device) return;
  FlushDrawLog();
  FinishReadbacks(base);
  g_logged_run_draws = 0;

  uint32_t front = a.r[1];
  uint32_t address = 0;
  ResolveTargetKey key{};
  if (IsPlausibleAddress(front)) {
    uint32_t dword1 = Load32(base, front + 0x20);
    uint32_t dword2 = Load32(base, front + 0x24);
    address = GpuAddress(dword1 & 0xFFFFF000u);
    key = {address, (dword2 & 0x1FFF) + 1, ((dword2 >> 13) & 0x1FFF) + 1, dword1 & 0x3F};
  }
  auto it = g_resolve_targets.find(key);
  bool found = it != g_resolve_targets.end() && it->second.texture;

  // Debug view: present a color host RT instead of the front buffer.
  dl::ITextureView* debug_view = nullptr;
  if (int32_t view = REXCVAR_GET(dante_debug_view); view > 0) {
    std::vector<std::pair<RenderTargetKey, RenderTarget*>> colors;
    for (auto& [rt_key, rt] : g_render_targets) {
      if (!rt_key.depth && rt.texture) colors.push_back({rt_key, &rt});
    }
    std::sort(colors.begin(), colors.end(), [](const auto& a, const auto& b) {
      return std::tie(a.first.base_tile, a.first.pitch, a.first.format) <
             std::tie(b.first.base_tile, b.first.pitch, b.first.format);
    });
    if (!colors.empty()) {
      auto& [rt_key, rt] = colors[(view - 1) % colors.size()];
      debug_view = rt->texture->GetDefaultView(dl::TEXTURE_VIEW_SHADER_RESOURCE);
      static int32_t logged_view = -1;
      if (logged_view != view) {
        logged_view = view;
        REXLOG_INFO("NATIVE-RT debug view {}: color RT base={} pitch={} fmt={} {}x{}", view,
                    rt_key.base_tile, rt_key.pitch, rt_key.format, rt->width, rt->height);
      }
    }
  }
  if (g_log_this_frame) {
    REXLOG_INFO("NATIVE-RT f{} swap front={:08X} {}", g_frame, address,
                found ? "presented" : "missing (test pattern)");
  }
  if (int32_t record = REXCVAR_GET(dante_record_frames); record > 0) {
    if (found) {
      auto dir = FrameDumpDirectory() / "record";
      std::error_code ec;
      std::filesystem::create_directories(dir, ec);
      DumpTexturePng(g_device->RenderDevice(), g_device->Context(), it->second.texture,
                     dir / fmt::format("f{:06}.png", g_frame));
    }
    REXLOG_INFO(
        "NATIVE-REC f{} front={:08X} {}x{} fmt={} rb_swap={} draws={} resolves={} "
        "dest_info_missing={}{}",
        g_frame, address, key.width, key.height, key.guest_format, found && it->second.rb_swap,
        g_swap_draws, g_swap_resolves, g_swap_dest_info_missing,
        found ? "" : " (front buffer missing)");
    rex::cvar::SetFlagByName("dante_record_frames", std::to_string(record - 1));
  }
  // Anti-alias the finished frame (3D and UI) before it is presented.
  if (found && g_smaa && REXCVAR_GET(dante_smaa)) {
    if (g_dump_this_frame) {
      DumpTexturePng(g_device->RenderDevice(), g_device->Context(), it->second.texture,
                     FrameDumpDirectory() / fmt::format("f{}_98_front_before_smaa.png", g_frame));
    }
    g_smaa->Apply(it->second.texture);
    if (g_dump_this_frame && g_smaa->Edges()) {
      DumpTexturePng(g_device->RenderDevice(), g_device->Context(), g_smaa->Edges(),
                     FrameDumpDirectory() / fmt::format("f{}_98_smaa_edges.png", g_frame));
    }
  }
  uint32_t front_swizzle = 0;
  dl::ITextureView* front_view = nullptr;
  if (found) {
    // The front buffer's fetch swizzle (dword 3 bits 1-12) maps the stored
    // channels to RGB, e.g. ZYX1 for this game's BGR-ordered front buffer.
    front_swizzle = (Load32(base, front + 0x28) >> 1) & 0xFFF;
    if (it->second.rb_swap) front_swizzle = SwapRedBlueSwizzle(front_swizzle);
    front_view = FrontBufferView(it->second.texture, front_swizzle);
  }
  ReplayCapture(debug_view ? debug_view : front_view,
                fmt::format("f{} front={:08X} {}x{} fmt={} rb_swap={} swizzle={:03X} draws={} "
                            "resolves={} dest_info_missing={} new_targets={}{}",
                            g_frame, address, key.width, key.height, key.guest_format,
                            found && it->second.rb_swap, front_swizzle, g_swap_draws,
                            g_swap_resolves, g_swap_dest_info_missing, g_swap_new_targets,
                            found ? "" : " (front buffer missing)"));
  if (REXCVAR_GET(dante_replay_dump)) {
    rex::cvar::SetFlagByName("dante_replay_dump", "false");
    ReplayDump();
  }
  PerfOnSwap(g_swap_draws, g_swap_resolves);
  if (g_button_glyphs && g_glyph_style_source) {
    GlyphStyle style = g_glyph_style_source();
    if (style == GlyphStyle::kKeyboard && g_glyph_bindings_source) {
      g_button_glyphs->SetBindings(g_glyph_bindings_source());
    }
    g_button_glyphs->SetStyle(style);
  }
  g_swap_draws = g_swap_resolves = g_swap_dest_info_missing = g_swap_new_targets = 0;
  if (g_dump_this_frame && found) {
    DumpTexturePng(g_device->RenderDevice(), g_device->Context(), it->second.texture,
                   FrameDumpDirectory() / fmt::format("f{}_99_front.png", g_frame));
  }
  ++g_frame;
  g_dump_this_frame = log_next_frame && !g_log_this_frame;
  g_dump_seq = 0;
  g_log_this_frame = log_next_frame;
  if (g_draw_renderer) g_draw_renderer->SetFrame(g_frame);
  if (g_shader_cache && g_draw_renderer && g_frame % 300 == 0) {
    g_shader_cache->LogStats();
    g_draw_renderer->LogStats();
    g_texture_cache->LogStats();
    REXLOG_INFO("NATIVE-DRAW draws_without_shaders={} unparsed={}", g_draws_without_shaders,
                g_draws_unparsed);
  }
  if (!g_device->UpdateWindowSize()) return;  // minimized
  if (debug_view) {
    g_device->PresentTexture(debug_view, 0);
  } else if (front_view) {
    g_device->PresentTexture(front_view, 0);
  } else {
    PresentTestPattern();
  }
}

}  // namespace renderer
