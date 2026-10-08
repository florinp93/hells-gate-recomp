#include "dante_textures.h"

#include "Common/interface/RefCntAutoPtr.hpp"
#include "Graphics/GraphicsEngine/interface/DeviceContext.h"
#include "Graphics/GraphicsEngine/interface/RenderDevice.h"
#include "Graphics/GraphicsEngine/interface/Sampler.h"
#include "Graphics/GraphicsEngine/interface/Texture.h"
#include "Graphics/GraphicsEngine/interface/TextureView.h"

#include <rex/graphics/pipeline/texture/conversion.h>
#include <rex/graphics/pipeline/texture/info.h>
#include <rex/graphics/pipeline/texture/util.h>
#include <rex/graphics/xenos.h>
#include <rex/logging/macros.h>

#define XXH_STATIC_LINKING_ONLY  // XXH3_state_t on the stack
#include <xxhash.h>

#include <fmt/format.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <set>
#include <unordered_map>
#include <vector>

namespace native {
namespace {

namespace dl = Diligent;
namespace rg = rex::graphics;
namespace xenos = rex::graphics::xenos;

constexpr uint32_t kSharedMemorySize = 512u * 1024 * 1024;
constexpr uint32_t kPhysicalMirror = 0xA0000000u;

struct HostFormat {
  dl::TEXTURE_FORMAT format;
  uint32_t bytes_per_block;
  // Signed variant (sampled through the translator's _s bindings): the texture
  // is created typeless so both views exist. UNKNOWN if the SDK has no signed
  // host format (DXT etc.; signed fetches then read zero, as in the SDK).
  dl::TEXTURE_FORMAT typeless = dl::TEX_FORMAT_UNKNOWN;
  dl::TEXTURE_FORMAT snorm = dl::TEX_FORMAT_UNKNOWN;
};

// Guest texture format -> host format with the same block size (data copied
// after the endian swap; component order is handled by the fetch swizzle).
bool GetHostFormat(xenos::TextureFormat format, HostFormat& out) {
  switch (format) {
    case xenos::TextureFormat::k_8:
    case xenos::TextureFormat::k_8_A:
    case xenos::TextureFormat::k_8_B:
      out = {dl::TEX_FORMAT_R8_UNORM, 1, dl::TEX_FORMAT_R8_TYPELESS, dl::TEX_FORMAT_R8_SNORM};
      return true;
    case xenos::TextureFormat::k_8_8:
      out = {dl::TEX_FORMAT_RG8_UNORM, 2, dl::TEX_FORMAT_RG8_TYPELESS, dl::TEX_FORMAT_RG8_SNORM};
      return true;
    case xenos::TextureFormat::k_8_8_8_8:
    case xenos::TextureFormat::k_8_8_8_8_A:
    case xenos::TextureFormat::k_8_8_8_8_AS_16_16_16_16:
      out = {dl::TEX_FORMAT_RGBA8_UNORM, 4, dl::TEX_FORMAT_RGBA8_TYPELESS, dl::TEX_FORMAT_RGBA8_SNORM};
      return true;
    case xenos::TextureFormat::k_2_10_10_10:
    case xenos::TextureFormat::k_2_10_10_10_AS_16_16_16_16:
      out = {dl::TEX_FORMAT_RGB10A2_UNORM, 4};
      return true;
    case xenos::TextureFormat::k_DXT1:
    case xenos::TextureFormat::k_DXT1_AS_16_16_16_16:
      out = {dl::TEX_FORMAT_BC1_UNORM, 8};
      return true;
    case xenos::TextureFormat::k_DXT2_3:
    case xenos::TextureFormat::k_DXT2_3_AS_16_16_16_16:
      out = {dl::TEX_FORMAT_BC2_UNORM, 16};
      return true;
    case xenos::TextureFormat::k_DXT4_5:
    case xenos::TextureFormat::k_DXT4_5_AS_16_16_16_16:
      out = {dl::TEX_FORMAT_BC3_UNORM, 16};
      return true;
    case xenos::TextureFormat::k_DXN:
      out = {dl::TEX_FORMAT_BC5_UNORM, 16};
      return true;
    case xenos::TextureFormat::k_16:
      out = {dl::TEX_FORMAT_R16_UNORM, 2, dl::TEX_FORMAT_R16_TYPELESS, dl::TEX_FORMAT_R16_SNORM};
      return true;
    case xenos::TextureFormat::k_16_16:
      out = {dl::TEX_FORMAT_RG16_UNORM, 4, dl::TEX_FORMAT_RG16_TYPELESS, dl::TEX_FORMAT_RG16_SNORM};
      return true;
    case xenos::TextureFormat::k_16_16_16_16:
      out = {dl::TEX_FORMAT_RGBA16_UNORM, 8, dl::TEX_FORMAT_RGBA16_TYPELESS, dl::TEX_FORMAT_RGBA16_SNORM};
      return true;
    case xenos::TextureFormat::k_16_FLOAT:
      out = {dl::TEX_FORMAT_R16_FLOAT, 2};
      return true;
    case xenos::TextureFormat::k_16_16_FLOAT:
      out = {dl::TEX_FORMAT_RG16_FLOAT, 4};
      return true;
    case xenos::TextureFormat::k_16_16_16_16_FLOAT:
      out = {dl::TEX_FORMAT_RGBA16_FLOAT, 8};
      return true;
    case xenos::TextureFormat::k_32_FLOAT:
      out = {dl::TEX_FORMAT_R32_FLOAT, 4};
      return true;
    case xenos::TextureFormat::k_32_32_FLOAT:
      out = {dl::TEX_FORMAT_RG32_FLOAT, 8};
      return true;
    case xenos::TextureFormat::k_32_32_32_32_FLOAT:
      out = {dl::TEX_FORMAT_RGBA32_FLOAT, 16};
      return true;
    default:
      return false;
  }
}

dl::TEXTURE_COMPONENT_SWIZZLE Swizzle(uint32_t component) {
  switch (component) {
    case 0: return dl::TEXTURE_COMPONENT_SWIZZLE_R;
    case 1: return dl::TEXTURE_COMPONENT_SWIZZLE_G;
    case 2: return dl::TEXTURE_COMPONENT_SWIZZLE_B;
    case 3: return dl::TEXTURE_COMPONENT_SWIZZLE_A;
    case 4: return dl::TEXTURE_COMPONENT_SWIZZLE_ZERO;
    default: return dl::TEXTURE_COMPONENT_SWIZZLE_ONE;
  }
}

dl::TEXTURE_ADDRESS_MODE AddressMode(xenos::ClampMode mode) {
  switch (mode) {
    case xenos::ClampMode::kRepeat: return dl::TEXTURE_ADDRESS_WRAP;
    case xenos::ClampMode::kMirroredRepeat: return dl::TEXTURE_ADDRESS_MIRROR;
    case xenos::ClampMode::kClampToEdge:
    case xenos::ClampMode::kClampToHalfway: return dl::TEXTURE_ADDRESS_CLAMP;
    case xenos::ClampMode::kClampToBorder: return dl::TEXTURE_ADDRESS_BORDER;
    default: return dl::TEXTURE_ADDRESS_MIRROR_ONCE;
  }
}

// Host channel layout per guest format (as the SDK Vulkan texture cache):
// single-channel formats replicate R, two-channel formats are RGGG. Returned as
// the host component (0-3) each guest component X/Y/Z/W reads.
std::array<uint32_t, 4> HostSwizzle(xenos::TextureFormat format) {
  switch (format) {
    case xenos::TextureFormat::k_8:
    case xenos::TextureFormat::k_8_A:
    case xenos::TextureFormat::k_8_B:
    case xenos::TextureFormat::k_16:
    case xenos::TextureFormat::k_16_FLOAT:
    case xenos::TextureFormat::k_32_FLOAT:
    case xenos::TextureFormat::k_24_8:
    case xenos::TextureFormat::k_24_8_FLOAT:
      return {0, 0, 0, 0};
    case xenos::TextureFormat::k_8_8:
    case xenos::TextureFormat::k_16_16:
    case xenos::TextureFormat::k_16_16_FLOAT:
    case xenos::TextureFormat::k_32_32_FLOAT:
    case xenos::TextureFormat::k_DXN:
      return {0, 1, 1, 1};
    default:
      return {0, 1, 2, 3};
  }
}

// Fetch swizzle (3 bits per component: 0-3 = X..W, 4 = 0, 5 = 1) composed
// with the host channel layout.
uint32_t ComposeSwizzle(uint32_t fetch_swizzle, xenos::TextureFormat format) {
  std::array<uint32_t, 4> host = HostSwizzle(format);
  uint32_t out = 0;
  for (uint32_t c = 0; c < 4; ++c) {
    uint32_t s = (fetch_swizzle >> (3 * c)) & 7;
    out |= (s < 4 ? host[s] : s) << (3 * c);
  }
  return out;
}

struct GuestTextureEntry {
  dl::RefCntAutoPtr<dl::ITexture> texture;
  uint64_t content_hash = 0;
  uint64_t checked_frame = UINT64_MAX;
};

}  // namespace

struct TextureCache::Impl {
  dl::IRenderDevice* device;
  dl::IDeviceContext* context;
  ResolveLookup resolve_lookup;
  // Key: XXH3 of the fetch dwords identifying the texture data.
  std::unordered_map<uint64_t, GuestTextureEntry> textures;
  std::unordered_map<uint64_t, dl::RefCntAutoPtr<dl::ITextureView>> views;
  std::unordered_map<uint64_t, dl::RefCntAutoPtr<dl::ISampler>> samplers;
  std::set<uint32_t> reported_formats;
  std::vector<uint8_t> staging;
  uint32_t uploads = 0, resolved_hits = 0, unsupported = 0;
  uint64_t uploaded_bytes = 0;

