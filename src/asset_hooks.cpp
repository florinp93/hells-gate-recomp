// Asset lookup by name hash.
//
// 0x825BA100 searches the loaded packages for an entry: the manager (r3) holds
// a list of nodes (next at +4, package at +12); each package has a table of
// 24-byte entries at +40 with the count at +48, keyed by the hash at +20 of an
// entry. Its caller 0x825BC220 instantiates what it finds. Once (on a
// Snapdragon 8 Elite, 2.5 minutes into play) a package still in the list had
// a garbage table pointer (0x6FFFFFF8): it had been freed while another thread
// searched it, and the game crashed reading guest 0x7000000C. This is the same
// search with every pointer checked first; a package that isn't readable is
// skipped, as if it held no match.

#include "guest_memory.h"

#include <rex/hook.h>
#include <rex/logging/macros.h>
#include <rex/memory/utils.h>
#include <rex/ppc/context.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>

#include <atomic>
#include <cstdint>

namespace {

// [address, address + size) is mapped and readable guest memory.
bool Readable(uint32_t address, uint32_t size) {
  if (!size || address + size < address) return false;
  auto* heap = rex::system::kernel_memory()->LookupHeap(address);
  if (!heap) return false;
  auto access = heap->QueryRangeAccess(address, address + size - 1);
  return (uint32_t(access) & uint32_t(rex::memory::PageAccess::kReadOnly)) != 0;
}

void LogSkipped(const char* what, uint32_t address, uint32_t lr) {
  static std::atomic<uint32_t> count{0};
  uint32_t n = ++count;
  if (n <= 20) {
    REXLOG_WARN("ASSET: skipped a package with an unreadable {} {:08X} (lr {:08X}, {} so far)",
                what, address, lr, n);
  }
}

}  // namespace

REX_HOOK_RAW(sub_825BA100) {
  const uint32_t manager = ctx.r3.u32;
  const uint32_t key = ctx.r4.u32;
  const uint32_t lr = uint32_t(ctx.lr);
  ctx.r3.u64 = 0;
  if (!Readable(manager, 20) || guest::Load16(base, manager + 18) == 0) return;

  for (uint32_t node = guest::Load32(base, manager + 8); node;) {
    if (!Readable(node, 16)) {
      LogSkipped("list node", node, lr);
      return;
    }
    uint32_t package = guest::Load32(base, node + 12);
    if (package) {
      if (!Readable(package, 52)) {
        LogSkipped("package", package, lr);
      } else {
        uint32_t count = guest::Load32(base, package + 48);
        uint32_t entry = guest::Load32(base, package + 40);
        if (count && (count > 0x100000 || !Readable(entry, count * 24))) {
          LogSkipped("entry table", entry, lr);
        } else {
          for (uint32_t i = 0; i < count; ++i, entry += 24) {
            if (guest::Load32(base, entry + 20) != key) continue;
            uint32_t index = guest::Load32(base, entry + 4);
            uint32_t slot = package + (index + 20) * 4;
            guest::Store32(base, ctx.r7.u32, node);
            guest::Store32(base, ctx.r6.u32, index);
            guest::Store8(base, ctx.r5.u32, uint8_t(guest::Load32(base, package + 4)));
            uint32_t result = Readable(slot, 4) ? guest::Load32(base, slot) : 0;
            ctx.r3.u64 = result;
            if (result) return;
            break;
          }
        }
      }
    }
    node = guest::Load32(base, node + 4);
  }
}
