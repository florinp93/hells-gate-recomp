// Non-native-renderer builds (Android): the SDK's Xenos backend (rexgpu-xenos)
// renders, so the renderer=dante entry points the app references are no-ops.
// DantesInfernoApp only calls them when --renderer=dante, which is never set here.

#include "renderer/renderer.h"
#include "renderer/ui_overlay.h"

namespace renderer {

void ConfigureSyncOnlyGpu() {}
bool Start(rex::ui::Window*, const std::filesystem::path&) { return false; }
PrecompileStatus GetPrecompileStatus() { return {}; }
bool RunWhenPrecompiled(std::function<void()>) { return false; }
void Stop() {}
void SetOverlayFrameRequest(std::function<void()>) {}
void SetButtonGlyphStyleSource(std::function<GlyphStyle()>) {}
void SetButtonGlyphBindingsSource(std::function<KeyBindings()>) {}
PresentCount PresentedFrames() { return {}; }

std::unique_ptr<rex::ui::ImmediateTexture> OverlayDrawer::CreateTexture(
    uint32_t, uint32_t, rex::ui::ImmediateTextureFilter, bool, const uint8_t*) {
  return nullptr;
}
void OverlayDrawer::Begin(rex::ui::UIDrawContext&, float, float) {}
void OverlayDrawer::BeginDrawBatch(const rex::ui::ImmediateDrawBatch&) {}
void OverlayDrawer::Draw(const rex::ui::ImmediateDraw&) {}
void OverlayDrawer::End() {}
void OverlayDrawer::PublishEmpty() {}

}  // namespace renderer
