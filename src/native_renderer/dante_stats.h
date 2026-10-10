#pragma once

#include <cstdint>

namespace native {

// Render-thread counters for the performance log.
struct DanteStats {
  uint64_t shaders_compiled = 0;
  uint64_t pipelines_created = 0;
};

inline DanteStats g_dante_stats;

}  // namespace native
