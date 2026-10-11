// Texture view swizzles as 12-bit values: 3 bits per output component (R, G,
// B, A), each 0-3 = source R..A, 4 = zero, 5 = one.

#pragma once

#include "Graphics/GraphicsEngine/interface/TextureView.h"

#include <cstdint>

namespace renderer {

inline Diligent::TEXTURE_COMPONENT_SWIZZLE ComponentSwizzle(uint32_t component) {
  switch (component) {
    case 0: return Diligent::TEXTURE_COMPONENT_SWIZZLE_R;
    case 1: return Diligent::TEXTURE_COMPONENT_SWIZZLE_G;
    case 2: return Diligent::TEXTURE_COMPONENT_SWIZZLE_B;
    case 3: return Diligent::TEXTURE_COMPONENT_SWIZZLE_A;
    case 4: return Diligent::TEXTURE_COMPONENT_SWIZZLE_ZERO;
    default: return Diligent::TEXTURE_COMPONENT_SWIZZLE_ONE;
  }
}

inline Diligent::TextureComponentMapping ComponentMapping(uint32_t swizzle) {
  return {ComponentSwizzle(swizzle & 7), ComponentSwizzle((swizzle >> 3) & 7),
          ComponentSwizzle((swizzle >> 6) & 7), ComponentSwizzle((swizzle >> 9) & 7)};
}

// Exchanges the red and blue source selectors.
inline uint32_t SwapRedBlueSwizzle(uint32_t swizzle) {
  uint32_t out = 0;
  for (uint32_t i = 0; i < 4; ++i) {
    uint32_t c = (swizzle >> (3 * i)) & 7;
    c = c == 0 ? 2 : c == 2 ? 0 : c;
    out |= c << (3 * i);
  }
  return out;
}

}  // namespace renderer
