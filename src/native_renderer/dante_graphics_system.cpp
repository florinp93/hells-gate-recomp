#include "dante_graphics_system.h"

#include <rex/cvar.h>
#include <rex/logging/macros.h>
#include <rex/system/gpu_plugin.h>

#include <windows.h>

#include <cstdlib>

#if REX_HAS_VULKAN
#include <rex/graphics/vulkan/graphics_system.h>
#endif

REXCVAR_DEFINE_STRING(renderer, "xenos", "Graphics",
                      "Graphics system: 'xenos' loads the rexgpu-xenos plugin "
                      "(default), 'native' uses the in-process Vulkan xenos "
                      "backend, 'wrapped' forwards to the plugin through "
                      "DanteGraphicsSystem, 'dante' renders natively and keeps "
                      "the plugin for sync only (work in progress)");

REXCVAR_DEFINE_STRING(native_render_scale, "off", "Graphics",
                      "Guest draw resolution scale, applied via the SDK "
                      "'resolution_scale' cvar before GPU init. 'off' leaves "
                      "the SDK default, 'auto' derives the scale from the "
                      "display height (display_height / 720, rounded), "
                      "'1'-'7' forces a fixed integer scale");

REXCVAR_DEFINE_BOOL(dante_packet_inventory, false, "Diagnostics",
                    "renderer=dante: turn on the plugin's gpu_packet_inventory (PM4 packets "
                    "the sync-only command processor executes, logged every 10 s)");

namespace dante {

DanteGraphicsSystem::DanteGraphicsSystem(
    std::unique_ptr<rex::system::IGraphicsSystem> inner)
    : inner_(std::move(inner)) {
  REXLOG_INFO("DanteGraphicsSystem: created");
}

rex::X_STATUS DanteGraphicsSystem::SetupPresentation(
    rex::ui::WindowedAppContext* app_context) {
  REXLOG_INFO("DanteGraphicsSystem: SetupPresentation");
  return inner_->SetupPresentation(app_context);
}

rex::X_STATUS DanteGraphicsSystem::SetupGuestGpu(
    rex::runtime::FunctionDispatcher* function_dispatcher,
    rex::system::KernelState* kernel_state) {
  REXLOG_INFO("DanteGraphicsSystem: SetupGuestGpu");
  return inner_->SetupGuestGpu(function_dispatcher, kernel_state);
}

bool DanteGraphicsSystem::has_presentation() const {
  return inner_->has_presentation();
}

rex::ui::GraphicsProvider* DanteGraphicsSystem::provider() const {
  return inner_->provider();
}

rex::ui::Presenter* DanteGraphicsSystem::presenter() const {
  return inner_->presenter();
}

void DanteGraphicsSystem::SetInterruptCallback(uint32_t callback,
                                               uint32_t user_data) {
  inner_->SetInterruptCallback(callback, user_data);
}

void DanteGraphicsSystem::InitializeRingBuffer(uint32_t ptr,
                                               uint32_t size_log2) {
  REXLOG_INFO(
      "DanteGraphicsSystem: InitializeRingBuffer ptr={:#x} size_log2={}", ptr,
      size_log2);
  inner_->InitializeRingBuffer(ptr, size_log2);
}

void DanteGraphicsSystem::EnableReadPointerWriteBack(
    uint32_t ptr, uint32_t block_size_log2) {
  inner_->EnableReadPointerWriteBack(ptr, block_size_log2);
}

void DanteGraphicsSystem::InitializeShaderStorage(
    const std::filesystem::path& cache_root, uint32_t title_id, bool blocking) {
  inner_->InitializeShaderStorage(cache_root, title_id, blocking);
}

void DanteGraphicsSystem::Shutdown() {
  REXLOG_INFO("DanteGraphicsSystem: Shutdown");
  inner_->Shutdown();
}

std::unique_ptr<rex::system::IGraphicsSystem> CreateConfiguredGraphicsSystem(
    const std::string& gpu_plugin) {
  const std::string renderer = rex::cvar::Query<std::string>("renderer");
  if (renderer == "xenos" || renderer == "rexglue" || renderer == "dante") {
    return nullptr;
  }
  if (renderer == "native") {
#if REX_HAS_VULKAN
    REXLOG_INFO(
        "DanteGraphicsSystem: renderer=native, constructing in-process "
        "Vulkan xenos backend");
    return std::make_unique<DanteGraphicsSystem>(
        std::make_unique<rex::graphics::vulkan::VulkanGraphicsSystem>());
#else
    REXLOG_ERROR(
        "renderer=native requires a Vulkan-enabled SDK build "
        "(REXGLUE_USE_VULKAN); falling back to the default plugin path");
    return nullptr;
#endif
  }
  if (renderer == "wrapped") {
    auto inner = rex::system::LoadGpuPlugin(gpu_plugin);
    if (!inner) {
      REXLOG_ERROR(
          "DanteGraphicsSystem: failed to load inner plugin '{}'; falling back "
          "to the default plugin path",
          gpu_plugin);
      return nullptr;
    }
    return std::make_unique<DanteGraphicsSystem>(std::move(inner));
  }
  REXLOG_WARN("Unknown renderer='{}'; using default xenos plugin", renderer);
  return nullptr;
}

void ApplyDanteRendererConfig() {
  if (rex::cvar::Query<std::string>("renderer") != "dante") {
    return;
  }
  bool host_off = rex::cvar::SetFlagByName("gpu_host_rendering", "false");
  // Without host draws there are no real sample counts; the CP then writes
  // query_occlusion_fake_sample_count ("visible").
  bool queries_off = rex::cvar::SetFlagByName("occlusion_query_enable", "false");
  REXLOG_INFO("renderer=dante: gpu_host_rendering=false ({}), occlusion_query_enable=false ({})",
              host_off ? "ok" : "FAILED", queries_off ? "ok" : "FAILED");
  if (REXCVAR_GET(dante_packet_inventory)) {
    bool ok = rex::cvar::SetFlagByName("gpu_packet_inventory", "true");
    REXLOG_INFO("renderer=dante: gpu_packet_inventory ({})", ok ? "ok" : "FAILED");
  }
}

void ApplyRenderScaleConfig() {
  const std::string mode =
      rex::cvar::Query<std::string>("native_render_scale");
  if (mode.empty() || mode == "off") {
    return;
  }

  int scale = 0;
  if (mode == "auto") {
    // The guest always renders a 720p-class frame; pick the integer scale
    // that best matches the target display height, rounding up so
    // non-integer ratios supersample rather than undersample.
    const bool fullscreen = rex::cvar::Query<bool>("fullscreen");
    const int window_height = rex::cvar::Query<int32_t>("window_height");
    const int target_height =
        fullscreen ? GetSystemMetrics(SM_CYSCREEN)
                   : (window_height > 0 ? window_height : 720);
    scale = (target_height + 359) / 720;
  } else {
    scale = std::atoi(mode.c_str());
  }

  scale = std::clamp(scale, 1, 7);
  if (scale <= 1) {
    return;
  }
  REXLOG_INFO("native_render_scale={} -> resolution_scale={}", mode, scale);
  rex::cvar::SetFlagByName("resolution_scale", std::to_string(scale));
}

}  // namespace dante
