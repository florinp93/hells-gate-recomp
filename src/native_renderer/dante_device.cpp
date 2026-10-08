#include "dante_device.h"

#include "dante_draws.h"
#include "dante_dump.h"
#include "dante_shaders.h"
#include "dante_textures.h"
#include "gpu_state.h"
#include "native_device.h"

#include "Common/interface/RefCntAutoPtr.hpp"
#include "Graphics/GraphicsEngine/interface/DeviceContext.h"
#include "Graphics/GraphicsEngine/interface/Buffer.h"
#include "Graphics/GraphicsEngine/interface/PipelineState.h"
#include "Graphics/GraphicsEngine/interface/Sampler.h"
#include "Graphics/GraphicsEngine/interface/ShaderResourceBinding.h"
#include "Graphics/GraphicsTools/interface/MapHelper.hpp"
#include "Graphics/GraphicsEngine/interface/RenderDevice.h"
#include "Graphics/GraphicsEngine/interface/Texture.h"
#include "Graphics/GraphicsEngine/interface/TextureView.h"

#include <rex/cvar.h>
#include <xxhash.h>
#include <rex/graphics/register_file.h>
#include <rex/graphics/registers.h>
#include <rex/logging/macros.h>
#include <rex/ui/window.h>

#include <algorithm>
#include <cmath>
#include <cstdint>

#include <fmt/format.h>
#include <cstring>
#include <memory>
#include <mutex>
#include <set>
#include <tuple>
#include <unordered_map>
#include <utility>
#include <vector>

REXCVAR_DEFINE_INT32(dante_debug_view, 0, "Diagnostics",
                     "renderer=dante: 0 presents the front buffer, N presents the N-th "
                     "color host render target (F11 cycles)")
    .range(0, 64);

