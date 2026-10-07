#pragma once

#include <rex/rex_app.h>
#include <rex/cvar.h>
#include <rex/chrono/clock.h>
#include <rex/filesystem.h>
#include <rex/input/flags.h>
#include <rex/ui/keybinds.h>
#include <rex/ui/imgui_dialog.h>
#include <rex/graphics/command_processor.h>
#include <rex/graphics/graphics_system.h>
#include <rex/system/function_dispatcher.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xam/content_manager.h>
#include <rex/logging/macros.h>

#include "native_renderer/dante_graphics_system.h"

#include <array>
#include <chrono>
#include <cstring>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <set>
#include <string>
#include <thread>
#include <vector>

REXCVAR_DEFINE_DOUBLE(time_scalar, 1.0, "Gameplay",
                      "Guest time scaling factor (1.0 = normal, 50.0 = fast-forward)");

REXCVAR_DEFINE_BOOL(show_fps_overlay, false, "UI",
                    "Show FPS and frametime overlay (top-left corner)");

REXCVAR_DEFINE_STRING(glyph_family, "auto", "UI",
                      "Button glyph family: auto, xbox, or playstation");

REXCVAR_DEFINE_DOUBLE(ultrawide_target_aspect, 0.0, "Graphics",
                      "Target aspect ratio for ultrawide (0=disabled, 1.7778=16:9, "
                      "2.3889=21:9, 3.5556=32:9)");

REXCVAR_DEFINE_BOOL(dlc_trace, false, "Diagnostics",
                    "Trace DLC module reads, guest callers, and activation (requires restart).")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

REXCVAR_DEFINE_BOOL(dlc_dump_image, false, "Diagnostics",
                    "Dump the loaded guest image for offline DLC analysis (requires restart).")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

REXCVAR_DEFINE_STRING(dlc_source_path, "dlc", "Content",
                      "Folder scanned for DLC packages to auto-install on launch. "
                      "If empty or missing, the game runs without DLC.");

class FpsOverlayDialog : public rex::ui::ImGuiDialog {
 public:
  explicit FpsOverlayDialog(rex::ui::ImGuiDrawer* drawer,
                            rex::graphics::CommandProcessor* command_processor)
      : rex::ui::ImGuiDialog(drawer),
        command_processor_(command_processor),
        window_start_(std::chrono::steady_clock::now()),
        last_guest_frame_count_(command_processor ? command_processor->swap_counter() : 0),
        last_vblank_count_(command_processor ? command_processor->vblank_counter() : 0) {}

