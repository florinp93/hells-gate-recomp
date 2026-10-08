// renderer=dante textures: Xenos texture fetch constants -> DiligentCore views
// and samplers. Resolve outputs are used directly; other textures are untiled
// from guest memory and re-uploaded when their contents change. Render thread only.

#pragma once

#include <cstdint>
#include <functional>
#include <memory>

namespace Diligent {
struct IRenderDevice;
struct IDeviceContext;
struct ITexture;
struct ITextureView;
struct ISampler;
}  // namespace Diligent

namespace native {

class TextureCache {
 public:
  // Native texture written by Resolve for (guest address, width, height, guest
  // format); rb_swap: guest memory has red and blue exchanged relative to it.
  using ResolveLookup =
      std::function<Diligent::ITexture*(uint32_t address, uint32_t width, uint32_t height,
                                        uint32_t guest_format, bool& rb_swap)>;

  TextureCache(Diligent::IRenderDevice* device, Diligent::IDeviceContext* context,
               ResolveLookup resolve_lookup);
  ~TextureCache();

  // fetch: the 6 fetch constant dwords. Returns a 2D-array view (with the
  // fetch swizzle) or null when unsupported (the caller binds a dummy).
  // source (optional): 'R' resolve output, 'U' uploaded guest texture,
  // '-' unsupported / not a 2D texture.
  // is_signed: the view for the translator's signed binding (SNORM), null if
  // the format has no signed host format (the fetch then reads zero).
  Diligent::ITextureView* GetView2D(const uint32_t fetch[6], const uint8_t* base, uint64_t frame,
                                    char* source = nullptr, bool is_signed = false);

  // 3D textures (e.g. color grading lookup tables): a 3D view, or null.
  Diligent::ITextureView* GetView3D(const uint32_t fetch[6], const uint8_t* base, uint64_t frame,
                                    char* source = nullptr, bool is_signed = false);

  // Cube textures (6 faces, base level): a cube view, or null.
  Diligent::ITextureView* GetViewCube(const uint32_t fetch[6], const uint8_t* base, uint64_t frame,
                                      char* source = nullptr, bool is_signed = false);

  // filters: SamplerBinding mag/min/mip filter and aniso (kUseFetchConst = take
  // them from the fetch constant).
  Diligent::ISampler* GetSampler(const uint32_t fetch[6], uint32_t mag_filter, uint32_t min_filter,
                                 uint32_t mip_filter, uint32_t aniso_filter);

  void LogStats();


 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace native
