#pragma once

#include "texture_cache.h"

#include <cstdint>
#include <filesystem>
#include <memory>

namespace Diligent {
struct IRenderDevice;
struct ITexture;
}  // namespace Diligent

namespace renderer {

class ModLoader {
 public:
  ModLoader(Diligent::IRenderDevice* device, const std::filesystem::path& mods_dir);
  ~ModLoader();

  bool Active() const;
  Diligent::ITexture* Replace(uint64_t content_hash, const TextureCache::LoadedTexture* loaded);

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace renderer
