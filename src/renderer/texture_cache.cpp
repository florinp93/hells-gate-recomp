#include "texture_cache.h"

#include "guest_memory.h"
#include "swizzle.h"

#include "Common/interface/RefCntAutoPtr.hpp"
#include "Graphics/GraphicsEngine/interface/DeviceContext.h"
#include "Graphics/GraphicsEngine/interface/RenderDevice.h"
#include "Graphics/GraphicsEngine/interface/Sampler.h"
#include "Graphics/GraphicsEngine/interface/Texture.h"
#include "Graphics/GraphicsEngine/interface/TextureView.h"
#include "Primitives/interface/ReferenceCounters.h"

#include <rex/graphics/pipeline/texture/conversion.h>
#include <rex/graphics/pipeline/texture/info.h>
#include <rex/graphics/pipeline/texture/util.h>
#include <rex/cvar.h>
#include <rex/graphics/xenos.h>
#include <rex/logging/macros.h>

#define XXH_STATIC_LINKING_ONLY  // XXH3_state_t on the stack
#include <xxhash.h>

#include <fmt/format.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <exception>
#include <new>
#include <set>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace renderer {
namespace {

namespace dl = Diligent;
namespace rg = rex::graphics;
namespace xenos = rex::graphics::xenos;

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
      out = {dl::TEX_FORMAT_RGBA8_UNORM, 4, dl::TEX_FORMAT_RGBA8_TYPELESS,
             dl::TEX_FORMAT_RGBA8_SNORM};
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
    case xenos::TextureFormat::k_DXN: out = {dl::TEX_FORMAT_BC5_UNORM, 16}; return true;
    case xenos::TextureFormat::k_16:
      out = {dl::TEX_FORMAT_R16_UNORM, 2, dl::TEX_FORMAT_R16_TYPELESS, dl::TEX_FORMAT_R16_SNORM};
      return true;
    case xenos::TextureFormat::k_16_16:
      out = {dl::TEX_FORMAT_RG16_UNORM, 4, dl::TEX_FORMAT_RG16_TYPELESS, dl::TEX_FORMAT_RG16_SNORM};
      return true;
    case xenos::TextureFormat::k_16_16_16_16:
      out = {dl::TEX_FORMAT_RGBA16_UNORM, 8, dl::TEX_FORMAT_RGBA16_TYPELESS,
             dl::TEX_FORMAT_RGBA16_SNORM};
      return true;
    case xenos::TextureFormat::k_16_FLOAT: out = {dl::TEX_FORMAT_R16_FLOAT, 2}; return true;
    case xenos::TextureFormat::k_16_16_FLOAT: out = {dl::TEX_FORMAT_RG16_FLOAT, 4}; return true;
    case xenos::TextureFormat::k_16_16_16_16_FLOAT:
      out = {dl::TEX_FORMAT_RGBA16_FLOAT, 8};
      return true;
    case xenos::TextureFormat::k_32_FLOAT: out = {dl::TEX_FORMAT_R32_FLOAT, 4}; return true;
    case xenos::TextureFormat::k_32_32_FLOAT: out = {dl::TEX_FORMAT_RG32_FLOAT, 8}; return true;
    case xenos::TextureFormat::k_32_32_32_32_FLOAT:
      out = {dl::TEX_FORMAT_RGBA32_FLOAT, 16};
      return true;
    default: return false;
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
    case xenos::TextureFormat::k_24_8_FLOAT: return {0, 0, 0, 0};
    case xenos::TextureFormat::k_8_8:
    case xenos::TextureFormat::k_16_16:
    case xenos::TextureFormat::k_16_16_FLOAT:
    case xenos::TextureFormat::k_32_32_FLOAT:
    case xenos::TextureFormat::k_DXN: return {0, 1, 1, 1};
    default: return {0, 1, 2, 3};
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
  uint64_t bytes = 0;
};

constexpr uint64_t kUnusedFrames = 1800;