 protected:
  void OnDraw(ImGuiIO& io) override {
    auto now = std::chrono::steady_clock::now();

    uint64_t current_guest_frames =
        command_processor_ ? command_processor_->swap_counter() : 0;
    uint64_t current_vblanks =
        command_processor_ ? command_processor_->vblank_counter() : 0;
    window_frames_ += current_guest_frames - last_guest_frame_count_;
    window_vblanks_ += current_vblanks - last_vblank_count_;
    last_guest_frame_count_ = current_guest_frames;
    last_vblank_count_ = current_vblanks;

    double window_ms =
        std::chrono::duration<double, std::milli>(now - window_start_).count();
    if (window_ms >= kWindowMs) {
      double guest_fps = window_frames_ * 1000.0 / window_ms;
      double guest_ft_ms = window_frames_ ? window_ms / window_frames_ : 0.0;
      double vblank_hz = window_vblanks_ * 1000.0 / window_ms;

      if (smoothed_fps_ == 0.0) {
        smoothed_fps_ = guest_fps;
        smoothed_ft_ = guest_ft_ms;
        smoothed_vblank_hz_ = vblank_hz;
      } else {
        smoothed_fps_ = smoothed_fps_ * 0.5 + guest_fps * 0.5;
        smoothed_ft_ = smoothed_ft_ * 0.5 + guest_ft_ms * 0.5;
        smoothed_vblank_hz_ = smoothed_vblank_hz_ * 0.5 + vblank_hz * 0.5;
      }

      frame_history_[history_idx_] = static_cast<float>(guest_ft_ms);
      history_idx_ = (history_idx_ + 1) % kHistorySize;

      window_frames_ = 0;
      window_vblanks_ = 0;
      window_start_ = now;
    }

    ImGui::SetNextWindowPos(ImVec2(8, 8), ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(0.65f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(10, 8));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 4.0f);

    bool visible = true;
    if (ImGui::Begin("##fps_overlay", &visible,
                     ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                         ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
                         ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoNav |
                         ImGuiWindowFlags_NoSavedSettings |
                         ImGuiWindowFlags_AlwaysAutoResize |
                         ImGuiWindowFlags_NoFocusOnAppearing)) {
      ImGui::SetWindowFontScale(2.0f);
      ImU32 fps_color = smoothed_fps_ >= 55.0f ? IM_COL32(80, 255, 80, 255) :
                       smoothed_fps_ >= 30.0f ? IM_COL32(255, 220, 60, 255) :
                                                IM_COL32(255, 80, 80, 255);
      ImGui::PushStyleColor(ImGuiCol_Text, fps_color);
      ImGui::Text("%.0f FPS", smoothed_fps_);
      ImGui::PopStyleColor();
      ImGui::SetWindowFontScale(1.0f);

      ImGui::SetWindowFontScale(1.3f);
      ImGui::Text("%.1f ms", smoothed_ft_);
      ImGui::SetWindowFontScale(1.0f);

      ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(160, 160, 160, 255));
      ImGui::Text("vblank %.0f Hz", smoothed_vblank_hz_);
      ImGui::PopStyleColor();

      ImGui::Spacing();
      ImGui::PlotLines("##frametime", frame_history_.data(),
                       static_cast<int>(kHistorySize),
                       static_cast<int>(history_idx_), nullptr,
                       0.0f, 50.0f, ImVec2(220, 50));
    }
    ImGui::End();
    ImGui::PopStyleVar(2);
  }

 private:
  static constexpr size_t kHistorySize = 120;
  static constexpr double kWindowMs = 250.0;
  std::array<float, kHistorySize> frame_history_{};
  size_t history_idx_ = 0;
  rex::graphics::CommandProcessor* command_processor_;
  std::chrono::steady_clock::time_point window_start_;
  uint64_t last_guest_frame_count_;
  uint64_t last_vblank_count_;
  uint64_t window_frames_ = 0;
  uint64_t window_vblanks_ = 0;
  double smoothed_fps_ = 0.0;
  double smoothed_ft_ = 0.0;
  double smoothed_vblank_hz_ = 0.0;
};

class DantesInfernoApp : public rex::ReXApp {
 public:
  using rex::ReXApp::ReXApp;

  static std::unique_ptr<rex::ui::WindowedApp> Create(
      rex::ui::WindowedAppContext& ctx) {
    return std::unique_ptr<DantesInfernoApp>(new DantesInfernoApp(ctx, "dantes_inferno",
        PPCImageConfig));
  }

  void OnConfigurePaths(rex::PathConfig& paths) override {
    if (!paths.game_data_root.empty())
      return;

    const std::filesystem::path candidates[] = {
        rex::filesystem::GetExecutableFolder() / "game",
        std::filesystem::current_path() / "game",
    };
    for (const auto& candidate : candidates) {
      std::error_code ec;
      if (std::filesystem::is_directory(candidate, ec)) {
        paths.game_data_root = candidate;
        return;
      }
    }
  }

