// RenderWare Audio voice creation.
//
// 0x824364F0 creates a voice from a decoder object (r3), first calling its
// vtable. All three callers look the decoder up in a list by codec type and
// pass the result on without checking it, so when no decoder matches, r3 is
// null and the game crashes reading the vtable (seen on the RwAudioCore
// thread, caller 0x82456CB8). Each caller already treats a 0 return as "no
// voice", so return that instead: the one sound is skipped.

#include "guest_memory.h"

#include <rex/hook.h>
#include <rex/logging/macros.h>
#include <rex/ppc/context.h>

#include <atomic>
#include <cstdint>
#include <cstdio>

namespace {

// The decoder list hangs off the audio manager at +60; each node's key is the
// decoder type at node+4 (the node is intrusive, payload at node-16). Logging
// the registered types next to the requested one tells a codec that was never
// registered apart from a sound entry that was never loaded.
char* AppendType(char* out, char* end, uint32_t type, bool first) {
  if (out >= end) return out;
  size_t available = size_t(end - out);
  int written = std::snprintf(out, available, first ? "%08X" : " %08X", type);
  if (written <= 0) return out;
  return out + (size_t(written) < available ? size_t(written) : available - 1);
}

void LogDecoderKeys(uint8_t* base, uint32_t manager, uint32_t wanted) {
  char text[192] = {};
  char* out = text;
  char* end = text + sizeof(text) - 1;
  uint32_t list = guest::IsPlausibleAddress(manager) ? guest::Load32(base, manager + 60) : 0;
  uint32_t node = guest::IsPlausibleAddress(list) ? guest::Load32(base, list) : 0;
  for (int i = 0; node && i < 16; ++i) {
    if (!guest::IsPlausibleAddress(node)) break;
    out = AppendType(out, end, guest::Load32(base, node + 4), i == 0);
    node = guest::Load32(base, node);
  }
  REXLOG_WARN("AUDIO: wanted decoder type {:08X}, registered decoders: {}", wanted, text);
}

}  // namespace

REX_EXTERN(__imp__sub_824364F0);
REX_HOOK_RAW(sub_824364F0) {
  if (ctx.r3.u32 != 0) {
    __imp__sub_824364F0(ctx, base);
    return;
  }
  static std::atomic<uint32_t> count{0};
  uint32_t n = ++count;
  if (n <= 20) {
    // The format byte selects the expected decoder type from the game's table
    // at 0x82106410.
    uint32_t wanted = 0;
    if (guest::IsPlausibleAddress(ctx.r31.u32)) {
      uint32_t format = guest::Ptr(base, ctx.r31.u32 + 72)[0];
      wanted = guest::Load32(base, 0x82106410u + format * 4);
    }
    REXLOG_WARN("AUDIO: voice requested with no decoder (lr {:08X} r4 {:08X} r5 {:08X} "
                "r31 {:08X}), sound skipped ({} so far)",
                uint32_t(ctx.lr), ctx.r4.u32, ctx.r5.u32, ctx.r31.u32, n);
    LogDecoderKeys(base, ctx.r5.u32, wanted);
  }
  ctx.r3.u64 = 0;
}

// 0x8243FF98 (RenderWare Audio) reads bytes +48/+49 of its object (r3) first.
// Its caller at 0x82455EF0 looks the object up in a global list
// and falls back to a default that can be null, so a cutscene sound with no
// match crashed the audio thread reading guest 0x31. Both callers store the
// result and treat 0 as "no voice", so return that: the one sound is skipped.
REX_EXTERN(__imp__sub_8243FF98);
REX_HOOK_RAW(sub_8243FF98) {
  if (ctx.r3.u32 != 0) {
    __imp__sub_8243FF98(ctx, base);
    return;
  }
  static std::atomic<uint32_t> count{0};
  uint32_t n = ++count;
  if (n <= 20) {
    // The lookup key is the voice's player key (+40); the registered players
    // are the two keys in the list at 0x82CA7824.
    uint32_t key = 0;
    if (guest::IsPlausibleAddress(ctx.r29.u32)) {
      key = guest::Load32(base, ctx.r29.u32 + 40);
    }
    REXLOG_WARN("AUDIO: voice started with no sound entry (lr {:08X} voice {:08X} key {:08X} "
                "r6 {:08X}), sound skipped ({} so far)",
                uint32_t(ctx.lr), ctx.r29.u32, key, ctx.r6.u32, n);
  }
  ctx.r3.u64 = 0;
}
