#pragma once

// Code called from the generated recompiled functions.
//
// Saving and loading go through a guest setjmp / longjmp pair. The longjmp
// has to unwind the host C++ stack of the recompiled calls as well, so
// patches/generated/apply_generated_patches.py routes both through a host
// setjmp / longjmp here.

#include <rex/logging/macros.h>
#include <rex/ppc.h>
#include <rex/runtime.h>

#include <algorithm>
#include <cstring>
#include <csetjmp>
#include <cstdint>

inline thread_local jmp_buf g_fiber_jmp_buf;
inline thread_local uint32_t g_setjmp_ctx_addr = 0;
inline thread_local uint32_t g_longjmp_return_value = 0;

inline int FiberSetjmp(uint32_t ctx_addr) {
  g_setjmp_ctx_addr = ctx_addr;
  int ret = setjmp(g_fiber_jmp_buf);
  if (ret != 0) {
    REXLOG_INFO("FIBER: setjmp returning from longjmp (ret={})", ret);
  }
  return ret;
}

inline void FiberLongjmp(uint32_t return_value) {
  g_longjmp_return_value = return_value;
  REXLOG_INFO("FIBER: longjmp called with return_value={}", return_value);
  longjmp(g_fiber_jmp_buf, 1);
}

// After a longjmp: the guest registers the guest setjmp saved in its buffer
// (f14-f31, r1, r13-r31, CR, LR, v64-v127), and the longjmp's return value.
inline void FiberRestoreContext(PPCContext& ctx, uint8_t* base) {
  if (g_setjmp_ctx_addr == 0) return;
  uint32_t addr = g_setjmp_ctx_addr;
  uint32_t phys_offset = (addr >= 0xE0000000u) ? 0x1000u : 0u;
  const uint8_t* ptr = base + addr + phys_offset;
  auto load64 = [&](uint32_t off) {
    return __builtin_bswap64(*reinterpret_cast<const uint64_t*>(ptr + off));
  };
  auto load32 = [&](uint32_t off) {
    return __builtin_bswap32(*reinterpret_cast<const uint32_t*>(ptr + off));
  };
  for (int i = 14; i <= 31; ++i) {
    (&ctx.f0)[i].u64 = load64((i - 14) * 8);
  }
  ctx.r1.u64 = load64(144);
  for (int i = 13; i <= 31; ++i) {
    (&ctx.r3)[i].u64 = load64(152 + (i - 13) * 8);
  }
  uint32_t cr = load32(304);
  for (int i = 0; i < 8; ++i) {
    (&ctx.cr0)[i].set_raw((cr >> (28 - i * 4)) & 0xF);
  }
  ctx.lr = load32(308);
  for (int i = 64; i <= 127; ++i) {
    const uint8_t* src = ptr + 320 + (i - 64) * 16;
    auto* dst = (&ctx.v0)[i].u8;
    for (int b = 0; b < 16; ++b) {
      dst[b] = src[15 - b];
    }
  }
  ctx.r3.u32 = g_longjmp_return_value;
  REXLOG_INFO("FIBER: restored ctx from jmp_buf @{:08X} r1=0x{:08X} r3={}", addr, ctx.r1.u32,
              ctx.r3.s32);
}

inline float g_ultrawide_target_aspect = 0.0f;

// Runs before `stfs f0,-30916(r6)` in the display-mode init, which writes the
// game's global aspect ratio field. Overwriting f0 here makes every consumer
// (projection, FOV, UI layout) use the configured target aspect.
inline void UltrawideAspectHook(rex::ppc::Register& f0) {
  if (g_ultrawide_target_aspect > 0.0f) {
    REXLOG_INFO("ULTRAWIDE: aspect store override {:.4f} -> {:.4f}", f0.f64,
                g_ultrawide_target_aspect);
    f0.f64 = g_ultrawide_target_aspect;
  }
}

// Guest virtual memory, set before the game starts (OnPreLaunchModule).
inline uint8_t* g_guest_membase = nullptr;

// Render size override, 0 = the game's own choice (1280x720 on an HD output).
inline uint32_t g_render_width = 0;
inline uint32_t g_render_height = 0;

// Runs in the display-mode init (0x824D6B94) after the size globals are
// written and before the back buffer size (r30 x r29) goes into the D3D
// present parameters. The game sizes its surfaces, viewports and resolves from
// these, so drawing at e.g. 960x540 needs nothing else; the presenter then
// upscales the smaller front buffer (SGSR when chosen). Larger than 1280x720
// overflows the 10 MB EDRAM and corrupts the picture, so it's refused.
inline void RenderSizeHook(rex::ppc::Register& r29, rex::ppc::Register& r30) {
  if (!g_render_width || !g_render_height || !g_guest_membase) return;
  // Only the HD path; standard-definition outputs keep the game's sizes.
  if ((r30.u32 & 0xFFFF) != 1280 || (r29.u32 & 0xFFFF) != 720) return;
  uint32_t width = std::clamp<uint32_t>(g_render_width, 320, 1280);
  uint32_t height = std::clamp<uint32_t>(g_render_height, 180, 720);
  if (width == 1280 && height == 720) return;
  auto store16 = [](uint32_t address, uint32_t value) {
    uint16_t be = __builtin_bswap16(uint16_t(value));
    std::memcpy(g_guest_membase + address, &be, sizeof(be));
  };
  REXLOG_INFO("RENDER SIZE: {}x{} -> {}x{}", r30.u32 & 0xFFFF, r29.u32 & 0xFFFF, width, height);
  // Back buffer: the present parameters (from r30/r29) and its globals.
  r30.u64 = width;
  r29.u64 = height;
  store16(0x82C98740, width);
  store16(0x82C98744, height);
  // Output size: the front buffer the presenter receives.
  store16(0x82C9875C, width);
  store16(0x82C98760, height);
}
