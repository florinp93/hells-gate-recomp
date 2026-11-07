#include "menu_hooks.h"

#include "guest_memory.h"

#include <rex/hook.h>
#include <rex/logging/macros.h>
#include <rex/ppc/context.h>

#include <cstdint>
#include <cstring>
#include <mutex>
#include <utility>

namespace {

constexpr uint32_t kMenuListOffset = 688;
constexpr uint32_t kMenuItemSize = 128;
constexpr uint32_t kMenuItemLabelSize = 64;
constexpr uint32_t kMenuItemStateFunction = 68;
constexpr uint32_t kMenuItemSelectObject = 96;
constexpr uint32_t kAlwaysVisibleState = 0x82374AA8u;
constexpr uint32_t kQuitToDesktopMarker = 0x8212719Cu;
constexpr char kQuitToDesktopLabel[] = "Quit to Desktop";

std::mutex g_quit_mutex;
std::function<void()> g_quit_handler;

void RequestQuitToDesktop() {
  REXLOG_INFO("MENU: quit to desktop selected");
  std::function<void()> handler;
  {
    std::lock_guard lock(g_quit_mutex);
    handler = g_quit_handler;
  }
  if (handler) handler();
}

void SetLabel(uint8_t* base, uint32_t item, const char* label, size_t size) {
  std::memset(guest::Ptr(base, item), 0, kMenuItemLabelSize);
  std::memcpy(guest::Ptr(base, item), label, size);
}

}  // namespace

namespace menu_hooks {

void SetQuitToDesktopHandler(std::function<void()> handler) {
  std::lock_guard lock(g_quit_mutex);
  g_quit_handler = std::move(handler);
}

}  // namespace menu_hooks

REX_EXTERN(sub_823E2140);

REX_EXTERN(__imp__sub_823D3370);
REX_HOOK_RAW(sub_823D3370) {
  const uint32_t screen = ctx.r3.u32;
  const uint32_t stack = ctx.r1.u32;
  __imp__sub_823D3370(ctx, base);

  const uint32_t list = screen + kMenuListOffset;
  const uint32_t items = guest::Load32(base, list);
  const uint32_t count = guest::Load32(base, list + 4);
  if (!items || !count) return;

  const uint32_t item = (stack - 512) & ~15u;
  std::memcpy(guest::Ptr(base, item), guest::Ptr(base, items + (count - 1) * kMenuItemSize),
              kMenuItemSize);
  SetLabel(base, item, kQuitToDesktopLabel, sizeof(kQuitToDesktopLabel));
  guest::Store32(base, item + kMenuItemSelectObject, kQuitToDesktopMarker);

  ctx.r1.u32 = item - 128;
  guest::Store32(base, ctx.r1.u32, stack);
  ctx.r3.u32 = list;
  ctx.r4.u32 = item;
  sub_823E2140(ctx, base);
  ctx.r1.u32 = stack;
  REXLOG_INFO("MENU: pause menu entries {} -> {}", count, guest::Load32(base, list + 4));
}

REX_EXTERN(__imp__sub_823D3B10);
REX_HOOK_RAW(sub_823D3B10) {
  if (ctx.r3.u32 != kQuitToDesktopMarker) {
    __imp__sub_823D3B10(ctx, base);
    return;
  }
  RequestQuitToDesktop();
}

REX_EXTERN(__imp__sub_823DA5B0);
REX_HOOK_RAW(sub_823DA5B0) {
  const uint32_t screen = ctx.r3.u32;
  __imp__sub_823DA5B0(ctx, base);

  const uint32_t list = screen + kMenuListOffset;
  const uint32_t items = guest::Load32(base, list);
  const uint32_t count = guest::Load32(base, list + 4);
  if (!items || !count) return;

  const uint32_t item = items + (count - 1) * kMenuItemSize;
  SetLabel(base, item, kQuitToDesktopLabel, sizeof(kQuitToDesktopLabel));
  guest::Store32(base, item + kMenuItemStateFunction, kAlwaysVisibleState);
  guest::Store32(base, item + kMenuItemSelectObject, kQuitToDesktopMarker);
  REXLOG_INFO("MENU: main menu entries {}, last entry is Quit to Desktop", count);
}

REX_EXTERN(__imp__sub_823DB698);
REX_HOOK_RAW(sub_823DB698) {
  if (ctx.r3.u32 != kQuitToDesktopMarker) {
    __imp__sub_823DB698(ctx, base);
    return;
  }
  RequestQuitToDesktop();
}
