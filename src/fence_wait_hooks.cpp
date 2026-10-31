// GPU fence wait.
//
// D3D's BlockOnFence (0x827D3178 -> 0x827D3060) waits for the GPU to reach a
// fence by calling a spin helper (0x827D5650) in a tight loop: a few no-op
// delays, then a check of the fence the GPU writes back and of the 5 s hang
// timeout. Under recompilation the delays are free, so the waiting thread
// burns a whole core while the GPU catches up, which on phones and low-end
// PCs takes CPU time and heat from the threads doing real work. Every build
// needs this, whichever renderer is in use.

#include <rex/hook.h>
#include <rex/ppc/context.h>

#include <chrono>
#include <cstdint>
#include <thread>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace {

void PauseFenceSpin(bool waiting) {
  thread_local uint32_t streak = 0;
#if defined(__ANDROID__)
  thread_local std::chrono::steady_clock::time_point started;
#endif
  if (!waiting) {
    streak = 0;
    return;
  }
#if defined(__ANDROID__)
  // Yield through the first millisecond: sleeping on every fence wakes late
  // and lets the cores clock down (35 vs 41 fps unlocked on a Snapdragon
  // 8 Gen 2). Past that, the GPU is the bottleneck and yielding only kept a
  // core at full power, heating the phone into throttling the GPU (heavy
  // fights dropped to 26 fps), so sleep.
  if (streak++ == 0) started = std::chrono::steady_clock::now();
  if (std::chrono::steady_clock::now() - started < std::chrono::milliseconds(1)) {
    std::this_thread::yield();
    return;
  }
  std::this_thread::sleep_for(std::chrono::microseconds(200));
#else
  if (++streak < 32) {
    std::this_thread::yield();
    return;
  }
#if defined(_WIN32)
  thread_local HANDLE timer =
      CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                             TIMER_ALL_ACCESS);
  if (timer) {
    LARGE_INTEGER due;
    due.QuadPart = -2500;
    if (SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE)) {
      WaitForSingleObject(timer, INFINITE);
      return;
    }
  }
#endif
  std::this_thread::sleep_for(std::chrono::microseconds(250));
#endif
}

}  // namespace

// Returns nonzero while the fence is still pending.
REX_EXTERN(__imp__sub_827D5650);
REX_HOOK_RAW(sub_827D5650) {
  __imp__sub_827D5650(ctx, base);
  PauseFenceSpin(ctx.r3.u32 != 0);
}
