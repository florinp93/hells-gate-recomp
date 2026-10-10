#pragma once

#include <memory>

namespace Diligent {
struct ITexture;
}

namespace dante {
class NativeDevice;
}

namespace native {

// SMAA 1x (Jimenez et al.) applied in place to a color render target: luma
// edge detection, blending weights, neighborhood blending.
class SmaaPass {
 public:
  explicit SmaaPass(dante::NativeDevice* device);
  ~SmaaPass();

  // Anti-aliases `color` (render target + shader resource). False when the
  // pass could not be created; the texture is then left unchanged.
  bool Apply(Diligent::ITexture* color);

  // Edge mask of the last Apply (diagnostics).
  Diligent::ITexture* Edges() const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace native