namespace native {
namespace {

using namespace native::gpu;
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
struct RtKey {
  uint32_t base_tile;
  uint32_t pitch;
  uint32_t format;
  bool depth;
  bool operator==(const RtKey&) const = default;
};
struct RtKeyHash {
  size_t operator()(const RtKey& k) const {
    return (size_t(k.base_tile) << 32) ^ (size_t(k.pitch) << 16) ^ (k.format << 1) ^ k.depth;
  }
};

struct HostRt {
  uint32_t width = 0;
  uint32_t height = 0;
  dl::TEXTURE_FORMAT format = dl::TEX_FORMAT_UNKNOWN;
  dl::RefCntAutoPtr<dl::ITexture> texture;
};

// Native stand-in for a guest texture written by Resolve; no copy goes back to
// guest memory. The game resolves different sizes/formats to the same address
// within a frame, so the key is the full texture description.
struct GuestTextureKey {
  uint32_t address;
  uint32_t width;
  uint32_t height;
  uint32_t guest_format;
  bool operator==(const GuestTextureKey&) const = default;
};
struct GuestTextureKeyHash {
  size_t operator()(const GuestTextureKey& k) const {
    return (size_t(k.address) << 24) ^ (size_t(k.width) << 12) ^ k.height ^ (size_t(k.guest_format) << 40);
  }
};
struct GuestTexture {
  uint32_t width = 0;
  uint32_t height = 0;
  uint32_t guest_format = 0;
  dl::TEXTURE_FORMAT format = dl::TEX_FORMAT_UNKNOWN;
  dl::RefCntAutoPtr<dl::ITexture> texture;
  // Last resolve had RB_COPY_DEST_INFO.copy_dest_swap: guest memory holds
  // red and blue exchanged relative to the host texture.
  bool rb_swap = false;
};

// Swaps the red and blue channel selectors of a 12-bit host swizzle.
uint32_t SwapRedBlue(uint32_t swizzle) {
  uint32_t out = 0;
  for (uint32_t i = 0; i < 4; ++i) {
    uint32_t c = (swizzle >> (3 * i)) & 7;
    c = c == 0 ? 2 : c == 2 ? 0 : c;
    out |= c << (3 * i);
  }
  return out;
}

// Last non-zero value written to a register in (start, end], or ~0u (Resolve
// resets the copy registers to 0 after the copy).
uint32_t FindRegisterWrite(const uint8_t* base, uint32_t start, uint32_t end, uint32_t reg) {
  uint32_t value = ~0u;
  if (!Plausible(start) || !Plausible(end) || end <= start || end - start > 0x40000) return value;
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
std::unique_ptr<dante::NativeDevice> g_device;
uint64_t g_frame = 0;
bool g_log_frame = false;
// First frame of an F8 capture: every resolve output and the front buffer are
// written to FrameDumpDirectory() as PNGs.
bool g_dump_frame = false;
uint32_t g_dump_seq = 0;
uint32_t g_frame_draws = 0;
RtKey g_draw_key{};
std::unordered_map<RtKey, HostRt, RtKeyHash> g_rts;
std::unordered_map<GuestTextureKey, GuestTexture, GuestTextureKeyHash> g_textures;
std::set<uint64_t> g_reported_conversions;
std::unique_ptr<ShaderCache> g_shaders;
std::unique_ptr<TextureCache> g_texture_cache;
std::unique_ptr<DrawRenderer> g_draws;
std::unique_ptr<rex::graphics::RegisterFile> g_regs;
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
InlineDraw g_inline;

bool Is64bpp(uint32_t color_format) {
  return color_format == 5 || color_format == 7 || color_format == 15;
}

dl::TEXTURE_FORMAT ColorFormat(uint32_t format) {
  switch (format) {
    case 0:   // 8_8_8_8
    case 1:   // 8_8_8_8_GAMMA
      return dl::TEX_FORMAT_RGBA8_UNORM;
    case 2:   // 2_10_10_10
    case 10:  // 2_10_10_10_AS_10_10_10_10
      return dl::TEX_FORMAT_RGB10A2_UNORM;
    case 4:   // 16_16 (fixed -32..32)
    case 6:   // 16_16_FLOAT
      return dl::TEX_FORMAT_RG16_FLOAT;
    case 3:   // 2_10_10_10_FLOAT
    case 5:   // 16_16_16_16 (fixed -32..32)
    case 7:   // 16_16_16_16_FLOAT
    case 12:  // 2_10_10_10_FLOAT_AS_16_16_16_16
      return dl::TEX_FORMAT_RGBA16_FLOAT;
    case 14:
      return dl::TEX_FORMAT_R32_FLOAT;
    case 15:
      return dl::TEX_FORMAT_RG32_FLOAT;
    default:
      return dl::TEX_FORMAT_RGBA8_UNORM;
  }
}

dl::TEXTURE_FORMAT DepthFormat(uint32_t format) {
  return format == 1 ? dl::TEX_FORMAT_D32_FLOAT_S8X24_UINT : dl::TEX_FORMAT_D24_UNORM_S8_UINT;
}

// Guest texture format (fetch constant dword1 & 0x3F) as a resolve destination.
dl::TEXTURE_FORMAT GuestTextureFormat(uint32_t format) {
  switch (format) {
    case 2:   // k_8
      return dl::TEX_FORMAT_R8_UNORM;
    case 6:   // k_8_8_8_8
      return dl::TEX_FORMAT_RGBA8_UNORM;
    case 7:   // k_2_10_10_10
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
    default:
      return dl::TEX_FORMAT_RGBA8_UNORM;
  }
}

bool ReadSurface(const uint8_t* base, uint32_t surface, bool depth, SurfaceDesc& out) {
  if (!Plausible(surface)) return false;
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

HostRt* GetRt(const SurfaceDesc& s) {
  RtKey key{s.base_tile, s.pitch, s.format, s.depth};
  HostRt& rt = g_rts[key];
  if (rt.texture && rt.width == s.width && rt.height >= s.height) return &rt;
  dl::TextureDesc desc;
  desc.Name = s.depth ? "EdramDepth" : "EdramColor";
  desc.Type = dl::RESOURCE_DIM_TEX_2D;
  desc.Width = s.width;
  desc.Height = std::max(s.height, rt.height);
  desc.MipLevels = 1;
  desc.Format = s.depth ? DepthFormat(s.format) : ColorFormat(s.format);
  desc.BindFlags = (s.depth ? dl::BIND_DEPTH_STENCIL : dl::BIND_RENDER_TARGET) |
                   dl::BIND_SHADER_RESOURCE;
  desc.Usage = dl::USAGE_DEFAULT;
  rt.texture.Release();
  g_device->renderDevice()->CreateTexture(desc, nullptr, &rt.texture);
  if (!rt.texture) {
    REXLOG_ERROR("DanteDevice: failed to create host RT base={} pitch={} fmt={} depth={}",
                 s.base_tile, s.pitch, s.format, s.depth);
    g_rts.erase(key);
    return nullptr;
  }
  rt.width = desc.Width;
  rt.height = desc.Height;
  rt.format = desc.Format;
  REXLOG_INFO("NATIVE-RT new {} RT: edram base={} pitch={} fmt={} -> {}x{}",
              s.depth ? "depth" : "color", s.base_tile, s.pitch, s.format, rt.width, rt.height);
  return &rt;
}

GuestTexture* GetGuestTexture(uint32_t address, uint32_t width, uint32_t height,
                              uint32_t guest_format) {
  GuestTextureKey key{address, width, height, guest_format};
  GuestTexture& t = g_textures[key];
  if (t.texture) return &t;
  dl::TextureDesc desc;
  desc.Name = "ResolveTarget";
  desc.Type = dl::RESOURCE_DIM_TEX_2D;
  desc.Width = width;
  desc.Height = height;
  desc.MipLevels = 1;
  desc.Format = GuestTextureFormat(guest_format);
  desc.BindFlags = dl::BIND_SHADER_RESOURCE | dl::BIND_RENDER_TARGET;
  desc.Usage = dl::USAGE_DEFAULT;
  t.texture.Release();
  g_device->renderDevice()->CreateTexture(desc, nullptr, &t.texture);
  if (!t.texture) {
    REXLOG_ERROR("DanteDevice: failed to create resolve target {:08X} {}x{} fmt={}", address,
                 width, height, guest_format);
    g_textures.erase(key);
    return nullptr;
  }
  t.width = width;
  t.height = height;
  t.guest_format = guest_format;
  t.format = desc.Format;
  REXLOG_INFO("NATIVE-RT new resolve target {:08X} {}x{} guest fmt={}", address, width, height,
              guest_format);
  return &t;
}

// Resolve of a depth surface into a sampled depth texture (k_24_8 /
// k_24_8_FLOAT -> R32_FLOAT, depth in red): texelFetch copy pass.
struct DepthCopy {
  dl::RefCntAutoPtr<dl::IPipelineState> pso;
  dl::RefCntAutoPtr<dl::IShaderResourceBinding> srb;
  dl::RefCntAutoPtr<dl::IBuffer> params;
  bool failed = false;
};
DepthCopy g_depth_copy;

constexpr char kDepthCopyVS[] = R"(#version 450
void main() {
  uint id = uint(gl_VertexIndex);
  gl_Position = vec4(id == 1u ? 3.0 : -1.0, id == 2u ? 3.0 : -1.0, 0.0, 1.0);
}
)";
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

bool EnsureDepthCopy() {
  DepthCopy& d = g_depth_copy;
  if (d.pso || d.failed) return d.pso;
  d.failed = true;
  dl::RefCntAutoPtr<dl::IShader> vs, ps;
  vs.Attach(g_device->createGlslShader(kDepthCopyVS, false, "DepthCopyVS"));
  ps.Attach(g_device->createGlslShader(kDepthCopyPS, true, "DepthCopyPS"));
  if (!vs || !ps) return false;
  dl::GraphicsPipelineStateCreateInfo ci;
  ci.PSODesc.Name = "DepthCopy";
  ci.PSODesc.ResourceLayout.DefaultVariableType = dl::SHADER_RESOURCE_VARIABLE_TYPE_DYNAMIC;
  ci.pVS = vs;
  ci.pPS = ps;
  ci.GraphicsPipeline.NumRenderTargets = 1;
  ci.GraphicsPipeline.RTVFormats[0] = dl::TEX_FORMAT_R32_FLOAT;
  ci.GraphicsPipeline.PrimitiveTopology = dl::PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
  ci.GraphicsPipeline.RasterizerDesc.CullMode = dl::CULL_MODE_NONE;
  ci.GraphicsPipeline.RasterizerDesc.ScissorEnable = dl::True;
  ci.GraphicsPipeline.DepthStencilDesc.DepthEnable = dl::False;
  g_device->renderDevice()->CreateGraphicsPipelineState(ci, &d.pso);
  if (!d.pso) return false;
  d.pso->CreateShaderResourceBinding(&d.srb, true);
  dl::BufferDesc params;
  params.Name = "DepthCopyParams";
  params.Size = 16;
  params.BindFlags = dl::BIND_UNIFORM_BUFFER;
  params.Usage = dl::USAGE_DYNAMIC;
  params.CPUAccessFlags = dl::CPU_ACCESS_WRITE;
  g_device->renderDevice()->CreateBuffer(params, nullptr, &d.params);
  dl::SamplerDesc point;
  point.MinFilter = point.MagFilter = point.MipFilter = dl::FILTER_TYPE_POINT;
  dl::RefCntAutoPtr<dl::ISampler> sampler;
  g_device->renderDevice()->CreateSampler(point, &sampler);
  if (!d.params || !sampler) return false;
  d.srb->GetVariableByName(dl::SHADER_TYPE_PIXEL, "g_Point")->Set(sampler);
  d.srb->GetVariableByName(dl::SHADER_TYPE_PIXEL, "CopyParams")->Set(d.params);
  d.failed = false;
  return true;
}

// Copies depth texels [x1, x2) x [y1, y2) of the host depth RT to (dst_x, dst_y).
void CopyDepth(HostRt& depth, dl::ITexture* dest, uint32_t x1, uint32_t y1, uint32_t x2,
               uint32_t y2, uint32_t dst_x, uint32_t dst_y) {
  if (!EnsureDepthCopy()) return;
  DepthCopy& d = g_depth_copy;
  auto* context = g_device->immediateContext();
  {
    dl::MapHelper<int32_t> map(context, d.params, dl::MAP_WRITE, dl::MAP_FLAG_DISCARD);
    map[0] = int32_t(x1) - int32_t(dst_x);
    map[1] = int32_t(y1) - int32_t(dst_y);
    map[2] = map[3] = 0;
  }
  d.srb->GetVariableByName(dl::SHADER_TYPE_PIXEL, "g_Depth")
      ->Set(depth.texture->GetDefaultView(dl::TEXTURE_VIEW_SHADER_RESOURCE));
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

void ClearColor(HostRt& rt, const float color[4]) {
  auto* rtv = rt.texture->GetDefaultView(dl::TEXTURE_VIEW_RENDER_TARGET);
  g_device->immediateContext()->ClearRenderTarget(rtv, color,
                                                  dl::RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
}

void ClearDepth(HostRt& rt, bool depth, bool stencil, float z, uint8_t stencil_value) {
  dl::CLEAR_DEPTH_STENCIL_FLAGS flags = dl::CLEAR_DEPTH_FLAG_NONE;
  if (depth) flags |= dl::CLEAR_DEPTH_FLAG;
  if (stencil) flags |= dl::CLEAR_STENCIL_FLAG;
  auto* dsv = rt.texture->GetDefaultView(dl::TEXTURE_VIEW_DEPTH_STENCIL);
  g_device->immediateContext()->ClearDepthStencil(dsv, flags, z, stencil_value,
                                                  dl::RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
}

// Draw counts are logged when the next clear/resolve/swap or RT change ends a run.
void FlushDrawLog() {
  if (!g_log_frame || !g_frame_draws) return;
  REXLOG_INFO("NATIVE-RT f{} draws={} on RT0 base={} pitch={} fmt={}", g_frame, g_frame_draws,
              g_draw_key.base_tile, g_draw_key.pitch, g_draw_key.format);
  g_frame_draws = 0;
}

// Vertex shader microcode last loaded by a program flush (guest physical, dwords).
struct LoadedShader {
  uint32_t address = 0, dwords = 0;
};
LoadedShader g_loaded_vs;
// Index buffer of the last submitted draw (M2 observer cross-check).
uint32_t g_last_index_base = 0, g_last_index_count = 0, g_last_index_format = 0;
bool g_in_program_flush = false;
uint32_t g_flush_start = 0;

constexpr uint32_t kPhysicalBase = 0xA0000000u;

// Parses the PM4 packets written in (start, end] (end = last dword written).
void ScanProgramFlush(const uint8_t* base, uint32_t start, uint32_t end) {
  if (!Plausible(start) || !Plausible(end) || end <= start || end - start > 0x40000) return;
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

uint32_t DanteRingWritePtr(uint32_t dev, const uint8_t* base) {
  return Plausible(dev) ? Load32(base, dev + kDevRingWritePtr) : 0;
}

void DanteOnProgramFlushBegin(uint32_t dev, const uint8_t* base) {
  if (!g_device) return;
  g_in_program_flush = true;
  g_flush_start = Load32(base, dev + kDevRingWritePtr);
}

void DanteOnCommandReserve(uint32_t write_ptr) {
  if (g_in_program_flush) g_flush_start = write_ptr;
}

void DanteOnProgramFlushEnd(uint32_t dev, const uint8_t* base) {
  if (!g_in_program_flush) return;
  g_in_program_flush = false;
  ScanProgramFlush(base, g_flush_start, Load32(base, dev + kDevRingWritePtr));
}

namespace {

// The VS microcode the GPU runs is the one the last program flush loaded (D3D
// patches the fetches per declaration, see docs/NATIVE_D3D_MAP.md). Before the
// first flush: variant v bound to the declaration whose id (decl+48) is in
// VS[416*v + 40], ucode VS[+0x20] + blk[+872] with blk = VS + VS[+896 + 8*v].
GuestShaderCode VertexShaderCode(const uint8_t* base, uint32_t dev) {
  if (g_loaded_vs.dwords && g_loaded_vs.dwords <= 0x4000) {
    return {reinterpret_cast<const uint32_t*>(base + kPhysicalBase + g_loaded_vs.address),
            g_loaded_vs.dwords};
  }
  uint32_t vs = Load32(base, dev + kDevVertexShader);
  uint32_t decl = Load32(base, dev + kDevVertexDecl);
  if (!Plausible(vs)) return {};
  uint32_t decl_id = Plausible(decl) ? Load32(base, decl + 48) : 0;
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
  if (!Plausible(ucode) || !size || size > 0x10000) return {};
  return {reinterpret_cast<const uint32_t*>(GuestPtr(base, ucode)), size / 4};
}

// Pixel shader: ucode = PS[+0x18] + blk[+40], size blk[+44], blk = PS + PS[+0x40].
GuestShaderCode PixelShaderCode(const uint8_t* base, uint32_t dev) {
  uint32_t ps = Load32(base, dev + kDevPixelShader);
  if (!Plausible(ps)) return {};
  uint32_t blk = ps + Load32(base, ps + 0x40);
  uint32_t ucode = Load32(base, ps + 0x18) + Load32(base, blk + 40);
  uint32_t size = Load32(base, blk + 44);
  if (!Plausible(ucode) || !size || size > 0x10000) return {};
  return {reinterpret_cast<const uint32_t*>(GuestPtr(base, ucode)), size / 4};
}

// The draw packet ends the command-buffer range the D3D call wrote (dev+48 =
// last dword, verified by M2). PM4 type-3 DRAW_INDX (0x22): [hdr][viz]
// [initiator][dma base][dma size] for indexed, [hdr][viz][initiator] otherwise;
// DRAW_INDX_2 (0x36): [hdr][initiator].
bool ParseDrawPacket(const uint8_t* base, uint32_t dev, uint32_t& initiator, uint32_t& dma_base,
                     uint32_t& dma_size) {
  uint32_t last = Load32(base, dev + kDevRingWritePtr);
  if (!Plausible(last)) return false;
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
  for (uint32_t i = 0; i < 4; ++i) {
    SurfaceDesc s;
    if (!ReadSurface(base, Load32(base, dev + kDevRenderTargets + i * 4), false, s)) break;
    HostRt* rt = GetRt(s);
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
    if (HostRt* rt = GetRt(ds)) {
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

// Effective state + shaders + draw for the draw just submitted.
void SubmitDraw(uint32_t dev, uint32_t initiator, uint32_t dma_base, uint32_t dma_size,
                const InlineDraw* inline_draw, const uint8_t* base) {
  State state;
  Capture(dev, base, state);
  uint32_t index = 0;
  for (const auto& range : kShadowRanges) {
    for (uint32_t i = 0; i < range.count; ++i) g_regs->values[range.reg + i] = state[index++];
  }
  g_regs->values[rex::graphics::XE_GPU_REG_VGT_DRAW_INITIATOR] = initiator;
  g_regs->values[rex::graphics::XE_GPU_REG_VGT_DMA_BASE] = dma_base;
  g_regs->values[rex::graphics::XE_GPU_REG_VGT_DMA_SIZE] = dma_size;
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
    g_regs->values[fetch] = GpuAddress(inline_draw->data) | 3;
    g_regs->values[fetch + 1] = (words << 2) | 2;  // k8in32
  }
  TranslatedShaders shaders;
  if (!g_shaders->Prepare(*g_regs, VertexShaderCode(base, dev), PixelShaderCode(base, dev),
                          shaders)) {
    ++g_draws_without_shaders;
    return;
  }
  DrawShaders draw_shaders{shaders.vertex,     shaders.pixel,      shaders.vertex_info,
                           shaders.pixel_info, shaders.vertex_key, shaders.pixel_key};
  g_draws->Draw(*g_regs, draw_shaders, BoundTargets(base, dev), base, g_log_frame,
                inline_draw ? 95u : ~0u);
}

dl::TEXTURE_COMPONENT_SWIZZLE ComponentSwizzle(uint32_t component) {
  switch (component) {
    case 0: return dl::TEXTURE_COMPONENT_SWIZZLE_R;
    case 1: return dl::TEXTURE_COMPONENT_SWIZZLE_G;
    case 2: return dl::TEXTURE_COMPONENT_SWIZZLE_B;
    case 3: return dl::TEXTURE_COMPONENT_SWIZZLE_A;
    case 4: return dl::TEXTURE_COMPONENT_SWIZZLE_ZERO;
    default: return dl::TEXTURE_COMPONENT_SWIZZLE_ONE;
  }
}

std::unordered_map<uint64_t, dl::RefCntAutoPtr<dl::ITextureView>> g_front_views;

dl::ITextureView* FrontBufferView(dl::ITexture* texture, uint32_t swizzle) {
  uint64_t key = uint64_t(reinterpret_cast<uintptr_t>(texture)) ^ (uint64_t(swizzle) << 52);
  auto it = g_front_views.find(key);
  if (it != g_front_views.end()) return it->second;
  dl::TextureViewDesc desc;
  desc.ViewType = dl::TEXTURE_VIEW_SHADER_RESOURCE;
  desc.TextureDim = dl::RESOURCE_DIM_TEX_2D;
  desc.Swizzle = {ComponentSwizzle(swizzle & 7), ComponentSwizzle((swizzle >> 3) & 7),
                  ComponentSwizzle((swizzle >> 6) & 7), ComponentSwizzle((swizzle >> 9) & 7)};
  dl::RefCntAutoPtr<dl::ITextureView> view;
  texture->CreateView(desc, &view);
  g_front_views.emplace(key, view);
  return view ? view.RawPtr() : texture->GetDefaultView(dl::TEXTURE_VIEW_SHADER_RESOURCE);
}

void PresentTestPattern() {
  float phase = float(g_frame % 240) / 240.0f * 6.2831853f;
  g_device->beginFrame();
  g_device->clear(0.3f + 0.3f * std::sin(phase), 0.3f + 0.3f * std::sin(phase + 2.0943951f),
                  0.3f + 0.3f * std::sin(phase + 4.1887902f), 1.0f);
  g_device->present(0);
}

}  // namespace

bool StartDanteDevice(rex::ui::Window* window) {
  std::lock_guard lock(g_mutex);
  if (g_device) return true;
  void* hwnd = window ? window->GetNativeWindowHandle() : nullptr;
  if (!hwnd) {
    REXLOG_ERROR("DanteDevice: no game window");
    return false;
  }
  window->SetPresenter(nullptr);
  auto device = std::make_unique<dante::NativeDevice>();
  if (!device->initialize(hwnd, 1280, 720)) {
    REXLOG_ERROR("DanteDevice: native device initialization failed");
    return false;
  }
  device->setDisplayAspect(16.0 / 9.0, true);
  g_device = std::move(device);
  g_shaders = std::make_unique<ShaderCache>(g_device->renderDevice());
  g_texture_cache = std::make_unique<TextureCache>(
      g_device->renderDevice(), g_device->immediateContext(),
      [](uint32_t address, uint32_t width, uint32_t height, uint32_t format,
         bool& rb_swap) -> dl::ITexture* {
        auto it = g_textures.find(GuestTextureKey{address, width, height, format});
        if (it == g_textures.end()) return nullptr;
        rb_swap = it->second.rb_swap;
        return it->second.texture.RawPtr();
      });
  g_draws = std::make_unique<DrawRenderer>(g_device->renderDevice(), g_device->immediateContext(),
                                           g_texture_cache.get());
  g_regs = std::make_unique<rex::graphics::RegisterFile>();
  g_frame = 0;
  REXLOG_INFO("DanteDevice: presenting on the game window (SDK presenter detached)");
  return true;
}

void StopDanteDevice() {
  std::lock_guard lock(g_mutex);
  if (!g_device) return;
  g_rts.clear();
  g_textures.clear();
  g_depth_copy = {};
  g_front_views.clear();
  g_loaded_vs = {};
  g_draws.reset();
  g_texture_cache.reset();
  g_shaders.reset();
  g_device->shutdown();
  g_device.reset();
  REXLOG_INFO("DanteDevice: stopped after {} frames", g_frame);
}

// Clear(count, pRects, flags, color, Z, stencil): flags TARGET0..3 = bits 0-3,
// ZBUFFER = 0x10, STENCIL = 0x20; color is D3DCOLOR (ARGB).
void DanteLastIndexBuffer(uint32_t& base, uint32_t& count, uint32_t& format) {
  base = g_last_index_base;
  count = g_last_index_count;
  format = g_last_index_format;
}

void DanteShaderHashes(uint32_t dev, const uint8_t* base, uint64_t& vs, uint64_t& ps) {
  GuestShaderCode v = VertexShaderCode(base, dev), p = PixelShaderCode(base, dev);
  vs = v.ucode ? XXH3_64bits(v.ucode, v.dwords * 4) : 0;
  ps = p.ucode ? XXH3_64bits(p.ucode, p.dwords * 4) : 0;
}

void DanteOnClear(const CallArgs& a, const uint8_t* base) {
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
    if (HostRt* rt = GetRt(s)) ClearColor(*rt, color);
    if (g_log_frame) {
      REXLOG_INFO("NATIVE-RT f{} clear RT{} base={} pitch={} fmt={} color={:08X}", g_frame, i,
                  s.base_tile, s.pitch, s.format, c);
    }
  }
  if (flags & 0x30) {
    SurfaceDesc s;
    if (ReadSurface(base, Load32(base, dev + kDevDepthStencil), true, s)) {
      if (HostRt* rt = GetRt(s)) {
        ClearDepth(*rt, flags & 0x10, flags & 0x20, float(a.f1), uint8_t(a.r[5]));
      }
      if (g_log_frame) {
        REXLOG_INFO("NATIVE-RT f{} clear depth base={} pitch={} z={:.3f} stencil={}", g_frame,
                    s.base_tile, s.pitch, a.f1, a.r[5] & 0xFF);
      }
    }
  }
}

// Resolve(flags, pSourceRect, pDestTexture, pDestPoint, level, slice,
// pClearColor, ClearZ, ...): flags & 7 = source (RT0-3, 4 = depth),
// 0x100 / 0x200 = clear color / depth-stencil afterwards.
void DanteOnResolve(const CallArgs& a, const uint8_t* base) {
  std::lock_guard lock(g_mutex);
  if (!g_device) return;
  FlushDrawLog();
  uint32_t dev = a.r[0];
  uint32_t flags = a.r[1];
  uint32_t source = flags & 7;
  bool from_depth = source == 4;
  SurfaceDesc s;
  uint32_t surface = Load32(base, dev + (from_depth ? kDevDepthStencil : kDevRenderTargets + source * 4));
  if (!ReadSurface(base, surface, from_depth, s)) return;
  HostRt* rt = GetRt(s);
  if (!rt) return;

  uint32_t x1 = 0, y1 = 0, x2 = s.width, y2 = s.height;
  if (Plausible(a.r[2])) {
    x1 = Load32(base, a.r[2]);
    y1 = Load32(base, a.r[2] + 4);
    x2 = Load32(base, a.r[2] + 8);
    y2 = Load32(base, a.r[2] + 12);
  }
  uint32_t dst_x = 0, dst_y = 0;
  if (Plausible(a.r[4])) {
    dst_x = Load32(base, a.r[4]);
    dst_y = Load32(base, a.r[4] + 4);
  }

  uint32_t dest = a.r[3];
  if (Plausible(dest)) {
    // Fetch constant at +0x1C: dword1 = base | format, dword2 = size.
    uint32_t dword1 = Load32(base, dest + 0x20);
    uint32_t dword2 = Load32(base, dest + 0x24);
    // Texture objects hold the guest virtual address; fetch constants (what
    // sampling sees) hold the physical one.
    uint32_t address = GpuAddress(dword1 & 0xFFFFF000u);
    uint32_t guest_format = dword1 & 0x3F;
    uint32_t width = (dword2 & 0x1FFF) + 1;
    uint32_t height = ((dword2 >> 13) & 0x1FFF) + 1;
    GuestTexture* t = GetGuestTexture(address, width, height, guest_format);
    uint32_t dest_info = FindRegisterWrite(base, a.ring_before,
                                           Load32(base, dev + kDevRingWritePtr), 0x231B);
    bool rb_swap = dest_info != ~0u && ((dest_info >> 24) & 1);
    const char* result = "skipped";
    if (t && !from_depth && t->format == rt->format) {
      x2 = std::min({x2, rt->width, x1 + (t->width - std::min(dst_x, t->width))});
      y2 = std::min({y2, rt->height, y1 + (t->height - std::min(dst_y, t->height))});
      if (x2 > x1 && y2 > y1) {
        dl::Box box(x1, x2, y1, y2);
        dl::CopyTextureAttribs copy(rt->texture, dl::RESOURCE_STATE_TRANSITION_MODE_TRANSITION,
                                    t->texture, dl::RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
        copy.pSrcBox = &box;
        copy.DstX = dst_x;
        copy.DstY = dst_y;
        g_device->immediateContext()->CopyTexture(copy);
        t->rb_swap = rb_swap;
        result = rb_swap ? "copied(rb swap)" : "copied";
      }
    } else if (t && from_depth && t->format == dl::TEX_FORMAT_R32_FLOAT) {
      x2 = std::min({x2, rt->width, x1 + (t->width - std::min(dst_x, t->width))});
      y2 = std::min({y2, rt->height, y1 + (t->height - std::min(dst_y, t->height))});
      if (x2 > x1 && y2 > y1) {
        CopyDepth(*rt, t->texture, x1, y1, x2, y2, dst_x, dst_y);
        result = "depth-copied";
      }
    } else if (t) {
      // Other format conversions need a shader copy.
      uint64_t pair = (uint64_t(from_depth) << 40) | (uint64_t(s.format) << 32) | guest_format;
      if (g_reported_conversions.insert(pair).second) {
        REXLOG_INFO("NATIVE-RT resolve needs conversion: {} fmt={} -> guest fmt={}",
                    from_depth ? "depth" : "color", s.format, guest_format);
      }
    }
    if (g_dump_frame && t) {
      DumpTexturePng(g_device->renderDevice(), g_device->immediateContext(), t->texture,
                     FrameDumpDirectory() /
                         fmt::format("f{}_{:02}_{}_{:08X}_{}x{}_g{}.png", g_frame, ++g_dump_seq,
                                     from_depth ? "depth" : "color", address, width, height,
                                     guest_format));
    }
    if (g_log_frame) {
      REXLOG_INFO(
          "NATIVE-RT f{} resolve {} base={} pitch={} fmt={} rect=({},{})-({},{}) -> {:08X} "
          "{}x{} fmt={} at ({},{}) {} dest_info={:08X}",
          g_frame, from_depth ? "depth" : "color", s.base_tile, s.pitch, s.format, x1, y1, x2, y2,
          address, width, height, guest_format, dst_x, dst_y, result, dest_info);
    }
  }

  if ((flags & 0x100) && !from_depth && Plausible(a.r[7])) {
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
      if (HostRt* depth_rt = GetRt(ds)) ClearDepth(*depth_rt, true, true, float(a.f1), 0);
    }
  }
}

void DanteOnDraw(uint32_t device, const uint8_t* base) {
  if (!g_log_frame) return;
  std::lock_guard lock(g_mutex);
  SurfaceDesc s;
  ReadSurface(base, Load32(base, device + kDevRenderTargets), false, s);
  RtKey key{s.base_tile, s.pitch, s.format, false};
  if (!(key == g_draw_key)) FlushDrawLog();
  g_draw_key = key;
  ++g_frame_draws;
}

void DanteOnDrawSubmitted(const CallArgs& a, int kind, const uint8_t* base) {
  std::lock_guard lock(g_mutex);
  if (!g_device || !g_shaders || !g_draws) return;
  if (kind == 2) return;  // BeginVertices: drawn at EndVertices
  uint32_t dev = a.r[0];
  if (kind == 3) {
    // DrawVerticesUP(prim, count, pData, stride): its nested BeginVertices
    // provided the ring copy of the data.
    if (!g_inline.pending) return;
    g_inline.pending = false;
    uint32_t initiator = (g_inline.count << 16) | (2u << 6) | (g_inline.prim & 0x3F);
    SubmitDraw(dev, initiator, 0, 0, &g_inline, base);
    return;
  }
  uint32_t initiator, dma_base, dma_size;
  if (!ParseDrawPacket(base, dev, initiator, dma_base, dma_size)) {
    ++g_draws_unparsed;
    return;
  }
  SubmitDraw(dev, initiator, dma_base, dma_size, nullptr, base);
}

void DanteOnBeginVertices(const CallArgs& a, uint32_t data) {
  std::lock_guard lock(g_mutex);
  g_inline.pending = true;
  g_inline.args = a;
  g_inline.prim = a.r[1];
  g_inline.count = a.r[2];
  g_inline.stride = a.r[3];
  g_inline.data = data;
}

void DanteOnEndVertices(const uint8_t* base) {
  std::lock_guard lock(g_mutex);
  if (!g_device || !g_draws || !g_inline.pending) return;
  g_inline.pending = false;
  uint32_t initiator = (g_inline.count << 16) | (2u << 6) | (g_inline.prim & 0x3F);
  SubmitDraw(g_inline.args.r[0], initiator, 0, 0, &g_inline, base);
}

// Swap(pFrontBuffer, ...): present the native texture resolved to the front buffer.
void DanteOnSwap(const CallArgs& a, const uint8_t* base, bool log_next_frame) {
  std::lock_guard lock(g_mutex);
  if (!g_device) return;
  FlushDrawLog();
  g_frame_draws = 0;

  uint32_t front = a.r[1];
  uint32_t address = 0;
  GuestTextureKey key{};
  if (Plausible(front)) {
    uint32_t dword1 = Load32(base, front + 0x20);
    uint32_t dword2 = Load32(base, front + 0x24);
    address = GpuAddress(dword1 & 0xFFFFF000u);
    key = {address, (dword2 & 0x1FFF) + 1, ((dword2 >> 13) & 0x1FFF) + 1, dword1 & 0x3F};
  }
  auto it = g_textures.find(key);
  bool found = it != g_textures.end() && it->second.texture;

  // Debug view: present a color host RT instead of the front buffer.
  dl::ITextureView* debug_view = nullptr;
  if (int32_t view = REXCVAR_GET(dante_debug_view); view > 0) {
    std::vector<std::pair<RtKey, HostRt*>> colors;
    for (auto& [rt_key, rt] : g_rts) {
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
  if (g_log_frame) {
    REXLOG_INFO("NATIVE-RT f{} swap front={:08X} {}", g_frame, address,
                found ? "presented" : "missing (test pattern)");
  }
  if (g_dump_frame && found) {
    DumpTexturePng(g_device->renderDevice(), g_device->immediateContext(), it->second.texture,
                   FrameDumpDirectory() / fmt::format("f{}_99_front.png", g_frame));
  }
  ++g_frame;
  g_dump_frame = log_next_frame && !g_log_frame;
  g_dump_seq = 0;
  g_log_frame = log_next_frame;
  if (g_draws) g_draws->SetFrame(g_frame);
  if (g_shaders && g_draws && g_frame % 300 == 0) {
    g_shaders->LogStats();
    g_draws->LogStats();
    g_texture_cache->LogStats();
    REXLOG_INFO("NATIVE-DRAW draws_without_shaders={} unparsed={}", g_draws_without_shaders,
                g_draws_unparsed);
  }
  if (!g_device->updateWindowSize()) return;  // minimized
  if (debug_view) {
    g_device->presentTexture(debug_view, 0);
  } else if (found) {
    // The front buffer's fetch swizzle (dword 3 bits 1-12) maps the stored
    // channels to RGB, e.g. ZYX1 for this game's BGR-ordered front buffer.
    uint32_t swizzle = (Load32(base, front + 0x28) >> 1) & 0xFFF;
    if (it->second.rb_swap) swizzle = SwapRedBlue(swizzle);
    g_device->presentTexture(FrontBufferView(it->second.texture, swizzle), 0);
  } else {
    PresentTestPattern();
  }
}

}  // namespace native
