#include "guest_memory.h"

#include <rex/hook.h>
#include <rex/ppc/context.h>

#include <cstdint>
#include <cstring>

namespace {

bool InOneRegion(uint32_t address, uint32_t size) {
  uint64_t end = uint64_t(address) + size;
  return end <= 0x100000000ull && (address >= 0xE0000000u) == (end - 1 >= 0xE0000000u);
}

}  // namespace

REX_EXTERN(__imp__sub_82820E50);
REX_HOOK_RAW(sub_82820E50) {
  uint32_t dst = ctx.r3.u32, src = ctx.r4.u32, size = ctx.r5.u32;
  if (!size || (size & 127) || (dst & 127) || (src & 15) || !InOneRegion(dst, size) ||
      !InOneRegion(src, size)) {
    __imp__sub_82820E50(ctx, base);
    return;
  }
  std::memmove(guest::Ptr(base, dst), guest::Ptr(base, src), size);
  ctx.r3.u64 = dst + size;
  ctx.r4.u64 = src + size;
  ctx.r5.u64 = 0;
  ctx.r6.u64 = 16;
  ctx.r7.u64 = 32;
  ctx.r8.u64 = 48;
  ctx.r9.u64 = 64;
  ctx.r10.u64 = 80;
  ctx.r11.u64 = 96;
  ctx.r12.u64 = 112;
}
