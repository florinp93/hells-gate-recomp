// renderer=dante shader pipeline: Xenos ucode -> SPIR-V through the SDK's
// SpirvShaderTranslator -> DiligentCore shaders. Render thread only.

#pragma once

#include <cstdint>
#include <memory>

namespace Diligent {
struct IRenderDevice;
struct IShader;
}  // namespace Diligent

namespace rex::graphics {
class RegisterFile;
class SpirvShader;
}  // namespace rex::graphics

namespace native {

struct GuestShaderCode {
  const uint32_t* ucode = nullptr;  // host pointer to big-endian guest ucode
  uint32_t dwords = 0;
};

struct TranslatedShaders {
  Diligent::IShader* vertex = nullptr;
  Diligent::IShader* pixel = nullptr;  // null when the draw has no pixel shader
  // Analyzed guest shaders (constant maps, vertex/texture bindings).
  const rex::graphics::SpirvShader* vertex_info = nullptr;
  const rex::graphics::SpirvShader* pixel_info = nullptr;
  uint64_t vertex_key = 0;             // ucode hash ^ modification, for pipeline keys
  uint64_t pixel_key = 0;
};

class ShaderCache {
 public:
  explicit ShaderCache(Diligent::IRenderDevice* device);
  ~ShaderCache();

  // Translates (cached) the shaders bound for a draw with the given state.
  bool Prepare(const rex::graphics::RegisterFile& regs, const GuestShaderCode& vs,
               const GuestShaderCode& ps, TranslatedShaders& out);

  // One NATIVE-SH summary line.
  void LogStats();

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace native