// Cheap change detection: the first 64 bytes of every 4 KB page.
void HashSamples(XXH3_state_t& state, const uint8_t* data, uint32_t size) {
  for (uint32_t offset = 0; offset < size; offset += 4096) {
    XXH3_64bits_update(&state, data + offset, std::min<uint32_t>(64, size - offset));
  }
}

inline void SwapBlock(xenos::Endian endian, uint8_t* dst, const uint8_t* src, uint32_t bytes) {
  switch (endian) {
    case xenos::Endian::k8in16:
      for (uint32_t i = 0; i < bytes; i += 2) {
        uint16_t v;
        std::memcpy(&v, src + i, 2);
        v = __builtin_bswap16(v);
        std::memcpy(dst + i, &v, 2);
      }
      break;
    case xenos::Endian::k8in32:
      for (uint32_t i = 0; i < bytes; i += 4) {
        uint32_t v;
        std::memcpy(&v, src + i, 4);
        v = __builtin_bswap32(v);
        std::memcpy(dst + i, &v, 4);
      }
      break;
    case xenos::Endian::k16in32:
      for (uint32_t i = 0; i < bytes; i += 4) {
        uint32_t v;
        std::memcpy(&v, src + i, 4);
        v = (v >> 16) | (v << 16);
        std::memcpy(dst + i, &v, 4);
      }
      break;
    default:
      std::memcpy(dst, src, bytes);
      break;
  }
}

uint64_t SampleHash(const uint8_t* data, uint32_t size) {
  XXH3_state_t state;
  XXH3_64bits_reset(&state);
  HashSamples(state, data, size);
  return XXH3_64bits_digest(&state);
}

uint32_t Log2BytesPerBlock(uint32_t bpb) {
  return bpb >= 16 ? 4 : bpb >= 8 ? 3 : bpb >= 4 ? 2 : bpb >= 2 ? 1 : 0;
}

constexpr uint64_t kMaxTextureBytes = 256ull << 20;
constexpr uint32_t kFailuresLogged = 16;

bool FitsPhysical(uint32_t address, uint64_t size) {
  return size <= guest::kPhysicalMemorySize && guest::IsPhysicalRange(address, uint32_t(size));
}

struct ViewKey {
  dl::ITexture* texture;
  uint32_t swizzle;
  dl::TEXTURE_FORMAT format;
  dl::RESOURCE_DIMENSION dimension;
  bool operator==(const ViewKey&) const = default;
};
struct ViewKeyHash {
  size_t operator()(const ViewKey& k) const {
    const uint64_t fields[3] = {uint64_t(reinterpret_cast<uintptr_t>(k.texture)), k.swizzle,
                                (uint64_t(k.format) << 8) | uint64_t(k.dimension)};
    return size_t(XXH3_64bits(fields, sizeof(fields)));
  }
};

}  // namespace

struct TextureCache::Impl {
  dl::IRenderDevice* device;
  dl::IDeviceContext* context;
  ResolveLookup resolve_lookup;
  Replacer replacer;
  // Key: XXH3 of the fetch dwords identifying the texture data.
  std::unordered_map<uint64_t, GuestTextureEntry> textures;
  std::unordered_map<ViewKey, dl::RefCntAutoPtr<dl::ITextureView>, ViewKeyHash> views;
  std::unordered_map<uint64_t, dl::RefCntAutoPtr<dl::ISampler>> samplers;
  std::set<uint32_t> reported_formats;
  std::vector<uint8_t> staging;
  uint32_t uploads = 0, resolved_hits = 0, unsupported = 0;
  uint64_t uploaded_bytes = 0;
  uint64_t last_frame = 0;
  uint64_t evicted = 0;
  uint64_t evicted_views = 0;
  uint32_t failures = 0;