  // Cheap change detection: the first 64 bytes of every 4 KB page.
  static uint64_t SampleHash(const uint8_t* data, uint32_t size) {
    XXH3_state_t state;
    XXH3_64bits_reset(&state);
    for (uint32_t offset = 0; offset < size; offset += 4096) {
      XXH3_64bits_update(&state, data + offset, std::min<uint32_t>(64, size - offset));
    }
    return XXH3_64bits_digest(&state);
  }

  // format: view format for typeless textures (UNKNOWN = the texture's own).
  dl::ITextureView* View(dl::ITexture* texture, uint32_t swizzle,
                         dl::RESOURCE_DIMENSION dimension = dl::RESOURCE_DIM_TEX_2D_ARRAY,
                         dl::TEXTURE_FORMAT format = dl::TEX_FORMAT_UNKNOWN) {
    uint64_t key = uint64_t(reinterpret_cast<uintptr_t>(texture)) ^ (uint64_t(swizzle) << 52) ^
                   (uint64_t(format) << 44);
    auto it = views.find(key);
    if (it != views.end()) return it->second;
    dl::TextureViewDesc desc;
    desc.ViewType = dl::TEXTURE_VIEW_SHADER_RESOURCE;
    desc.TextureDim = dimension;
    desc.Format = format;
    desc.Swizzle = {Swizzle(swizzle & 7), Swizzle((swizzle >> 3) & 7), Swizzle((swizzle >> 6) & 7),
                    Swizzle((swizzle >> 9) & 7)};
    dl::RefCntAutoPtr<dl::ITextureView> view;
    texture->CreateView(desc, &view);
    views.emplace(key, view);
    return view;
  }

