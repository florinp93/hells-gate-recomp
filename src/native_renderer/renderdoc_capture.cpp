#include "renderdoc_capture.h"

#include <rex/cvar.h>
#include <rex/logging/macros.h>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include "renderdoc_app.h"

REXCVAR_DEFINE_BOOL(rdoc_capture_frame, false, "Diagnostics",
                    "Under RenderDoc: capture the GPU work of the next guest frames "
                    "(swap to swap, Insert)");

namespace native {
namespace {

constexpr int kCapturedSwaps = 2;

RENDERDOC_API_1_1_2* g_api = nullptr;
bool g_api_looked_up = false;
int g_swaps_left = 0;

RENDERDOC_API_1_1_2* Api() {
  if (!g_api_looked_up) {
    g_api_looked_up = true;
    if (HMODULE module = GetModuleHandleA("renderdoc.dll")) {
      auto get_api =
          reinterpret_cast<pRENDERDOC_GetAPI>(GetProcAddress(module, "RENDERDOC_GetAPI"));
      if (get_api) get_api(eRENDERDOC_API_Version_1_1_2, reinterpret_cast<void**>(&g_api));
    }
  }
  return g_api;
}

}  // namespace

void RenderDocOnGuestSwap() {
  if (g_swaps_left) {
    if (--g_swaps_left == 0 && g_api) {
      bool ok = g_api->EndFrameCapture(nullptr, nullptr);
      REXLOG_INFO("RenderDoc: guest frame capture {}", ok ? "saved" : "failed");
    }
    return;
  }
  if (!REXCVAR_GET(rdoc_capture_frame)) return;
  rex::cvar::SetFlagByName("rdoc_capture_frame", "false");
  RENDERDOC_API_1_1_2* api = Api();
  if (!api) {
    REXLOG_WARN("RenderDoc: not loaded, launch the game through RenderDoc");
    return;
  }
  api->StartFrameCapture(nullptr, nullptr);
  g_swaps_left = kCapturedSwaps;
  REXLOG_INFO("RenderDoc: capturing {} guest frames", kCapturedSwaps);
}

}  // namespace native
