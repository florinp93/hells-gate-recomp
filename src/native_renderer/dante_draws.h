// renderer=dante draw submission: binds the translated Xenos shaders to
// DiligentCore with the translator's resource contract (shared memory,
// constant buffers, textures) and issues the draw into the host RTs.
// Render thread only.

#pragma once

#include <cstdint>
#include <memory>

namespace Diligent {
struct IRenderDevice;
struct IDeviceContext;
struct ITextureView;
struct IShader;
}  // namespace Diligent

namespace rex::graphics {
class RegisterFile;
class SpirvShader;
}  // namespace rex::graphics

namespace native {

class TextureCache;

struct DrawTargets {
  Diligent::ITextureView* color[4] = {};
  uint32_t color_format[4] = {};  // Diligent::TEXTURE_FORMAT
  uint32_t color_count = 0;
  Diligent::ITextureView* depth = nullptr;
  uint32_t depth_format = 0;
  uint32_t width = 0;
  uint32_t height = 0;
};

struct DrawShaders {
  Diligent::IShader* vertex = nullptr;
  Diligent::IShader* pixel = nullptr;
  const rex::graphics::SpirvShader* vertex_info = nullptr;
  const rex::graphics::SpirvShader* pixel_info = nullptr;
  uint64_t vertex_key = 0;
  uint64_t pixel_key = 0;
};

class DrawRenderer {
 public:
  DrawRenderer(Diligent::IRenderDevice* device, Diligent::IDeviceContext* context,
               TextureCache* textures);
  ~DrawRenderer();

  // regs must include VGT_DRAW_INITIATOR (and VGT_DMA_BASE/SIZE for indexed
  // draws). Returns false when the draw was skipped.
  // log: write one NATIVE-DRAW line describing the draw.
  // volatile_fetch: vertex fetch constant whose data may change within a frame
  // (inline vertices); other vertex data is checked once per frame.
  bool Draw(const rex::graphics::RegisterFile& regs, const DrawShaders& shaders,
            const DrawTargets& targets, const uint8_t* base, bool log,
            uint32_t volatile_fetch = ~0u);

  // Guest frame number (texture content checks run once per frame).
  void SetFrame(uint64_t frame);

  void LogStats();

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace native
