#include "guest_memory.h"

#include <rex/hook.h>
#include <rex/ppc/context.h>

#include <rex/logging/macros.h>

#include <atomic>
#include <cstdint>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace {

using namespace guest;

constexpr uint32_t kLayerAnimation = 20;
constexpr uint32_t kLayerRoot = 96;
constexpr uint32_t kNodeData = 4;
constexpr uint32_t kNodeChildren = 12;
constexpr uint32_t kNodeTypeCount = 37;
constexpr uint32_t kBlendNodeType = 35;
constexpr uint32_t kBlendDataSize = 28;
constexpr int kMaxDepth = 64;
constexpr int kMaxNodes = 4096;
constexpr int kMaxWarnings = 32;

std::atomic<int> g_warnings{0};

struct TreeWalk {
  const uint8_t* base;
  int nodes = 0;
  volatile uint32_t current = 0;
  bool unreadable = false;
};

void Touch(const uint8_t* base, uint32_t address) {
  (void)*reinterpret_cast<const volatile uint8_t*>(Ptr(base, address));
}

[[clang::noinline]] bool WalkNode(TreeWalk& walk, uint32_t node, int depth) {
  if (depth > kMaxDepth || ++walk.nodes > kMaxNodes) return true;
  const uint8_t* base = walk.base;
  walk.current = node;
  Touch(base, node + kNodeChildren - 1);
  const uint32_t type = *reinterpret_cast<const volatile uint8_t*>(Ptr(base, node));
  const uint32_t count = *reinterpret_cast<const volatile uint8_t*>(Ptr(base, node + 1));
  if (type >= kNodeTypeCount) return false;
  if (type == kBlendNodeType) {
    const uint32_t data = Load32(base, node + kNodeData);
    Touch(base, data);
    Touch(base, data + kBlendDataSize - 1);
  }
  if (count) Touch(base, node + kNodeChildren + count * 4 - 1);
  for (uint32_t i = 0; i < count; ++i) {
    if (!WalkNode(walk, Load32(base, node + kNodeChildren + i * 4), depth + 1)) return false;
    walk.current = node;
  }
  return true;
}

#ifdef _WIN32
bool TreeValid(TreeWalk& walk, uint32_t root) {
  __try {
    return WalkNode(walk, root, 0);
  } __except (GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION ? EXCEPTION_EXECUTE_HANDLER
                                                                : EXCEPTION_CONTINUE_SEARCH) {
    walk.unreadable = true;
    return false;
  }
}
#else
bool TreeValid(TreeWalk&, uint32_t) { return true; }
#endif

void CheckLayer(uint8_t* base, uint32_t layer, const char* where) {
  if (!IsPlausibleAddress(layer)) return;
  const uint32_t root = Load32(base, layer + kLayerRoot);
  if (!root) return;
  TreeWalk walk{base};
  if (TreeValid(walk, root)) return;
  Store32(base, layer + kLayerRoot, 0);
  if (g_warnings.fetch_add(1, std::memory_order_relaxed) < kMaxWarnings) {
    REXLOG_WARN("ANIM: layer {:08X} animation {:08X} root {:08X} detached: {} node {:08X} ({})",
                layer, Load32(base, layer + kLayerAnimation), root,
                walk.unreadable ? "unreadable" : "invalid type in", uint32_t(walk.current), where);
  }
}

}  // namespace

REX_EXTERN(__imp__sub_824EF168);
REX_HOOK_RAW(sub_824EF168) {
  CheckLayer(base, ctx.r3.u32, "layer update");
  __imp__sub_824EF168(ctx, base);
}

REX_EXTERN(__imp__sub_824EEC78);
REX_HOOK_RAW(sub_824EEC78) {
  CheckLayer(base, ctx.r3.u32, "set animation");
  __imp__sub_824EEC78(ctx, base);
}