  void OnPreSetup(rex::RuntimeConfig& config) override {
    config.gpu_plugin = "xenos";

    REXCVAR_SET(input_backend, std::string("sdl"));

    auto keybind_default = [](const char* name, const char* value) {
      if (rex::cvar::GetFlagSource(name) == rex::cvar::Source::kDefault) {
        rex::cvar::SetFlagByName(name, value);
      }
    };
    keybind_default("mnk_mode", "true");
    keybind_default("mnk_mouse", "true");
    keybind_default("mnk_sensitivity", "1.5");
    keybind_default("keybind_a", "Space");
    keybind_default("keybind_b", "F");
    keybind_default("keybind_x", "LMB");
    keybind_default("keybind_y", "E");
    keybind_default("keybind_left_shoulder", "Q");
    keybind_default("keybind_right_shoulder", "RMB");
    keybind_default("keybind_left_trigger", "Shift");
    keybind_default("keybind_right_trigger", "Control");
    keybind_default("keybind_lstick_up", "W");
    keybind_default("keybind_lstick_down", "S");
    keybind_default("keybind_lstick_left", "A");
    keybind_default("keybind_lstick_right", "D");
    keybind_default("keybind_lstick_press", "X");
    keybind_default("keybind_rstick_up", "Up");
    keybind_default("keybind_rstick_down", "Down");
    keybind_default("keybind_rstick_left", "Left");
    keybind_default("keybind_rstick_right", "Right");
    keybind_default("keybind_rstick_press", "R");
    keybind_default("keybind_dpad_up", "Shift+Up");
    keybind_default("keybind_dpad_down", "Shift+Down");
    keybind_default("keybind_dpad_left", "Shift+Left");
    keybind_default("keybind_dpad_right", "Shift+Right");
    keybind_default("keybind_back", "Tab");
    keybind_default("keybind_start", "Escape");

    double target_aspect = REXCVAR_GET(ultrawide_target_aspect);
    if (target_aspect > 0.0) {
      g_ultrawide_target_aspect = static_cast<float>(target_aspect);
      REXLOG_INFO("ULTRAWIDE: target_aspect={:.4f}", target_aspect);
    } else {
      REXLOG_INFO("ULTRAWIDE: disabled (target_aspect={:.4f})", target_aspect);
    }
  }

  void OnPreLaunchModule() override {
    uint8_t* membase = runtime()->memory()->virtual_membase();

    auto* dispatcher = runtime()->function_dispatcher();
    const bool is_tu2 = dispatcher && dispatcher->GetFunction(0x82879110);
    uint32_t slot = is_tu2 ? 0x82CE68E4u : 0x82B101E4u;
    *reinterpret_cast<uint32_t*>(membase + slot) = 0u;

    if (REXCVAR_GET(dlc_dump_image)) {
      std::filesystem::path dump_path =
          std::filesystem::current_path() / "logs" / "guest_image.bin";
      std::filesystem::create_directories(dump_path.parent_path());
      FILE* f = fopen(dump_path.string().c_str(), "wb");
      if (f) {
        fwrite(membase + 0x82000000, 1, 0xD70000, f);
        fclose(f);
        REXLOG_INFO("DLC-MOD: dumped guest image to {}", dump_path.string());
      }
    }

    SeedShaderStorage();
  }

