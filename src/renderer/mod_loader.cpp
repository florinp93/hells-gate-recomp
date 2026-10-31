#include "mod_loader.h"

#include "Common/interface/RefCntAutoPtr.hpp"
#include "Graphics/GraphicsEngine/interface/RenderDevice.h"
#include "Graphics/GraphicsEngine/interface/Texture.h"

#include <rex/cvar.h>
#include <rex/graphics/pipeline/texture/info.h>
#include <rex/graphics/xenos.h>
#include <rex/logging/macros.h>

#include <xxhash.h>

#include <algorithm>
#include <charconv>
#include <cstring>
#include <fstream>
#include <unordered_map>
#include <unordered_set>
#include <vector>

REXCVAR_DEFINE_BOOL(mods_enabled, true, "Mods", "renderer=dante: load texture mods from mods/");
REXCVAR_DEFINE_BOOL(mod_capture, false, "Mods",
                    "renderer=dante: log the mod hash of every texture the game uploads");

namespace renderer {
namespace {

namespace dl = Diligent;
namespace rg = rex::graphics;
namespace xenos = rex::graphics::xenos;

struct DdsFormat {
  dl::TEXTURE_FORMAT format = dl::TEX_FORMAT_UNKNOWN;
  uint32_t block = 1;
  uint32_t bytes = 4;
};

DdsFormat FromFourCC(const char* fourcc) {
  if (!std::memcmp(fourcc, "DXT1", 4)) return {dl::TEX_FORMAT_BC1_UNORM, 4, 8};
  if (!std::memcmp(fourcc, "DXT3", 4)) return {dl::TEX_FORMAT_BC2_UNORM, 4, 16};
  if (!std::memcmp(fourcc, "DXT5", 4)) return {dl::TEX_FORMAT_BC3_UNORM, 4, 16};
  if (!std::memcmp(fourcc, "ATI1", 4) || !std::memcmp(fourcc, "BC4U", 4))
    return {dl::TEX_FORMAT_BC4_UNORM, 4, 8};
  if (!std::memcmp(fourcc, "ATI2", 4) || !std::memcmp(fourcc, "BC5U", 4))
    return {dl::TEX_FORMAT_BC5_UNORM, 4, 16};
  return {};
}

DdsFormat FromDxgi(uint32_t dxgi) {
  switch (dxgi) {
    case 28:
    case 29:
      return {dl::TEX_FORMAT_RGBA8_UNORM, 1, 4};
    case 87:
    case 91:
      return {dl::TEX_FORMAT_BGRA8_UNORM, 1, 4};
    case 71:
    case 72:
      return {dl::TEX_FORMAT_BC1_UNORM, 4, 8};
    case 74:
    case 75:
      return {dl::TEX_FORMAT_BC2_UNORM, 4, 16};
    case 77:
    case 78:
      return {dl::TEX_FORMAT_BC3_UNORM, 4, 16};
    case 80:
      return {dl::TEX_FORMAT_BC4_UNORM, 4, 8};
    case 83:
      return {dl::TEX_FORMAT_BC5_UNORM, 4, 16};
    case 98:
    case 99:
      return {dl::TEX_FORMAT_BC7_UNORM, 4, 16};
    default:
      return {};
  }
}

uint32_t Read32(const std::vector<uint8_t>& data, size_t offset) {
  uint32_t value;
  std::memcpy(&value, data.data() + offset, 4);
  return value;
}

dl::RefCntAutoPtr<dl::ITexture> LoadDds(dl::IRenderDevice* device,
                                        const std::filesystem::path& path) {
  std::ifstream file(path, std::ios::binary);
  std::vector<uint8_t> data((std::istreambuf_iterator<char>(file)),
                            std::istreambuf_iterator<char>());
  if (data.size() < 128 || std::memcmp(data.data(), "DDS ", 4)) return {};
  uint32_t height = Read32(data, 12), width = Read32(data, 16);
  uint32_t levels = std::max(Read32(data, 28), 1u);
  uint32_t pf_flags = Read32(data, 80);
  const char* fourcc = reinterpret_cast<const char*>(data.data() + 84);
  size_t offset = 128;
  DdsFormat format;
  if (pf_flags & 0x4) {
    if (!std::memcmp(fourcc, "DX10", 4)) {
      if (data.size() < 148) return {};
      format = FromDxgi(Read32(data, 128));
      offset = 148;
    } else {
      format = FromFourCC(fourcc);
    }
  } else if ((pf_flags & 0x40) && Read32(data, 88) == 32) {
    format = Read32(data, 92) == 0xFF ? DdsFormat{dl::TEX_FORMAT_RGBA8_UNORM, 1, 4}
                                       : DdsFormat{dl::TEX_FORMAT_BGRA8_UNORM, 1, 4};
  }
  if (format.format == dl::TEX_FORMAT_UNKNOWN || !width || !height) return {};
  uint32_t max_levels = 1;
  while ((std::max(width, height) >> max_levels) > 0) ++max_levels;
  levels = std::min(levels, max_levels);
  std::vector<dl::TextureSubResData> subresources;
  for (uint32_t level = 0; level < levels; ++level) {
    uint32_t lw = std::max(width >> level, 1u), lh = std::max(height >> level, 1u);
    uint64_t pitch = uint64_t((lw + format.block - 1) / format.block) * format.bytes;
    uint64_t size = pitch * ((lh + format.block - 1) / format.block);
    if (offset + size > data.size()) {
      levels = level;
      break;
    }
    subresources.push_back(dl::TextureSubResData{data.data() + offset, pitch});
    offset += size;
  }
  if (!levels) return {};
  dl::TextureDesc desc;
  desc.Name = "ModTexture";
  desc.Type = dl::RESOURCE_DIM_TEX_2D_ARRAY;
  desc.Width = width;
  desc.Height = height;
  desc.ArraySize = 1;
  desc.MipLevels = levels;
  desc.Format = format.format;
  desc.BindFlags = dl::BIND_SHADER_RESOURCE;
  desc.Usage = dl::USAGE_IMMUTABLE;
  dl::TextureData init{subresources.data(), uint32_t(subresources.size())};
  dl::RefCntAutoPtr<dl::ITexture> texture;
  device->CreateTexture(desc, &init, &texture);
  return texture;
}

bool ParseHash(const std::string& stem, uint64_t& hash) {
  if (stem.size() < 16) return false;
  const char* begin = stem.data() + stem.size() - 16;
  auto [end, ec] = std::from_chars(begin, begin + 16, hash, 16);
  return ec == std::errc() && end == begin + 16;
}

}  // namespace

struct ModLoader::Impl {
  dl::IRenderDevice* device = nullptr;
  bool capture = false;
  std::unordered_map<uint64_t, std::filesystem::path> files;
  std::unordered_map<uint64_t, dl::RefCntAutoPtr<dl::ITexture>> textures;
  std::unordered_map<uint64_t, uint64_t> by_content;
  std::unordered_set<uint64_t> captured;
  uint32_t replaced = 0;

