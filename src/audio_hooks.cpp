// RenderWare Audio voice creation.
//
// 0x824364F0 creates a voice from a decoder object (r3), first calling its
// vtable. All three callers look the decoder up in a list by codec type and
// pass the result on without checking it, so when no decoder matches, r3 is
// null and the game crashes reading the vtable (seen on the RwAudioCore
// thread, caller 0x82456CB8). Each caller already treats a 0 return as "no
// voice", so return that instead: the one sound is skipped.

#include <rex/hook.h>
#include <rex/logging/macros.h>
#include <rex/ppc/context.h>

#include <atomic>
#include <cstdint>

REX_EXTERN(__imp__sub_824364F0);
REX_HOOK_RAW(sub_824364F0) {
  if (ctx.r3.u32 != 0) {
    __imp__sub_824364F0(ctx, base);
    return;
  }
  static std::atomic<uint32_t> count{0};
  uint32_t n = ++count;
  if (n <= 20) {
    REXLOG_WARN("AUDIO: voice requested with no decoder (lr {:08X} r4 {:08X} r5 {:08X} "
                "r31 {:08X}), sound skipped ({} so far)",
                uint32_t(ctx.lr), ctx.r4.u32, ctx.r5.u32, ctx.r31.u32, n);
  }
  ctx.r3.u64 = 0;
}
