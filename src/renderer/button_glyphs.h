// renderer=dante button glyphs: the game's Xbox button icon sheet and the HUD
// art with Xbox buttons (face button spheres, shoulder plates, the Fury bar's
// "LS RS", the execution prompt's 3D trigger button) are replaced by
// PlayStation or keyboard / mouse versions generated from them at runtime (no
// game art shipped). Keyboard versions show the current bindings and are
// redrawn when they change.

#pragma once

#include "texture_cache.h"

#include <cstdint>
#include <memory>
#include <string>

namespace Diligent {
struct IRenderDevice;
struct ITexture;
}  // namespace Diligent

namespace renderer {

enum class GlyphStyle { kXbox, kPlayStation4, kPlayStation5, kKeyboard };

// Keyboard / mouse bindings of the pad (the SDK's keybind_* values, e.g.
// "Space", "LMB", "Shift+Up", "Z,Tab").
struct KeyBindings {
  std::string a, b, x, y;
  std::string left_shoulder, right_shoulder, left_trigger, right_trigger;
  std::string back, start;
  std::string lstick_up, lstick_down, lstick_left, lstick_right, lstick_press;
  std::string rstick_up, rstick_down, rstick_left, rstick_right, rstick_press;
  bool mouse_look = true;  // the right stick is the mouse
  bool operator==(const KeyBindings&) const = default;
};

class ButtonGlyphs {
 public:
  explicit ButtonGlyphs(Diligent::IRenderDevice* device);
  ~ButtonGlyphs();

  // Style shown from now on (render thread, e.g. once per frame).
  void SetStyle(GlyphStyle style);
  // Bindings for the keyboard style; keyboard textures are rebuilt on change.
  void SetBindings(const KeyBindings& bindings);

  // TextureCache::Replacer.
  Diligent::ITexture* Replace(uint64_t content_hash, const TextureCache::LoadedTexture* loaded);

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace renderer