  // All stored levels (0..levels-1) of every array slice, in DiligentCore
  // subresource order (slice-major). Uses the SDK guest layout: level 0 from
  // the base address, levels 1+ from the mip address, levels at or past the
  // packed level from inside the packed mip tail.
  struct LoadedLevels {
    std::vector<uint8_t> data;
    std::vector<dl::TextureSubResData> subresources;
    uint32_t width = 0, height = 0, array_size = 1, levels = 1;
  };
  bool LoadLevels(const xenos::xe_gpu_texture_fetch_t& fetch, const HostFormat& host,
                  const uint8_t* base, LoadedLevels& out, uint64_t& content_hash,
                  bool hash_only = false) {
    uint32_t width_m1, height_m1, depth_m1, base_page, mip_page, mip_min, mip_max;
    rg::texture_util::GetSubresourcesFromFetchConstant(fetch, &width_m1, &height_m1, &depth_m1,
                                                       &base_page, &mip_page, &mip_min, &mip_max);
    if (!base_page) return false;  // no base level (mip-only textures are rare)
    auto format = xenos::TextureFormat(fetch.format);
    const rg::FormatInfo* format_info = rg::FormatInfo::Get(format);
    uint32_t bw = format_info->block_width, bh = format_info->block_height;
    uint32_t bpb = host.bytes_per_block;
    uint32_t bpb_log2 = bpb >= 16 ? 4 : bpb >= 8 ? 3 : bpb >= 4 ? 2 : bpb >= 2 ? 1 : 0;
    uint32_t width = width_m1 + 1, height = height_m1 + 1, array_size = depth_m1 + 1;
    uint32_t max_level = mip_page ? mip_max : 0;
    auto dimension = xenos::DataDimension(fetch.dimension);
    auto layout = rg::texture_util::GetGuestTextureLayout(
        dimension, fetch.pitch, width, height, array_size, fetch.tiled, format, fetch.packed_mips,
        true, max_level);
    uint32_t base_address = base_page << 12, mip_address = mip_page << 12;

    // Content hash: sampled over the base and mip data.
    {
      XXH3_state_t state;
      XXH3_64bits_reset(&state);
      auto sample = [&](uint32_t address, uint32_t size) {
        if (!size || address >= kSharedMemorySize) return;
        size = std::min(size, kSharedMemorySize - address);
        const uint8_t* p = base + kPhysicalMirror + address;
        for (uint32_t offset = 0; offset < size; offset += 4096) {
          XXH3_64bits_update(&state, p + offset, std::min<uint32_t>(64, size - offset));
        }
      };
      sample(base_address, layout.base.level_data_extent_bytes);
      if (max_level) sample(mip_address, layout.mips_total_extent_bytes);
      content_hash = XXH3_64bits_digest(&state);
    }
    if (hash_only) return true;

    out.width = width;
    out.height = height;
    out.array_size = array_size;
    out.levels = max_level + 1;
    std::vector<size_t> offsets;
    size_t total = 0;
    for (uint32_t slice = 0; slice < array_size; ++slice) {
      for (uint32_t level = 0; level <= max_level; ++level) {
        uint32_t lw = std::max(width >> level, 1u), lh = std::max(height >> level, 1u);
        offsets.push_back(total);
        total += size_t((lw + bw - 1) / bw) * ((lh + bh - 1) / bh) * bpb;
      }
    }
    out.data.assign(total, 0);
    out.subresources.clear();
    size_t index = 0;
    for (uint32_t slice = 0; slice < array_size; ++slice) {
      for (uint32_t level = 0; level <= max_level; ++level, ++index) {
        uint32_t lw = std::max(width >> level, 1u), lh = std::max(height >> level, 1u);
        uint32_t blocks_x = (lw + bw - 1) / bw, blocks_y = (lh + bh - 1) / bh;
        bool is_base = level == 0;
        uint32_t stored = std::min(level, layout.packed_level);
        const auto& storage = is_base ? layout.base : layout.mips[stored];
        uint32_t address = is_base ? base_address : mip_address + layout.mip_offsets_bytes[stored];
        address += slice * storage.array_slice_stride_bytes;
        uint32_t ox = 0, oy = 0, oz = 0;
        if (level >= layout.packed_level) {
          rg::texture_util::GetPackedMipOffset(width, height, 1, format, level, ox, oy, oz);
        }
        uint32_t pitch_bytes = storage.row_pitch_bytes;
        uint8_t* dst = out.data.data() + offsets[index];
        for (uint32_t y = 0; y < blocks_y; ++y) {
          for (uint32_t x = 0; x < blocks_x; ++x) {
            uint32_t in = fetch.tiled ? uint32_t(rg::texture_util::GetTiledOffset2D(
                                            int32_t(x + ox), int32_t(y + oy), pitch_bytes / bpb,
                                            bpb_log2))
                                      : (y + oy) * pitch_bytes + (x + ox) * bpb;
            uint32_t source = address + in;
            if (source >= kSharedMemorySize || kSharedMemorySize - source < bpb) continue;
            rg::texture_conversion::CopySwapBlock(xenos::Endian(fetch.endianness),
                                                  dst + (size_t(y) * blocks_x + x) * bpb,
                                                  base + kPhysicalMirror + source, bpb);
          }
        }
        out.subresources.push_back(
            dl::TextureSubResData{dst, uint64_t(blocks_x) * bpb});
      }
    }
    return true;
  }