  void Scan(const std::filesystem::path& mods_dir) {
    std::error_code ec;
    if (!std::filesystem::is_directory(mods_dir, ec)) return;
    std::vector<std::filesystem::path> mods;
    for (const auto& entry : std::filesystem::directory_iterator(mods_dir, ec)) {
      if (entry.is_directory()) mods.push_back(entry.path());
    }
    std::sort(mods.begin(), mods.end());
    for (const auto& mod : mods) {
      uint32_t count = 0;
      auto textures_dir = mod / "textures";
      if (!std::filesystem::is_directory(textures_dir, ec)) continue;
      for (const auto& entry : std::filesystem::recursive_directory_iterator(textures_dir, ec)) {
        if (!entry.is_regular_file()) continue;
        auto path = entry.path();
        auto extension = path.extension().string();
        std::transform(extension.begin(), extension.end(), extension.begin(), ::tolower);
        uint64_t hash;
        if (extension != ".dds" || !ParseHash(path.stem().string(), hash)) continue;
        files[hash] = path;
        ++count;
      }
      REXLOG_INFO("ModLoader: {} ({} textures)", mod.filename().string(), count);
    }
  }

  static uint64_t ModHash(const TextureCache::LoadedTexture& loaded) {
    auto format = xenos::TextureFormat(loaded.guest_format);
    const rg::FormatInfo* info = rg::FormatInfo::Get(format);
    uint32_t bx = (loaded.width + info->block_width - 1) / info->block_width;
    uint32_t by = (loaded.height + info->block_height - 1) / info->block_height;
    size_t size = size_t(bx) * by * info->bytes_per_block();
    uint64_t seed = uint64_t(loaded.width) | (uint64_t(loaded.height) << 16) |
                    (uint64_t(loaded.guest_format) << 32);
    return XXH3_64bits_withSeed(loaded.level0, size, seed);
  }

  dl::ITexture* Texture(uint64_t hash) {
    auto it = textures.find(hash);
    if (it != textures.end()) return it->second;
    auto file = files.find(hash);
    if (file == files.end()) return nullptr;
    auto texture = LoadDds(device, file->second);
    if (texture) {
      const auto& desc = texture->GetDesc();
      REXLOG_INFO("ModLoader: {:016X} -> {} ({}x{}, {} levels)", hash,
                  file->second.filename().string(), desc.Width, desc.Height, desc.MipLevels);
      ++replaced;
    } else {
      REXLOG_WARN("ModLoader: cannot load {}", file->second.string());
    }
    return textures.emplace(hash, texture).first->second;
  }
};

ModLoader::ModLoader(dl::IRenderDevice* device, const std::filesystem::path& mods_dir)
    : impl_(std::make_unique<Impl>()) {
  impl_->device = device;
  impl_->capture = REXCVAR_GET(mod_capture);
  if (REXCVAR_GET(mods_enabled)) impl_->Scan(mods_dir);
  REXLOG_INFO("ModLoader: {} replacement textures in {}{}", impl_->files.size(),
              mods_dir.string(), impl_->capture ? ", capture on" : "");
}

ModLoader::~ModLoader() = default;

bool ModLoader::Active() const { return impl_->capture || !impl_->files.empty(); }

dl::ITexture* ModLoader::Replace(uint64_t content_hash,
                                 const TextureCache::LoadedTexture* loaded) {
  Impl& m = *impl_;
  if (loaded && loaded->level0) {
    uint64_t hash = Impl::ModHash(*loaded);
    m.by_content[content_hash] = hash;
    if (m.capture && m.captured.insert(hash).second) {
      REXLOG_INFO("ModCapture: {:016X} {}x{} format {} levels {}", hash, loaded->width,
                  loaded->height, loaded->guest_format, loaded->levels);
    }
  }
  if (m.files.empty()) return nullptr;
  auto it = m.by_content.find(content_hash);
  if (it == m.by_content.end()) return nullptr;
  return m.Texture(it->second);
}

}  // namespace renderer