  void OnPostSetup() override {
    rex::chrono::Clock::set_guest_time_scalar(REXCVAR_GET(time_scalar));

    rex::cvar::RegisterChangeCallback("time_scalar",
        [](std::string_view, std::string_view new_value) {
          double scalar = std::stod(std::string(new_value));
          if (scalar < 0.0) scalar = 0.0;
          rex::chrono::Clock::set_guest_time_scalar(scalar);
        });

    rex::cvar::RegisterChangeCallback("ultrawide_target_aspect",
        [](std::string_view, std::string_view new_value) {
          g_ultrawide_target_aspect =
              static_cast<float>(std::stod(std::string(new_value)));
        });

    rex::ui::RegisterBind("bind_exit_game", "Alt+F4",
                          "Exit game to desktop", [this] {
      app_context().RequestDeferredQuit();
    });

    saved_vsync_ = rex::cvar::Query<bool>("vsync");

    if (REXCVAR_GET(show_fps_overlay) && imgui_drawer()) {
      fps_overlay_ = std::make_unique<FpsOverlayDialog>(imgui_drawer(),
                                                        GetCommandProcessor());
    }

    rex::ui::RegisterBind("bind_fps_overlay", "F1",
                          "Toggle FPS overlay", [this] {
      if (fps_overlay_) {
        fps_overlay_.reset();
      } else if (imgui_drawer()) {
        fps_overlay_ = std::make_unique<FpsOverlayDialog>(imgui_drawer(),
                                                          GetCommandProcessor());
      }
    });

    rex::ui::RegisterBind("bind_pass_trace", "F6",
                          "Trace GPU passes for 5 seconds", [this] {
      bool ok = rex::cvar::SetFlagByName("gpu_pass_trace_frames", "300");
      REXLOG_INFO("PASS-TRACE trigger: set={} value={}", ok,
                  rex::cvar::Query<int32_t>("gpu_pass_trace_frames"));
    });

    rex::ui::RegisterBind("bind_d3d_trace", "F8",
                          "Trace D3D API calls for 3 frames", [this] {
      bool ok = rex::cvar::SetFlagByName("d3d_trace_frames", "3");
      REXLOG_INFO("D3D-TRACE trigger: set={}", ok);
    });

    rex::ui::RegisterBind("bind_native_observe", "F9",
                          "M2 shadow observer for 3 frames", [this] {
      bool ok = rex::cvar::SetFlagByName("native_observe_frames", "3");
      REXLOG_INFO("NATIVE-OBS trigger: set={}", ok);
    });

    rex::ui::RegisterBind("bind_fast_forward", "F2",
                          "Toggle 50x fast-forward", [this] {
      double current = REXCVAR_GET(time_scalar);
      bool fast = current > 1.0;
      double target = fast ? 1.0 : 50.0;
      rex::cvar::SetFlagByName("time_scalar", std::to_string(target));
      rex::chrono::Clock::set_guest_time_scalar(target);
      if (!fast) {
        saved_vsync_ = rex::cvar::Query<bool>("vsync");
      }
      rex::cvar::SetFlagByName("vsync", fast ? (saved_vsync_ ? "true" : "false") : "false");
    });

    AutoInstallDlc();
  }

  void AutoInstallDlc() {
    auto* kernel_state = runtime() ? runtime()->kernel_state() : nullptr;
    auto* content_manager = kernel_state ? kernel_state->content_manager() : nullptr;
    if (!content_manager) {
      REXLOG_WARN("DLC auto-install skipped: content manager unavailable");
      return;
    }

    std::string dlc_path = REXCVAR_GET(dlc_source_path);
    if (dlc_path.empty()) dlc_path = "dlc";
    std::filesystem::path root(dlc_path);
    if (!std::filesystem::exists(root)) {
      REXLOG_INFO("DLC folder not found ({}); running without DLC", dlc_path);
      return;
    }

    auto is_hex_name = [](const std::string& s, size_t len) {
      return s.size() == len &&
             s.find_first_not_of("0123456789abcdefABCDEF") == std::string::npos;
    };

    std::filesystem::path marker = root / ".installed";
    std::set<std::string> installed;
    if (std::filesystem::exists(marker)) {
      std::ifstream in(marker);
      std::string line;
      while (std::getline(in, line)) {
        if (!line.empty()) installed.insert(line);
      }
    }

    // Content layout is user_data_root/<xuid>/<title_id>/<content_type>/.
    // Mirror pre-extracted content trees (e.g. copied from Xenia's content
    // folder) directly; loose files are treated as STFS packages.
    const auto& user_root = user_data_root();
    const auto content_root = user_root / "0000000000000000";

    std::vector<std::filesystem::path> packages;
    int mirrored_dirs = 0;
    for (auto& entry : std::filesystem::directory_iterator(root)) {
      if (entry.is_regular_file()) {
        if (entry.path().filename() != ".installed") {
          packages.push_back(entry.path());
        }
        continue;
      }
      if (!entry.is_directory()) continue;

      const auto name = entry.path().filename().string();
      std::filesystem::path dest;
      if (is_hex_name(name, 16)) {
        dest = user_root / name;
      } else if (is_hex_name(name, 8) &&
                 std::filesystem::exists(entry.path() / "Headers")) {
        dest = content_root / name;
      }
      if (!dest.empty()) {
        std::error_code ec;
        std::filesystem::create_directories(dest, ec);
        std::filesystem::copy(entry.path(), dest,
            std::filesystem::copy_options::recursive |
            std::filesystem::copy_options::update_existing, ec);
        if (ec) {
          REXLOG_WARN("DLC content mirror failed for {}: {}", name, ec.message());
        } else {
          REXLOG_INFO("Mirrored DLC content directory: {}", name);
          mirrored_dirs++;
        }
        continue;
      }

      for (auto& sub : std::filesystem::recursive_directory_iterator(entry.path())) {
        if (sub.is_regular_file()) packages.push_back(sub.path());
      }
    }

    if (packages.empty() && mirrored_dirs == 0) {
      REXLOG_INFO("DLC folder empty ({}); running without DLC", dlc_path);
      return;
    }

    int new_installed = 0;
    for (auto& pkg : packages) {
      auto rel = std::filesystem::relative(pkg, root).string();
      if (installed.count(rel)) continue;

      REXLOG_INFO("Installing DLC package: {}", rel);
      auto result = content_manager->InstallContent(pkg);
      if (XSUCCEEDED(result)) {
        installed.insert(rel);
        new_installed++;
        std::ofstream out(marker, std::ios::app);
        out << rel << "\n";
      } else {
        REXLOG_WARN("DLC install failed for {}: 0x{:08X}", rel, result);
      }
    }

    REXLOG_INFO("DLC auto-install complete: {} new packages, {} mirrored dirs, {} total",
                new_installed, mirrored_dirs, installed.size());
  }