  // Creates or updates a texture with every loaded subresource.
  bool StoreLevels(GuestTextureEntry& entry, const LoadedLevels& levels, const HostFormat& host,
                   dl::RESOURCE_DIMENSION type, const char* name) {
    if (!entry.texture) {
      dl::TextureDesc desc;
      desc.Name = name;
      desc.Type = type;
      desc.Width = levels.width;
      desc.Height = levels.height;
      desc.ArraySize = levels.array_size;
      desc.MipLevels = levels.levels;
      desc.Format = host.typeless != dl::TEX_FORMAT_UNKNOWN ? host.typeless : host.format;
      desc.BindFlags = dl::BIND_SHADER_RESOURCE;
      desc.Usage = dl::USAGE_DEFAULT;
      dl::TextureData data{const_cast<dl::TextureSubResData*>(levels.subresources.data()),
                           uint32_t(levels.subresources.size())};
      device->CreateTexture(desc, &data, &entry.texture);
      if (!entry.texture) return false;
    } else {
      size_t index = 0;
      for (uint32_t slice = 0; slice < levels.array_size; ++slice) {
        for (uint32_t level = 0; level < levels.levels; ++level, ++index) {
          dl::Box box(0, std::max(levels.width >> level, 1u), 0,
                      std::max(levels.height >> level, 1u));
          context->UpdateTexture(entry.texture, level, slice, box, levels.subresources[index],
                                 dl::RESOURCE_STATE_TRANSITION_MODE_TRANSITION,
                                 dl::RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
        }
      }
    }
    ++uploads;
    uploaded_bytes += levels.data.size();
    return true;
  }
};

TextureCache::TextureCache(dl::IRenderDevice* device, dl::IDeviceContext* context,
                           ResolveLookup resolve_lookup)
    : impl_(std::make_unique<Impl>()) {
  impl_->device = device;
  impl_->context = context;
  impl_->resolve_lookup = std::move(resolve_lookup);
}

TextureCache::~TextureCache() = default;

dl::ITextureView* TextureCache::GetView2D(const uint32_t fetch_dwords[6], const uint8_t* base,
                                          uint64_t frame, char* source, bool is_signed) {
  Impl& m = *impl_;
  char unused;
  char& src_kind = source ? *source : unused;
  src_kind = '-';
  xenos::xe_gpu_texture_fetch_t fetch;
  std::memcpy(&fetch, fetch_dwords, sizeof(fetch));
  if (fetch.type != xenos::FetchConstantType::kTexture) return nullptr;
  rg::TextureInfo info;
  if (!rg::TextureInfo::Prepare(fetch, &info)) return nullptr;
  if (info.dimension != xenos::DataDimension::k2DOrStacked &&
      info.dimension != xenos::DataDimension::k1D) {
    return nullptr;
  }
  uint32_t swizzle = ComposeSwizzle(fetch.swizzle, info.format);
  uint32_t address = info.memory.base_address;
  // TextureInfo keeps the fetch encoding (size - 1).
  uint32_t width = info.width + 1;
  uint32_t height = info.height + 1;

  // Rendered textures: the native resolve output.
  bool rb_swap = false;
  if (dl::ITexture* resolved =
          m.resolve_lookup(address, width, height, uint32_t(info.format), rb_swap)) {
    ++m.resolved_hits;
    src_kind = 'R';
    if (rb_swap) {
      // Swap the red/blue channel selectors of the host swizzle.
      uint32_t swapped = 0;
      for (uint32_t i = 0; i < 4; ++i) {
        uint32_t c = (swizzle >> (3 * i)) & 7;
        swapped |= (c == 0 ? 2u : c == 2 ? 0u : c) << (3 * i);
      }
      swizzle = swapped;
    }
    return m.View(resolved, swizzle);
  }

  HostFormat host;
  if (!GetHostFormat(info.format, host) ||
      info.format_info()->bytes_per_block() != host.bytes_per_block) {
    if (m.reported_formats.insert(uint32_t(info.format)).second) {
      REXLOG_INFO("NATIVE-TEX unsupported guest format {} ({}x{})", uint32_t(info.format),
                  width, height);
    }
    ++m.unsupported;
    return nullptr;
  }
  uint32_t size = info.extent.all_blocks() * host.bytes_per_block;
  if (!size || address >= kSharedMemorySize || size > kSharedMemorySize - address) return nullptr;

  // The texture is identified by its address, format, size and mip setup.
  // (dword 0: pitch and tiling; 1: base and format; 2: size; 5: mip address).
  uint32_t identity[5] = {fetch_dwords[0] & 0xFFC00000u, fetch_dwords[1], fetch_dwords[2],
                          fetch_dwords[5],
                          (uint32_t(fetch.mip_min_level) << 4) | uint32_t(fetch.mip_max_level)};
  uint64_t key = XXH3_64bits(identity, sizeof(identity));
  GuestTextureEntry& entry = m.textures[key];
  src_kind = 'U';
  if (is_signed && host.snorm == dl::TEX_FORMAT_UNKNOWN) {
    src_kind = '0';  // no signed host format: the fetch reads zero
    return nullptr;
  }
  dl::TEXTURE_FORMAT view_format = is_signed ? host.snorm : host.format;
  if (entry.texture && entry.checked_frame == frame) return m.View(entry.texture, swizzle, dl::RESOURCE_DIM_TEX_2D_ARRAY, view_format);
  entry.checked_frame = frame;
  Impl::LoadedLevels levels;
  uint64_t hash = 0;
  if (!m.LoadLevels(fetch, host, base, levels, hash, true)) {
    m.textures.erase(key);
    return nullptr;
  }
  if (entry.texture && entry.content_hash == hash) return m.View(entry.texture, swizzle, dl::RESOURCE_DIM_TEX_2D_ARRAY, view_format);
  entry.content_hash = hash;
  m.LoadLevels(fetch, host, base, levels, hash);
  if (!m.StoreLevels(entry, levels, host, dl::RESOURCE_DIM_TEX_2D_ARRAY, "GuestTexture")) {
    m.textures.erase(key);
    return nullptr;
  }
  return m.View(entry.texture, swizzle, dl::RESOURCE_DIM_TEX_2D_ARRAY, view_format);
}

dl::ITextureView* TextureCache::GetViewCube(const uint32_t fetch_dwords[6], const uint8_t* base,
                                            uint64_t frame, char* source, bool is_signed) {
  Impl& m = *impl_;
  char unused;
  char& src_kind = source ? *source : unused;
  src_kind = '-';
  xenos::xe_gpu_texture_fetch_t fetch;
  std::memcpy(&fetch, fetch_dwords, sizeof(fetch));
  if (fetch.type != xenos::FetchConstantType::kTexture) return nullptr;
  rg::TextureInfo info;
  if (!rg::TextureInfo::Prepare(fetch, &info) || info.dimension != xenos::DataDimension::kCube) {
    return nullptr;
  }
  HostFormat host;
  if (!GetHostFormat(info.format, host) ||
      info.format_info()->bytes_per_block() != host.bytes_per_block) {
    if (m.reported_formats.insert(0x200 | uint32_t(info.format)).second) {
      REXLOG_INFO("NATIVE-TEX unsupported cube guest format {}", uint32_t(info.format));
    }
    return nullptr;
  }
  uint32_t width = info.width + 1, height = info.height + 1;
  const auto& extent = info.extent;
  // Faces are consecutive slices, each aligned to 4 KB.
  uint32_t face_stride =
      (extent.block_pitch_h * extent.block_pitch_v * host.bytes_per_block + 4095) & ~4095u;
  uint32_t address = info.memory.base_address;
  uint32_t size = face_stride * 6;
  if (!face_stride || address >= kSharedMemorySize || size > kSharedMemorySize - address) {
    return nullptr;
  }
  uint32_t swizzle = ComposeSwizzle(fetch.swizzle, info.format);

  uint64_t key = XXH3_64bits(fetch_dwords, 6 * sizeof(uint32_t)) ^ 0xC0BEC0BEull;
  GuestTextureEntry& entry = m.textures[key];
  src_kind = 'U';
  if (is_signed && host.snorm == dl::TEX_FORMAT_UNKNOWN) {
    src_kind = '0';
    return nullptr;
  }
  dl::TEXTURE_FORMAT view_format = is_signed ? host.snorm : host.format;
  if (entry.texture && entry.checked_frame == frame) {
    return m.View(entry.texture, swizzle, dl::RESOURCE_DIM_TEX_CUBE, view_format);
  }
  entry.checked_frame = frame;
  Impl::LoadedLevels levels;
  uint64_t hash = 0;
  if (!m.LoadLevels(fetch, host, base, levels, hash, true)) {
    m.textures.erase(key);
    return nullptr;
  }
  if (entry.texture && entry.content_hash == hash) {
    return m.View(entry.texture, swizzle, dl::RESOURCE_DIM_TEX_CUBE, view_format);
  }
  entry.content_hash = hash;
  m.LoadLevels(fetch, host, base, levels, hash);
  if (levels.array_size != 6) return nullptr;
  bool created = !entry.texture;
  if (!m.StoreLevels(entry, levels, host, dl::RESOURCE_DIM_TEX_CUBE, "GuestTextureCube")) {
    m.textures.erase(key);
    return nullptr;
  }
  if (created) {
    REXLOG_INFO("NATIVE-TEX cube texture {:08X} {}x{} fmt={} levels={}", address, width, height,
                uint32_t(info.format), levels.levels);
  }
  return m.View(entry.texture, swizzle, dl::RESOURCE_DIM_TEX_CUBE, view_format);
}

dl::ITextureView* TextureCache::GetView3D(const uint32_t fetch_dwords[6], const uint8_t* base,
                                          uint64_t frame, char* source, bool is_signed) {
  Impl& m = *impl_;
  char unused;
  char& src_kind = source ? *source : unused;
  src_kind = '-';
  xenos::xe_gpu_texture_fetch_t fetch;
  std::memcpy(&fetch, fetch_dwords, sizeof(fetch));
  if (fetch.type != xenos::FetchConstantType::kTexture) return nullptr;
  rg::TextureInfo info;
  if (!rg::TextureInfo::Prepare(fetch, &info) || info.dimension != xenos::DataDimension::k3D) {
    return nullptr;
  }
  HostFormat host;
  if (!GetHostFormat(info.format, host) ||
      info.format_info()->bytes_per_block() != host.bytes_per_block ||
      info.format_info()->block_width != 1) {
    if (m.reported_formats.insert(0x100 | uint32_t(info.format)).second) {
      REXLOG_INFO("NATIVE-TEX unsupported 3D guest format {}", uint32_t(info.format));
    }
    return nullptr;
  }
  uint32_t width = info.width + 1, height = info.height + 1, depth = info.depth + 1;
  const auto& extent = info.extent;
  uint32_t pitch = extent.block_pitch_h, pitch_v = extent.block_pitch_v;
  uint32_t address = info.memory.base_address;
  uint32_t size = pitch * pitch_v * depth * host.bytes_per_block;
  if (!size || address >= kSharedMemorySize || size > kSharedMemorySize - address) return nullptr;
  const uint8_t* src = base + kPhysicalMirror + address;
  uint32_t swizzle = ComposeSwizzle(fetch.swizzle, info.format);

  uint64_t key = XXH3_64bits(fetch_dwords, 6 * sizeof(uint32_t)) ^ 0x3D3D3D3Dull;
  GuestTextureEntry& entry = m.textures[key];
  src_kind = 'U';
  if (is_signed && host.snorm == dl::TEX_FORMAT_UNKNOWN) {
    src_kind = '0';
    return nullptr;
  }
  dl::TEXTURE_FORMAT view_format = is_signed ? host.snorm : host.format;
  if (entry.texture && entry.checked_frame == frame) {
    return m.View(entry.texture, swizzle, dl::RESOURCE_DIM_TEX_3D, view_format);
  }
  uint64_t hash = Impl::SampleHash(src, size);
  entry.checked_frame = frame;
  if (entry.texture && entry.content_hash == hash) {
    return m.View(entry.texture, swizzle, dl::RESOURCE_DIM_TEX_3D, view_format);
  }
  entry.content_hash = hash;

  uint32_t bpb = host.bytes_per_block;
  uint32_t bpb_log2 = bpb >= 16 ? 4 : bpb >= 8 ? 3 : bpb >= 4 ? 2 : bpb >= 2 ? 1 : 0;
  m.staging.assign(size_t(width) * height * depth * bpb, 0);
  for (uint32_t z = 0; z < depth; ++z) {
    for (uint32_t y = 0; y < height; ++y) {
      for (uint32_t x = 0; x < width; ++x) {
        size_t in = info.is_tiled
                        ? size_t(rg::texture_util::GetTiledOffset3D(int32_t(x), int32_t(y),
                                                                    int32_t(z), pitch, pitch_v,
                                                                    bpb_log2))
                        : ((size_t(z) * pitch_v + y) * pitch + x) * bpb;
        if (in + bpb > size) continue;
        rg::texture_conversion::CopySwapBlock(
            info.endianness, &m.staging[((size_t(z) * height + y) * width + x) * bpb], src + in,
            bpb);
      }
    }
  }
  dl::TextureSubResData subresource{m.staging.data(), uint64_t(width) * bpb};
  subresource.DepthStride = uint64_t(width) * height * bpb;
  if (!entry.texture) {
    dl::TextureDesc desc;
    desc.Name = "GuestTexture3D";
    desc.Type = dl::RESOURCE_DIM_TEX_3D;
    desc.Width = width;
    desc.Height = height;
    desc.Depth = depth;
    desc.MipLevels = 1;
    desc.Format = host.typeless != dl::TEX_FORMAT_UNKNOWN ? host.typeless : host.format;
    desc.BindFlags = dl::BIND_SHADER_RESOURCE;
    desc.Usage = dl::USAGE_DEFAULT;
    dl::TextureData data{&subresource, 1};
    m.device->CreateTexture(desc, &data, &entry.texture);
    if (!entry.texture) {
      m.textures.erase(key);
      return nullptr;
    }
    REXLOG_INFO("NATIVE-TEX 3D texture {:08X} {}x{}x{} fmt={} tiled={}", address, width, height,
                depth, uint32_t(info.format), info.is_tiled);
  } else {
    dl::Box box(0, width, 0, height, 0, depth);
    m.context->UpdateTexture(entry.texture, 0, 0, box, subresource,
                             dl::RESOURCE_STATE_TRANSITION_MODE_TRANSITION,
                             dl::RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
  }
  ++m.uploads;
  m.uploaded_bytes += m.staging.size();
  return m.View(entry.texture, swizzle, dl::RESOURCE_DIM_TEX_3D, view_format);
}

dl::ISampler* TextureCache::GetSampler(const uint32_t fetch_dwords[6], uint32_t mag_filter,
                                       uint32_t min_filter, uint32_t mip_filter,
                                       uint32_t aniso_filter) {
  Impl& m = *impl_;
  xenos::xe_gpu_texture_fetch_t fetch;
  std::memcpy(&fetch, fetch_dwords, sizeof(fetch));
  constexpr uint32_t kUseFetch = uint32_t(xenos::TextureFilter::kUseFetchConst);
  if (mag_filter == kUseFetch) mag_filter = uint32_t(fetch.mag_filter);
  if (min_filter == kUseFetch) min_filter = uint32_t(fetch.min_filter);
  if (mip_filter == kUseFetch) mip_filter = uint32_t(fetch.mip_filter);
  if (aniso_filter == uint32_t(xenos::AnisoFilter::kUseFetchConst)) {
    aniso_filter = uint32_t(fetch.aniso_filter);
  }
  xenos::ClampMode clamp_x, clamp_y, clamp_z;
  rg::texture_util::GetClampModesForDimension(fetch, clamp_x, clamp_y, clamp_z);
  uint32_t mip_min = fetch.mip_min_level, mip_max = std::max(fetch.mip_min_level, fetch.mip_max_level);
  if (mip_filter == uint32_t(xenos::TextureFilter::kBaseMap)) mip_min = mip_max = 0;
  uint64_t key = mag_filter | (min_filter << 2) | (mip_filter << 4) | (aniso_filter << 6) |
                 (uint32_t(clamp_x) << 9) | (uint32_t(clamp_y) << 12) | (uint32_t(clamp_z) << 15) |
                 (uint64_t(mip_min) << 18) | (uint64_t(mip_max) << 22);
  auto it = m.samplers.find(key);
  if (it != m.samplers.end()) return it->second;

  auto filter = [](uint32_t f) {
    return f == uint32_t(xenos::TextureFilter::kLinear) ? dl::FILTER_TYPE_LINEAR
                                                        : dl::FILTER_TYPE_POINT;
  };
  dl::SamplerDesc desc;
  desc.MagFilter = filter(mag_filter);
  desc.MinFilter = filter(min_filter);
  desc.MipFilter = filter(mip_filter);
  if (aniso_filter != uint32_t(xenos::AnisoFilter::kDisabled)) {
    desc.MinFilter = desc.MagFilter = desc.MipFilter = dl::FILTER_TYPE_ANISOTROPIC;
    desc.MaxAnisotropy = 1u << (std::min(aniso_filter, 5u) - 1);
  }
  desc.MinLOD = float(mip_min);
  desc.MaxLOD = float(mip_max);
  desc.AddressU = AddressMode(clamp_x);
  desc.AddressV = AddressMode(clamp_y);
  desc.AddressW = AddressMode(clamp_z);
  dl::RefCntAutoPtr<dl::ISampler> sampler;
  m.device->CreateSampler(desc, &sampler);
  m.samplers.emplace(key, sampler);
  return sampler;
}

void TextureCache::LogStats() {
  REXLOG_INFO("NATIVE-TEX textures={} uploads={} uploaded={} KB resolved_hits={} unsupported={} "
              "views={} samplers={}",
              impl_->textures.size(), impl_->uploads, impl_->uploaded_bytes / 1024,
              impl_->resolved_hits, impl_->unsupported, impl_->views.size(),
              impl_->samplers.size());
}

}  // namespace native