  dl::ITextureView* View2D(const uint32_t fetch_dwords[6], const uint8_t* base, uint64_t frame,
                           char* source, bool is_signed);
  dl::ITextureView* ViewCube(const uint32_t fetch_dwords[6], const uint8_t* base, uint64_t frame,
                             char* source, bool is_signed);
  dl::ITextureView* View3D(const uint32_t fetch_dwords[6], const uint8_t* base, uint64_t frame,
                           char* source, bool is_signed);

  void ReportFailure(const char* kind, const char* what, uint64_t bytes) {
    if (++failures > kFailuresLogged) return;
    REXLOG_WARN("NATIVE-TEX {} texture failed: {} ({} KB, failure {}, textures={} views={})", kind,
                what, bytes >> 10, failures, textures.size(), views.size());
  }

  template <typename Body>
  dl::ITextureView* Guard(const char* kind, Body&& body) {
    for (uint32_t attempt = 0; attempt < 2; ++attempt) {
      try {
        return body();
      } catch (const std::bad_alloc&) {
        ReportFailure(kind, attempt ? "out of memory" : "out of memory, evicting and retrying", 0);
        try {
          Evict(last_frame, 1);
        } catch (...) {
          return nullptr;
        }
      } catch (const std::exception& e) {
        ReportFailure(kind, e.what(), 0);
        return nullptr;
      } catch (...) {
        ReportFailure(kind, "unknown exception", 0);
        return nullptr;
      }
    }
    return nullptr;
  }

  bool Allocate(std::vector<uint8_t>& data, size_t size, const char* kind) {
    if (size > kMaxTextureBytes) {
      ReportFailure(kind, "size out of range", size);
      return false;
    }
    for (uint32_t attempt = 0;; ++attempt) {
      try {
        data.assign(size, 0);
        return true;
      } catch (const std::bad_alloc&) {
        std::vector<uint8_t>().swap(data);
        if (attempt) {
          ReportFailure(kind, "host allocation", size);
          return false;
        }
        if (&data != &staging) std::vector<uint8_t>().swap(staging);
        Evict(last_frame, 1);
      }
    }
  }

  void ForgetTexture(dl::ITexture* texture) {
    if (!texture) return;
    for (auto it = views.begin(); it != views.end();) {
      it = it->first.texture == texture ? views.erase(it) : std::next(it);
    }
  }

  void Drop(uint64_t key) {
    auto it = textures.find(key);
    if (it == textures.end()) return;
    ForgetTexture(it->second.texture);
    textures.erase(it);
  }

  void DropOrphanViews() {
    struct Use {
      long views = 0;
      bool only_cached = true;
    };
    std::unordered_map<dl::ITexture*, Use> uses;
    for (const auto& [key, view] : views) {
      Use& u = uses[key.texture];
      ++u.views;
      if (view->GetReferenceCounters()->GetNumStrongRefs() != 1) u.only_cached = false;
    }
    std::unordered_set<dl::ITexture*> orphans;
    for (const auto& [texture, u] : uses) {
      if (u.only_cached && texture->GetReferenceCounters()->GetNumStrongRefs() == u.views) {
        orphans.insert(texture);
      }
    }
    if (orphans.empty()) return;
    for (auto it = views.begin(); it != views.end();) {
      it = orphans.count(it->first.texture) ? views.erase(it) : std::next(it);
    }
    evicted_views += orphans.size();
  }

  void Evict(uint64_t frame, uint64_t unused_frames) {
    std::unordered_set<dl::ITexture*> gone;
    for (auto it = textures.begin(); it != textures.end();) {
      const GuestTextureEntry& e = it->second;
      if (e.checked_frame != frame &&
          (e.checked_frame == UINT64_MAX || e.checked_frame + unused_frames <= frame)) {
        if (e.texture) gone.insert(e.texture.RawPtr());
        it = textures.erase(it);
      } else {
        ++it;
      }
    }
    if (gone.empty()) return;
    for (auto it = views.begin(); it != views.end();) {
      it = gone.count(it->second->GetTexture()) ? views.erase(it) : std::next(it);
    }
    evicted += gone.size();
  }

