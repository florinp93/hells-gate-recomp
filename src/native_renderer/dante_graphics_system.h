#pragma once

#include <memory>
#include <string>

#include <rex/system/interfaces/graphics.h>

namespace dante {

// IGraphicsSystem wrapper that forwards every call to an inner system.
// Proves the RuntimeConfig::graphics injection seam: when installed via
// OnPreSetup, ReXApp skips its own LoadGpuPlugin and uses this object.
// Later steps replace the inner xenos plugin with a project-owned system.
class DanteGraphicsSystem : public rex::system::IGraphicsSystem {
 public:
  explicit DanteGraphicsSystem(
      std::unique_ptr<rex::system::IGraphicsSystem> inner);
  ~DanteGraphicsSystem() override = default;

  rex::system::IGraphicsSystem* inner() const { return inner_.get(); }

  rex::X_STATUS SetupPresentation(
      rex::ui::WindowedAppContext* app_context) override;
  rex::X_STATUS SetupGuestGpu(
      rex::runtime::FunctionDispatcher* function_dispatcher,
      rex::system::KernelState* kernel_state) override;
  bool has_presentation() const override;
  rex::ui::GraphicsProvider* provider() const override;
  rex::ui::Presenter* presenter() const override;
  void SetInterruptCallback(uint32_t callback, uint32_t user_data) override;
  void InitializeRingBuffer(uint32_t ptr, uint32_t size_log2) override;
  void EnableReadPointerWriteBack(uint32_t ptr,
                                  uint32_t block_size_log2) override;
  void InitializeShaderStorage(const std::filesystem::path& cache_root,
                               uint32_t title_id, bool blocking) override;
  void Shutdown() override;

 private:
  std::unique_ptr<rex::system::IGraphicsSystem> inner_;
};

// Builds the graphics system selected by the `renderer` cvar:
//   "xenos"   -> nullptr (ReXApp loads the rexgpu-xenos plugin as usual)
//   "native"  -> DanteGraphicsSystem wrapping an in-process
//                rex::graphics::vulkan::VulkanGraphicsSystem
//   "wrapped" -> DanteGraphicsSystem wrapping the configured gpu_plugin
// nullptr means "fall back to the default plugin path".
std::unique_ptr<rex::system::IGraphicsSystem> CreateConfiguredGraphicsSystem(
    const std::string& gpu_plugin);

// Applies the `native_render_scale` cvar to the SDK `resolution_scale` cvar.
// Must run before SetupGuestGpu (which captures the scale). Works with any
// graphics backend since it only adjusts the shared cvar.
void ApplyRenderScaleConfig();

// renderer=dante: the Xenos command processor only keeps the game's sync
// (fences, interrupts, swaps); rendering is native. Call after the GPU plugin
// is loaded (its cvars register on load).
void ApplyDanteRendererConfig();

}  // namespace dante
