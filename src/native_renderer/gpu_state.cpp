#include "gpu_state.h"

#include <rex/ppc/context.h>

namespace native::gpu {
namespace {

// Per vec4 (0-511): buffer address loaded by 0x8291D778, 0 = shadow.
std::array<uint32_t, 512> g_constant_source{};

// Literal constants written to the GPU by earlier program flushes. D3D loads
// them without updating its shadow, so they stay in the GPU registers (and
// later shaders may read them) until a dirty-group flush or buffer load
// overwrites that vec4.
struct StaleLiteral {
  bool valid = false;
  uint32_t value[4] = {};
};
std::array<StaleLiteral, 512> g_stale_literals{};

// Literal constants loaded by the program flush (0x827EA218): records
// {u16 vec4, u16 dwords, u32 offset} at blk + [blk+20] + 20, data at base + offset.
uint32_t ApplyShaderLiterals(const uint8_t* base, uint32_t blk, uint32_t ucode_base,
                             State& state) {
  if (!Plausible(blk) || !Plausible(ucode_base)) return 0;
  uint32_t table_offset = Load32(base, blk + 20);
  if (!table_offset) return 0;
  uint32_t table = blk + table_offset;
  uint32_t rec = table + 20;
  uint32_t end = rec + Load32(base, table + 16);
  uint32_t applied = 0;
  for (uint32_t n = 0; rec < end && n < 256; ++n) {
    uint32_t vec4_index = Load16(base, rec);
    uint32_t dword_count = Load16(base, rec + 2);
    if (!dword_count || dword_count > 1024) break;
    uint32_t data = ucode_base + Load32(base, rec + 4);
    rec += 8;
    if (!Plausible(data)) break;
    for (uint32_t d = 0; d < dword_count; ++d) {
      uint32_t value = Load32(base, data + d * 4);
      uint32_t vec4 = vec4_index + d / 4;
      if (vec4 < 512) {
        g_stale_literals[vec4].valid = true;
        g_stale_literals[vec4].value[d & 3] = value;
      }
      int index = StateIndex(0x4000 + vec4_index * 4 + d);
      if (index < 0) continue;
      state[index] = value;
      ++applied;
    }
  }
  return applied;
}

}  // namespace

void TrackDrawFlush(uint32_t device, const uint8_t* base) {
  for (uint32_t type = 0; type < 2; ++type) {
    uint32_t mask_addr = device + type * 8;
    uint64_t dirty = (uint64_t(Load32(base, mask_addr)) << 32) | Load32(base, mask_addr + 4);
    if (!dirty) continue;
    for (uint32_t group = 0; group < 64; ++group) {
      if (!((dirty >> (63 - group)) & 1)) continue;
      for (uint32_t i = 0; i < 4; ++i) {
        g_constant_source[type * 256 + group * 4 + i] = 0;
        g_stale_literals[type * 256 + group * 4 + i].valid = false;
      }
    }
  }
}

void TrackConstantBufferLoad(const PPCContext& ctx, const uint8_t* base) {
  uint32_t start = (ctx.r4.u32 << 8) + ctx.r5.u32;
  uint32_t count = ctx.r6.u32;
  uint32_t buffer = ctx.r7.u32;
  if (!Plausible(buffer)) return;
  uint32_t source = Load32(base, buffer + 0x18) + ctx.r8.u32 * 16;
  for (uint32_t i = 0; i < count && start + i < 512; ++i) {
    g_constant_source[start + i] = source + i * 16;
    g_stale_literals[start + i].valid = false;
  }
}

CaptureStats Capture(uint32_t device, const uint8_t* base, State& out) {
  CaptureStats stats;
  uint32_t index = 0;
  for (const auto& range : kShadowRanges) {
    for (uint32_t i = 0; i < range.count; ++i) {
      out[index++] = Load32(base, device + range.offset + i * 4);
    }
  }
  for (uint32_t vec4 = 0; vec4 < 512; ++vec4) {
    const StaleLiteral& stale = g_stale_literals[vec4];
    if (!stale.valid) continue;
    for (uint32_t c = 0; c < 4; ++c) {
      int state_index = StateIndex(0x4000 + vec4 * 4 + c);
      if (state_index >= 0) out[state_index] = stale.value[c];
    }
  }
  for (uint32_t vec4 = 0; vec4 < 512; ++vec4) {
    uint32_t source = g_constant_source[vec4];
    if (!source) continue;
    for (uint32_t c = 0; c < 4; ++c) {
      int state_index = StateIndex(0x4000 + vec4 * 4 + c);
      if (state_index < 0) continue;
      out[state_index] = Load32(base, source + c * 4);
      ++stats.buffer_constant_dwords;
    }
  }
  uint32_t vs = Load32(base, device + kDevVertexShader);
  uint32_t ps = Load32(base, device + kDevPixelShader);
  if (Plausible(vs)) {
    stats.literal_dwords += ApplyShaderLiterals(base, vs + 872, Load32(base, vs + 0x20), out);
  }
  if (Plausible(ps)) {
    stats.literal_dwords += ApplyShaderLiterals(base, ps + 40, Load32(base, ps + 0x18), out);
  }
  return stats;
}

}  // namespace native::gpu