  void BeginUse(uint64_t frame) {
    if (frame == last_frame) return;
    last_frame = frame;
    if (frame % 60 == 0) {
      Evict(frame, kUnusedFrames);
      DropOrphanViews();
    }
  }

  // format: view format for typeless textures (UNKNOWN = the texture's own).
  dl::ITextureView* View(dl::ITexture* texture, uint32_t swizzle,
                         dl::RESOURCE_DIMENSION dimension = dl::RESOURCE_DIM_TEX_2D_ARRAY,
                         dl::TEXTURE_FORMAT format = dl::TEX_FORMAT_UNKNOWN) {
    if (!texture) return nullptr;
    ViewKey key{texture, swizzle, format, dimension};
    auto it = views.find(key);
    if (it != views.end()) return it->second;
    dl::TextureViewDesc desc;
    desc.ViewType = dl::TEXTURE_VIEW_SHADER_RESOURCE;
    desc.TextureDim = dimension;
    desc.Format = format;
    desc.Swizzle = ComponentMapping(swizzle);
    dl::RefCntAutoPtr<dl::ITextureView> view;
    texture->CreateView(desc, &view);
    if (!view) return nullptr;
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
    uint32_t bpb_log2 = Log2BytesPerBlock(bpb);
    uint32_t width = width_m1 + 1, height = height_m1 + 1, array_size = depth_m1 + 1;
    uint32_t max_level = mip_page ? mip_max : 0;
    auto dimension = xenos::DataDimension(fetch.dimension);
    auto layout = rg::texture_util::GetGuestTextureLayout(dimension, fetch.pitch, width, height,
                                                          array_size, fetch.tiled, format,
                                                          fetch.packed_mips, true, max_level);
    uint32_t base_address = base_page << 12, mip_address = mip_page << 12;

    // Content hash: sampled over the base and mip data.
    {
      XXH3_state_t state;
      XXH3_64bits_reset(&state);
      auto sample = [&](uint32_t address, uint32_t size) {
        if (!size || address >= guest::kPhysicalMemorySize) return;
        size = std::min(size, guest::kPhysicalMemorySize - address);
        HashSamples(state, guest::PhysicalPtr(base, address), size);
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
    if (!Allocate(out.data, total, "2D/cube")) return false;
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
        auto endian = xenos::Endian(fetch.endianness);
        for (uint32_t y = 0; y < blocks_y; ++y) {
          if (!fetch.tiled) {
            uint32_t row = address + (y + oy) * pitch_bytes + ox * bpb;
            uint32_t row_bytes = blocks_x * bpb;
            if (row < guest::kPhysicalMemorySize &&
                guest::kPhysicalMemorySize - row >= row_bytes) {
              rg::texture_conversion::CopySwapBlock(endian, dst + size_t(y) * row_bytes,
                                                    guest::PhysicalPtr(base, row), row_bytes);
              continue;
            }
          }
          for (uint32_t x = 0; x < blocks_x; ++x) {
            uint32_t in = fetch.tiled
                              ? uint32_t(rg::texture_util::GetTiledOffset2D(
                                    int32_t(x + ox), int32_t(y + oy), pitch_bytes / bpb, bpb_log2))
                              : (y + oy) * pitch_bytes + (x + ox) * bpb;
            uint32_t source = address + in;
            if (source >= guest::kPhysicalMemorySize || guest::kPhysicalMemorySize - source < bpb)
              continue;
            SwapBlock(endian, dst + (size_t(y) * blocks_x + x) * bpb,
                      guest::PhysicalPtr(base, source), bpb);
          }
        }
        out.subresources.push_back(dl::TextureSubResData{dst, uint64_t(blocks_x) * bpb});
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
      if (!entry.texture) {
        Evict(last_frame, 1);
        device->CreateTexture(desc, &data, &entry.texture);
      }
      if (!entry.texture) {
        ReportFailure(name, "device allocation", levels.data.size());
        return false;
      }
      entry.bytes = levels.data.size();
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

void TextureCache::SetReplacer(Replacer replacer) { impl_->replacer = std::move(replacer); }

dl::ITextureView* TextureCache::GetView2D(const uint32_t fetch_dwords[6], const uint8_t* base,
                                          uint64_t frame, char* source, bool is_signed) {
  return impl_->Guard("2D",
                      [&] { return impl_->View2D(fetch_dwords, base, frame, source, is_signed); });
}

dl::ITextureView* TextureCache::GetViewCube(const uint32_t fetch_dwords[6], const uint8_t* base,
                                            uint64_t frame, char* source, bool is_signed) {
  return impl_->Guard("cube",
                      [&] { return impl_->ViewCube(fetch_dwords, base, frame, source, is_signed); });
}

dl::ITextureView* TextureCache::GetView3D(const uint32_t fetch_dwords[6], const uint8_t* base,
                                          uint64_t frame, char* source, bool is_signed) {
  return impl_->Guard("3D",
                      [&] { return impl_->View3D(fetch_dwords, base, frame, source, is_signed); });
}

void TextureCache::ForgetTexture(dl::ITexture* texture) { impl_->ForgetTexture(texture); }

dl::ITextureView* TextureCache::Impl::View2D(const uint32_t fetch_dwords[6], const uint8_t* base,
                                             uint64_t frame, char* source, bool is_signed) {
  Impl& m = *this;
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
    if (rb_swap) swizzle = SwapRedBlueSwizzle(swizzle);
    return m.View(resolved, swizzle);
  }

  HostFormat host;
  if (!GetHostFormat(info.format, host) ||
      info.format_info()->bytes_per_block() != host.bytes_per_block) {
    if (m.reported_formats.insert(uint32_t(info.format)).second) {
      REXLOG_INFO("NATIVE-TEX unsupported guest format {} ({}x{})", uint32_t(info.format), width,
                  height);
    }
    ++m.unsupported;
    return nullptr;
  }
  uint64_t size = uint64_t(info.extent.block_pitch_h) * info.extent.block_pitch_v *
                  info.extent.depth * host.bytes_per_block;
  if (!FitsPhysical(address, size)) return nullptr;

  // The texture is identified by its address, format, size and mip setup.
  // (dword 0: pitch and tiling; 1: base and format; 2: size; 5: mip address).
  uint32_t identity[5] = {fetch_dwords[0] & 0xFFC00000u, fetch_dwords[1], fetch_dwords[2],
                          fetch_dwords[5],
                          (uint32_t(fetch.mip_min_level) << 4) | uint32_t(fetch.mip_max_level)};
  uint64_t key = XXH3_64bits(identity, sizeof(identity));
  m.BeginUse(frame);
  GuestTextureEntry& entry = m.textures[key];
  src_kind = 'U';
  if (is_signed && host.snorm == dl::TEX_FORMAT_UNKNOWN) {
    src_kind = '0';  // no signed host format: the fetch reads zero
    return nullptr;
  }
  dl::TEXTURE_FORMAT view_format = is_signed ? host.snorm : host.format;
  auto view = [&](const LoadedTexture* loaded) {
    if (m.replacer && !is_signed) {
      if (dl::ITexture* replacement = m.replacer(entry.content_hash, loaded)) {
        return m.View(replacement, swizzle, dl::RESOURCE_DIM_TEX_2D_ARRAY);
      }
    }
    return m.View(entry.texture, swizzle, dl::RESOURCE_DIM_TEX_2D_ARRAY, view_format);
  };
  if (entry.texture && entry.checked_frame == frame) return view(nullptr);
  entry.checked_frame = frame;
  Impl::LoadedLevels levels;
  uint64_t hash = 0;
  if (!m.LoadLevels(fetch, host, base, levels, hash, true)) {
    m.Drop(key);
    return nullptr;
  }
  if (entry.texture && entry.content_hash == hash) return view(nullptr);
  if (!m.LoadLevels(fetch, host, base, levels, hash)) {
    if (!entry.texture) m.textures.erase(key);
    return nullptr;
  }
  if (!m.StoreLevels(entry, levels, host, dl::RESOURCE_DIM_TEX_2D_ARRAY, "GuestTexture")) {
    m.Drop(key);
    return nullptr;
  }
  entry.content_hash = hash;
  LoadedTexture loaded{levels.width, levels.height, uint32_t(info.format), levels.levels,
                       levels.subresources.empty()
                           ? nullptr
                           : static_cast<const uint8_t*>(levels.subresources[0].pData)};
  return view(&loaded);
}

dl::ITextureView* TextureCache::Impl::ViewCube(const uint32_t fetch_dwords[6], const uint8_t* base,
                                               uint64_t frame, char* source, bool is_signed) {
  Impl& m = *this;
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
  uint64_t face_stride =
      (uint64_t(extent.block_pitch_h) * extent.block_pitch_v * host.bytes_per_block + 4095) &
      ~uint64_t(4095);
  uint32_t address = info.memory.base_address;
  if (!FitsPhysical(address, face_stride * 6)) return nullptr;
  uint32_t swizzle = ComposeSwizzle(fetch.swizzle, info.format);

  // Cube and 3D keys: the whole fetch constant, xored apart from each other.
  uint64_t key = XXH3_64bits(fetch_dwords, 6 * sizeof(uint32_t)) ^ 0xC0BEC0BEull;
  m.BeginUse(frame);
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
    m.Drop(key);
    return nullptr;
  }
  if (entry.texture && entry.content_hash == hash) {
    return m.View(entry.texture, swizzle, dl::RESOURCE_DIM_TEX_CUBE, view_format);
  }
  if (!m.LoadLevels(fetch, host, base, levels, hash) || levels.array_size != 6) {
    if (!entry.texture) m.textures.erase(key);
    return nullptr;
  }
  bool created = !entry.texture;
  if (!m.StoreLevels(entry, levels, host, dl::RESOURCE_DIM_TEX_CUBE, "GuestTextureCube")) {
    m.Drop(key);
    return nullptr;
  }
  entry.content_hash = hash;
  if (created) {
    REXLOG_INFO("NATIVE-TEX cube texture {:08X} {}x{} fmt={} levels={}", address, width, height,
                uint32_t(info.format), levels.levels);
  }
  return m.View(entry.texture, swizzle, dl::RESOURCE_DIM_TEX_CUBE, view_format);
}

dl::ITextureView* TextureCache::Impl::View3D(const uint32_t fetch_dwords[6], const uint8_t* base,
                                             uint64_t frame, char* source, bool is_signed) {
  Impl& m = *this;
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
  uint64_t guest_size = uint64_t(pitch) * pitch_v * depth * host.bytes_per_block;
  if (!FitsPhysical(address, guest_size)) return nullptr;
  uint32_t size = uint32_t(guest_size);
  const uint8_t* src = guest::PhysicalPtr(base, address);
  uint32_t swizzle = ComposeSwizzle(fetch.swizzle, info.format);

  uint64_t key = XXH3_64bits(fetch_dwords, 6 * sizeof(uint32_t)) ^ 0x3D3D3D3Dull;
  m.BeginUse(frame);
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
  uint64_t hash = SampleHash(src, size);
  entry.checked_frame = frame;
  if (entry.texture && entry.content_hash == hash) {
    return m.View(entry.texture, swizzle, dl::RESOURCE_DIM_TEX_3D, view_format);
  }
  uint32_t bpb = host.bytes_per_block;
  uint32_t bpb_log2 = Log2BytesPerBlock(bpb);
  if (!m.Allocate(m.staging, size_t(width) * height * depth * bpb, "3D")) {
    if (!entry.texture) m.textures.erase(key);
    return nullptr;
  }
  for (uint32_t z = 0; z < depth; ++z) {
    for (uint32_t y = 0; y < height; ++y) {
      for (uint32_t x = 0; x < width; ++x) {
        size_t in = info.is_tiled
                        ? size_t(rg::texture_util::GetTiledOffset3D(
                              int32_t(x), int32_t(y), int32_t(z), pitch, pitch_v, bpb_log2))
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
      m.Evict(m.last_frame, 1);
      m.device->CreateTexture(desc, &data, &entry.texture);
    }
    if (!entry.texture) {
      m.ReportFailure("3D", "device allocation", m.staging.size());
      m.textures.erase(key);
      return nullptr;
    }
    entry.bytes = m.staging.size();
    REXLOG_INFO("NATIVE-TEX 3D texture {:08X} {}x{}x{} fmt={} tiled={}", address, width, height,
                depth, uint32_t(info.format), info.is_tiled);
  } else {
    dl::Box box(0, width, 0, height, 0, depth);
    m.context->UpdateTexture(entry.texture, 0, 0, box, subresource,
                             dl::RESOURCE_STATE_TRANSITION_MODE_TRANSITION,
                             dl::RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
  }
  entry.content_hash = hash;
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
  uint32_t mip_min = fetch.mip_min_level,
           mip_max = std::max(fetch.mip_min_level, fetch.mip_max_level);
  bool base_map = mip_filter == uint32_t(xenos::TextureFilter::kBaseMap);
  // The anisotropic_override graphics option, applied as the SDK texture cache does.
  int32_t aniso_override = rex::cvar::Query<int32_t>("anisotropic_override");
  constexpr uint32_t kLinear = uint32_t(xenos::TextureFilter::kLinear);
  if (aniso_override >= 0 && aniso_override < 6 && mip_max > mip_min && !base_map &&
      mag_filter == kLinear && min_filter == kLinear &&
      (mip_filter == uint32_t(xenos::TextureFilter::kPoint) || mip_filter == kLinear)) {
    aniso_filter = uint32_t(aniso_override);
  }
  if (base_map) mip_min = mip_max = 0;
  bool border = xenos::ClampModeUsesBorder(clamp_x) || xenos::ClampModeUsesBorder(clamp_y) ||
                xenos::ClampModeUsesBorder(clamp_z);
  uint32_t border_color = border ? uint32_t(fetch.border_color) : 0;
  uint64_t key = mag_filter | (min_filter << 2) | (mip_filter << 4) | (aniso_filter << 6) |
                 (uint32_t(clamp_x) << 9) | (uint32_t(clamp_y) << 12) | (uint32_t(clamp_z) << 15) |
                 (uint64_t(mip_min) << 18) | (uint64_t(mip_max) << 22) |
                 (uint64_t(border_color) << 26);
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
  if (border_color == uint32_t(xenos::BorderColor::k_ABGR_White)) {
    desc.BorderColor[0] = desc.BorderColor[1] = desc.BorderColor[2] = desc.BorderColor[3] = 1.0f;
  }
  dl::RefCntAutoPtr<dl::ISampler> sampler;
  m.device->CreateSampler(desc, &sampler);
  if (!sampler) return nullptr;
  m.samplers.emplace(key, sampler);
  return sampler;
}

TextureCache::Counts TextureCache::GetCounts() const {
  Counts c;
  c.textures = impl_->textures.size();
  c.views = impl_->views.size();
  c.samplers = impl_->samplers.size();
  for (const auto& [key, entry] : impl_->textures) c.bytes += entry.bytes;
  return c;
}

void TextureCache::LogStats() {
  Counts c = GetCounts();
  REXLOG_INFO(
      "NATIVE-TEX textures={} ({} MB) evicted={} uploads={} uploaded={} KB resolved_hits={} "
      "unsupported={} views={} (orphans dropped {}) samplers={} failures={}",
      c.textures, c.bytes >> 20, impl_->evicted, impl_->uploads, impl_->uploaded_bytes / 1024,
      impl_->resolved_hits, impl_->unsupported, c.views, impl_->evicted_views, c.samplers,
      impl_->failures);
}

}  // namespace renderer
