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
