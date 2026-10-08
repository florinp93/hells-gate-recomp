// Effective Xenos GPU state at a D3D draw, rebuilt from the guest D3D device
// (verified against the Xenos register file by the M2 observer). Rules:
// docs/NATIVE_D3D_MAP.md, "Effective GPU state at a draw". Render thread only.

#pragma once

#include <array>
#include <cstdint>

struct PPCContext;

namespace native::gpu {

inline const uint8_t* GuestPtr(const uint8_t* base, uint32_t addr) {
  return base + addr + (addr >= 0xE0000000u ? 0x1000u : 0u);
}
inline uint32_t Load32(const uint8_t* base, uint32_t addr) {
  return __builtin_bswap32(*reinterpret_cast<const uint32_t*>(GuestPtr(base, addr)));
}
inline uint32_t Load16(const uint8_t* base, uint32_t addr) {
  return __builtin_bswap16(*reinterpret_cast<const uint16_t*>(GuestPtr(base, addr)));
}
inline bool Plausible(uint32_t addr) { return addr >= 0x10000u && addr < 0xFFFF0000u; }

// Guest virtual -> GPU physical, as the D3D layer computes it.
inline uint32_t GpuAddress(uint32_t addr) {
  return (addr & 0x1FFFFFFFu) + (((addr >> 20) + 0x200u) & 0x1000u);
}

// (first register, count, device offset)
struct ShadowRange {
  uint32_t reg;
  uint32_t count;
  uint32_t offset;
};
inline constexpr ShadowRange kShadowRanges[] = {
    {0x2000, 16, 10368},   // RB_SURFACE_INFO, RB_COLOR_INFO, RB_DEPTH_INFO, ...
    {0x2080, 3, 10432},    // PA_SC_WINDOW_OFFSET, PA_SC_WINDOW_SCISSOR_TL/BR
    {0x2100, 21, 10444},   // VGT_*, RB_COLOR_MASK, RB_STENCILREFMASK, ...
    {0x2180, 5, 10528},    // SQ_PROGRAM_CNTL, ...
    {0x2200, 12, 10548},   // RB_DEPTHCONTROL, RB_BLENDCONTROL*, RB_COLORCONTROL, ...
    {0x2280, 21, 10596},   // PA_SU_POINT_SIZE, ...
    {0x2300, 38, 10680},   // PA_SC_LINE_CNTL, RB_COPY_*, ...
    {0x2380, 8, 10832},    // PA_SU_POLY_OFFSET_*
    {0x4800, 192, 1152},   // fetch constants (32 x 6 dwords)
    {0x4000, 1024, 1920},  // VS float constants
    {0x4400, 1024, 6016},  // PS float constants
    {0x4900, 40, 10112},   // bool + loop constants
};
inline constexpr uint32_t kStateDwords = [] {
  uint32_t total = 0;
  for (const auto& range : kShadowRanges) total += range.count;
  return total;
}();

// Index into a State for a register, or -1 if not part of the state.
constexpr int StateIndex(uint32_t reg) {
  int index = 0;
  for (const auto& range : kShadowRanges) {
    if (reg >= range.reg && reg < range.reg + range.count) return index + int(reg - range.reg);
    index += int(range.count);
  }
  return -1;
}

// Device offsets of current objects.
inline constexpr uint32_t kDevRingWritePtr = 48;  // last dword written
inline constexpr uint32_t kDevVertexDecl = 11992;
inline constexpr uint32_t kDevIndexBuffer = 12612;
inline constexpr uint32_t kDevRenderTargets = 12616;  // RT0..3
inline constexpr uint32_t kDevDepthStencil = 12632;
inline constexpr uint32_t kDevStreams = 12636;  // 4 * stream
inline constexpr uint32_t kDevPixelShader = 12868;
inline constexpr uint32_t kDevVertexShader = 12872;

// Registers in shadow order (kShadowRanges), see StateIndex.
using State = std::array<uint32_t, kStateDwords>;

struct CaptureStats {
  uint32_t literal_dwords = 0;
  uint32_t buffer_constant_dwords = 0;
};

// Before every draw call: dirty constant groups are about to be uploaded from
// the shadow (dev+0 VS / dev+8 PS, bit 63-g for group g).
void TrackDrawFlush(uint32_t device, const uint8_t* base);
// Before 0x8291D778 (type, start, count, pBuffer, offset): constants loaded
// from buffer[+0x18] + offset * 16 instead of the shadow.
void TrackConstantBufferLoad(const PPCContext& ctx, const uint8_t* base);

// The state the GPU sees for the draw just submitted: shadow, then
// buffer-loaded float constants, then the bound shaders' literal constants.
CaptureStats Capture(uint32_t device, const uint8_t* base, State& out);

inline uint32_t Reg(const State& state, uint32_t reg) {
  int index = StateIndex(reg);
  return index < 0 ? 0 : state[index];
}

}  // namespace native::gpu
