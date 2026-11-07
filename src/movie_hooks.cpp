// Movie player.
//
// 0x82836AD0 forwards to the object at +68 of r3 through the first entry of
// its vtable, and returns at once when there is no object. Its caller
// 0x82848608 runs on the movie decode thread and ignores the result. Once
// (17:21 on 2026-10-31, right after loading a save) the object at +68 was
// already torn down, its vtable pointer 0, and the game crashed reading guest
// 0 while the XMA decoder was fed garbage from the same movie. Treat a dead
// object like a missing one and skip the call.

#include "guest_memory.h"

#include <rex/hook.h>
#include <rex/logging/macros.h>
#include <rex/ppc/context.h>

#include <atomic>
#include <cstdint>

REX_EXTERN(__imp__sub_82836AD0);
REX_HOOK_RAW(sub_82836AD0) {
  uint32_t object = guest::Load32(base, ctx.r3.u32 + 68);
  if (object == 0 || guest::Load32(base, object) != 0) {
    __imp__sub_82836AD0(ctx, base);
    return;
  }
  static std::atomic<uint32_t> count{0};
  uint32_t n = ++count;
  if (n <= 20) {
    REXLOG_WARN("MOVIE: call on a torn-down object {:08X} (owner {:08X}, lr {:08X}) skipped "
                "({} so far)",
                object, ctx.r3.u32, uint32_t(ctx.lr), n);
  }
}
