// Frame-rate independence for the UI.
//
// The game runs its simulation at the video mode's refresh rate
// (video_mode_refresh_rate), with the time step derived from it. The UI does
// not: the APT (Flash) player is given a fixed time per update, and the screen
// logic (menus, HUD, minigames) counts updates, so above 60 fps the UI runs
// fast. All of it runs in one listener of the per-frame update message, the
// APT manager's handler; that handler is run at 60 Hz with the real time
// elapsed and the button presses since its last run. Refresh rates that are
// multiples of 60 keep the UI updates evenly spaced; other rates judder.

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/ppc/context.h>

#include <chrono>
#include <cstdint>
#include <cstring>

namespace {

constexpr double kUiRate = 60.0;

// Per-frame update message (the struct sent by the UI step 0x8256E9D8); the
// handler runs the UI for the two message ids stored at these globals.
constexpr uint32_t kUpdateMessage = 0x82CC8930;
constexpr uint32_t kUiUpdateIds[2] = {0x82CC8974, 0x82CC8954};

// Game frame time in seconds (float).
constexpr uint32_t kGameFrameTime = 0x82CBEB60;

// Input manager pads: 4 records of 68 bytes from +60; buttons at +0, the
// previous frame's buttons at +2 (presses = buttons & ~previous).
constexpr uint32_t kPadsOffset = 60, kPadStride = 68, kPadCount = 4;

uint32_t LoadU32(uint8_t* base, uint32_t address) {
  uint32_t v;
  std::memcpy(&v, base + address, 4);
  return __builtin_bswap32(v);
}

uint16_t LoadU16(uint8_t* base, uint32_t address) {
  uint16_t v;
  std::memcpy(&v, base + address, 2);
  return __builtin_bswap16(v);
}

void StoreU16(uint8_t* base, uint32_t address, uint16_t v) {
  v = __builtin_bswap16(v);
  std::memcpy(base + address, &v, 2);
}

float LoadFloat(uint8_t* base, uint32_t address) {
  uint32_t v = LoadU32(base, address);
  float f;
  std::memcpy(&f, &v, 4);
  return f;
}

void StoreFloat(uint8_t* base, uint32_t address, float f) {
  uint32_t v;
  std::memcpy(&v, &f, 4);
  v = __builtin_bswap32(v);
  std::memcpy(base + address, &v, 4);
}

bool UiGated() {
  static const bool gated = rex::cvar::Query<double>("video_mode_refresh_rate") > kUiRate + 0.5;
  return gated;
}

// One UI update stream: runs once per 60 Hz real-time tick.
struct UiStream {
  int64_t last_tick = -1;
  float skipped_time = 0.0f;
  uint16_t buttons_at_last_run[kPadCount] = {};

  bool Due() {
    using Clock = std::chrono::steady_clock;
    static const Clock::time_point start = Clock::now();
    int64_t tick = int64_t(std::chrono::duration<double>(Clock::now() - start).count() * kUiRate);
    if (tick == last_tick) return false;
    last_tick = tick;
    return true;
  }
};

UiStream g_ui_streams[2];
uint32_t g_input_manager = 0;

}  // namespace

// Input manager update: remember the object for the pad records.
REX_EXTERN(__imp__sub_8252EDC0);
REX_HOOK_RAW(sub_8252EDC0) {
  g_input_manager = ctx.r3.u32;
  __imp__sub_8252EDC0(ctx, base);
}

// APT manager message handler (r4 = message).
REX_EXTERN(__imp__sub_82629450);
REX_HOOK_RAW(sub_82629450) {
  int stream = -1;
  if (UiGated() && ctx.r4.u32 == kUpdateMessage) {
    uint32_t id = LoadU32(base, kUpdateMessage);
    for (int i = 0; i < 2; ++i) {
      if (id == LoadU32(base, kUiUpdateIds[i])) stream = i;
    }
  }
  if (stream < 0) {
    __imp__sub_82629450(ctx, base);
    return;
  }
  UiStream& s = g_ui_streams[stream];
  float frame_time = LoadFloat(base, kGameFrameTime);
  if (!s.Due()) {
    s.skipped_time += frame_time;
    return;
  }
  // Frame time and button presses since the last run.
  StoreFloat(base, kGameFrameTime, frame_time + s.skipped_time);
  s.skipped_time = 0.0f;
  uint16_t previous[kPadCount] = {};
  uint32_t pads = g_input_manager ? g_input_manager + kPadsOffset : 0;
  for (uint32_t i = 0; pads && i < kPadCount; ++i) {
    uint32_t pad = pads + i * kPadStride;
    previous[i] = LoadU16(base, pad + 2);
    StoreU16(base, pad + 2, s.buttons_at_last_run[i]);
  }
  __imp__sub_82629450(ctx, base);
  for (uint32_t i = 0; pads && i < kPadCount; ++i) {
    uint32_t pad = pads + i * kPadStride;
    s.buttons_at_last_run[i] = LoadU16(base, pad);
    StoreU16(base, pad + 2, previous[i]);
  }
  StoreFloat(base, kGameFrameTime, frame_time);
}