  void OnShutdown() override {
    rex::ui::UnregisterBind("bind_fast_forward");
    rex::ui::UnregisterBind("bind_pass_trace");
    rex::ui::UnregisterBind("bind_d3d_trace");
    rex::ui::UnregisterBind("bind_native_observe");
    rex::ui::UnregisterBind("bind_fps_overlay");
    rex::ui::UnregisterBind("bind_exit_game");
    rex::cvar::UnregisterChangeCallbacks("time_scalar");
    rex::cvar::UnregisterChangeCallbacks("ultrawide_target_aspect");
    fps_overlay_.reset();
    rex::chrono::Clock::set_guest_time_scalar(1.0);
    rex::cvar::SetFlagByName("vsync", saved_vsync_ ? "true" : "false");
  }

 private:
  void SeedShaderStorage() {
    const std::filesystem::path bundled =
        rex::filesystem::GetExecutableFolder() / "shader_cache";
    std::error_code ec;
    if (!std::filesystem::is_directory(bundled, ec) ||
        runtime()->cache_root().empty()) {
      return;
    }
    const std::filesystem::path shareable =
        runtime()->cache_root() / "shaders" / "shareable";
    std::filesystem::create_directories(shareable, ec);
    uint32_t seeded = 0;
    for (const auto& entry :
         std::filesystem::directory_iterator(bundled, ec)) {
      if (!entry.is_regular_file()) continue;
      const std::filesystem::path dst = shareable / entry.path().filename();
      if (std::filesystem::exists(dst, ec)) continue;
      if (std::filesystem::copy_file(entry.path(), dst, ec) && !ec) {
        ++seeded;
      }
    }
    if (seeded) {
      REXLOG_INFO("Seeded {} shader cache file(s) into {}", seeded,
                  shareable.string());
    }
  }

  rex::graphics::CommandProcessor* GetCommandProcessor() {
    auto* gfx_sys = runtime() ? runtime()->graphics_system() : nullptr;
    if (auto* dante_sys =
            dynamic_cast<dante::DanteGraphicsSystem*>(gfx_sys)) {
      gfx_sys = dante_sys->inner();
    }
    auto* con_sys =
        dynamic_cast<rex::graphics::GraphicsSystem*>(gfx_sys);
    return con_sys ? con_sys->command_processor() : nullptr;
  }

  std::unique_ptr<FpsOverlayDialog> fps_overlay_;
  bool saved_vsync_ = true;
};
