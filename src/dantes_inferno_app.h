#pragma once

#include <cstdio>

#include "overlay/achievement_sound.h"
#include "path_util.h"
#include "overlay/achievement_ui.h"
#include "overlay/fps_overlay.h"
#include "overlay/overlay_style.h"
#include "overlay/shader_progress.h"
#include "renderer/renderer.h"
#include "renderer/renderer_config.h"
#include "renderer/ui_overlay.h"
#include "crash_log.h"
#include "session_info.h"
#include "timer_resolution.h"
#include "user_paths.h"

#include <rex/chrono/clock.h>
#include <rex/cvar.h>
#include <rex/filesystem.h>
#include <rex/graphics/command_processor.h>
#include <rex/graphics/graphics_system.h>
#include <rex/input/flags.h>
#include <rex/input/input_system.h>
#include <rex/logging/macros.h>
#include <rex/memory/utils.h>
#include <rex/rex_app.h>
#include <rex/system.h>
#include <rex/system/function_dispatcher.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xthread.h>
#include <rex/system/xam/content_manager.h>
#include <rex/ui/imgui_dialog.h>
#include <rex/ui/keybinds.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
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

REXCVAR_DEFINE_BOOL(achievement_preview, false, "UI",
                    "Show the first achievement's notification once (tests the toast and sound)");

REXCVAR_DEFINE_STRING(glyph_family, "auto", "UI",
                      "Button glyphs: auto (follow the controller in use), xbox360, playstation4, "
                      "playstation5 or keyboard");

REXCVAR_DEFINE_STRING(render_resolution, "", "Graphics",
                      "Game render size WxH up to 1280x720, e.g. 960x540 (empty = 1280x720)")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

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

class DantesInfernoApp : public rex::ReXApp {
 public:
  using rex::ReXApp::ReXApp;

  static std::unique_ptr<rex::ui::WindowedApp> Create(rex::ui::WindowedAppContext& ctx) {
    return std::unique_ptr<DantesInfernoApp>(
        new DantesInfernoApp(ctx, "dantes_inferno", PPCImageConfig));
  }

  void OnConfigurePaths(rex::PathConfig& paths) override {
    user_paths::Configure(paths);
    if (!paths.game_data_root.empty()) return;

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

  void OnConfigureFonts(ImFontAtlas* atlas) override { overlay::LoadFonts(atlas, window()); }

  std::unique_ptr<rex::ui::ImGuiDialog> CreateAchievementsOverlay() override {
    if (!runtime() || !runtime()->kernel_state() || !imgui_drawer()) return nullptr;
    return std::make_unique<overlay::AchievementList>(imgui_drawer(), IconDrawer(), runtime(),
                                                      &achievements());
  }

  std::unique_ptr<rex::ui::AchievementNotificationDialog> CreateAchievementNotificationDialog()
      override {
    if (!imgui_drawer() || !runtime()) return nullptr;
    auto toast =
        std::make_unique<overlay::AchievementToast>(imgui_drawer(), IconDrawer(), runtime());
    achievement_toast_ = toast.get();
    return toast;
  }

  void OnPreSetup(rex::RuntimeConfig& config) override {
    crash_log::Install();
    config.gpu_plugin = "xenos";

    REXCVAR_SET(input_backend, std::string("sdl"));

    // Keyboard / mouse layout, unless the user or the launcher set one.
    static constexpr std::pair<const char*, const char*> kKeybindDefaults[] = {
        {"mnk_mode", "true"},
        {"mnk_mouse", "true"},
        {"mnk_sensitivity", "1.5"},
        {"keybind_a", "Space"},
        {"keybind_b", "F"},
        {"keybind_x", "LMB"},
        {"keybind_y", "E"},
        {"keybind_left_shoulder", "Q"},
        {"keybind_right_shoulder", "RMB"},
        {"keybind_left_trigger", "Shift"},
        {"keybind_right_trigger", "Control"},
        {"keybind_lstick_up", "W"},
        {"keybind_lstick_down", "S"},
        {"keybind_lstick_left", "A"},
        {"keybind_lstick_right", "D"},
        {"keybind_lstick_press", "X"},
        {"keybind_rstick_up", "Up"},
        {"keybind_rstick_down", "Down"},
        {"keybind_rstick_left", "Left"},
        {"keybind_rstick_right", "Right"},
        {"keybind_rstick_press", "R"},
        {"keybind_dpad_up", "Shift+Up"},
        {"keybind_dpad_down", "Shift+Down"},
        {"keybind_dpad_left", "Shift+Left"},
        {"keybind_dpad_right", "Shift+Right"},
        {"keybind_back", "Tab"},
        {"keybind_start", "Escape"},
    };
    for (const auto& [name, value] : kKeybindDefaults) {
      if (rex::cvar::GetFlagSource(name) == rex::cvar::Source::kDefault) {
        rex::cvar::SetFlagByName(name, value);
      }
    }

    double target_aspect = REXCVAR_GET(ultrawide_target_aspect);
    if (target_aspect > 0.0) g_ultrawide_target_aspect = static_cast<float>(target_aspect);
    {
      unsigned w = 0, h = 0;
      if (std::sscanf(REXCVAR_GET(render_resolution).c_str(), "%ux%u", &w, &h) == 2) {
        g_render_width = w;
        g_render_height = h;
      }
    }
    REXLOG_INFO("ULTRAWIDE: target_aspect={:.4f}", target_aspect);
  }

  // The game starts once the renderer has built the recorded shaders and
  // pipelines (renderer::Start). Elsewhere its first draw waits for them.
  void OnPostLaunchModule(rex::system::XThread* thread) override {
#if REX_PLATFORM_WIN32
    if (thread && renderer::GetPrecompileStatus().active) {
      thread->Suspend();
      bool waiting = renderer::RunWhenPrecompiled([this, thread] {
        thread->Resume();
        app_context().CallInUIThread([this] {
          if (!shutting_down_) shader_progress_.reset();
        });
      });
      if (waiting) return;
      thread->Resume();
    }
#else
    (void)thread;
    renderer::RunWhenPrecompiled([this] {
      app_context().CallInUIThread([this] {
        if (!shutting_down_) shader_progress_.reset();
      });
    });
#endif
    if (!renderer::GetPrecompileStatus().active) shader_progress_.reset();
  }

  void OnPreLaunchModule() override {
    uint8_t* membase = runtime()->memory()->virtual_membase();
    g_guest_membase = membase;

    if (rex::memory::load_and_swap<uint32_t>(membase + 0x82879110) != 0x3C8082CEu) {
      auto msg = fmt::format(
          "Title Update 2 was not applied.\n\ndefault.xexp is missing or does not match "
          "default.xex in:\n{}\n\nStart the game from the launcher, which puts it in place, or "
          "copy tu\\default.xexp from the release into that folder.",
          path_util::Utf8(game_data_root()));
      REXLOG_CRITICAL("{}", msg);
      if (auto logger = rex::GetLogger()) logger->flush();
      rex::ShowSimpleMessageBox(rex::SimpleMessageBoxType::Error, msg);
      std::_Exit(1);
    }

    // The save system's setjmp must not switch fibers (dantes_inferno_hooks.h):
    // clear the fiber-switch callback.
    *reinterpret_cast<uint32_t*>(membase + 0x82CE68E4u) = 0u;

    if (REXCVAR_GET(dlc_dump_image)) {
      std::filesystem::path dump_path =
          std::filesystem::current_path() / "logs" / "guest_image.bin";
      std::filesystem::create_directories(dump_path.parent_path());
      FILE* f = path_util::Open(dump_path, "wb");
      if (f) {
        fwrite(membase + 0x82000000, 1, 0xD70000, f);
        fclose(f);
        REXLOG_INFO("DLC-MOD: dumped guest image to {}", path_util::Utf8(dump_path));
      }
    }
  }

  void OnPostSetup() override {
    session_info::Log();
    crash_log::LogGameFiles(game_data_root());
    if (auto* input = dynamic_cast<rex::input::InputSystem*>(runtime()->input_system())) {
      input->SetDeviceAssignment(std::make_unique<rex::input::SharedAssignment>());
    }
    timer_resolution::Apply();
    user_paths::Report(user_data_root());
    renderer::ConfigureSyncOnlyGpu();
    if (rex::cvar::Query<std::string>("renderer") == "dante") {
      renderer::Start(window(), cache_root());
      // The SDK presenter is detached: ImGui records on the UI thread and the
      // renderer draws the overlay over each presented frame.
      if (imgui_drawer()) {
        overlay_drawer_ = std::make_unique<renderer::OverlayDrawer>();
        imgui_drawer()->SetPresenterAndImmediateDrawer(nullptr, overlay_drawer_.get());
        renderer::SetOverlayFrameRequest([this] {
          if (overlay_frame_pending_.exchange(true)) return;
          app_context().CallInUIThread([this] {
            overlay_frame_pending_ = false;
            DrawOverlayFrame();
          });
        });
        if (renderer::GetPrecompileStatus().active) {
          shader_progress_ = std::make_unique<overlay::ShaderProgress>(imgui_drawer(), [] {
            renderer::PrecompileStatus s = renderer::GetPrecompileStatus();
            return overlay::ShaderProgress::Status{s.active,         s.pipelines,
                                                   s.shaders_done,   s.shaders_total,
                                                   s.pipelines_done, s.pipelines_total,
                                                   s.start};
          });
        }
      }
      // renderer=dante sets the aspect from a resolution wider than 16:9.
      g_ultrawide_target_aspect = static_cast<float>(REXCVAR_GET(ultrawide_target_aspect));
      // Button glyphs follow the controller in use, unless glyph_family forces a set.
      auto* input = dynamic_cast<rex::input::InputSystem*>(runtime()->input_system());
      renderer::SetButtonGlyphStyleSource([input] {
        using renderer::GlyphStyle;
        std::string family = REXCVAR_GET(glyph_family);
        if (family == "xbox360") return GlyphStyle::kXbox;
        if (family == "playstation4") return GlyphStyle::kPlayStation4;
        if (family == "playstation5") return GlyphStyle::kPlayStation5;
        if (family == "keyboard") return GlyphStyle::kKeyboard;
        if (!input) return GlyphStyle::kXbox;
        switch (input->ActiveButtonLabels(0)) {
          case rex::input::ButtonLabels::kPlayStation4: return GlyphStyle::kPlayStation4;
          case rex::input::ButtonLabels::kPlayStation5: return GlyphStyle::kPlayStation5;
          case rex::input::ButtonLabels::kKeyboard: return GlyphStyle::kKeyboard;
          default: return GlyphStyle::kXbox;
        }
      });
      // Keyboard glyphs show the live keybinds (the launcher and the settings
      // overlay can change them).
      renderer::SetButtonGlyphBindingsSource([] {
        auto bind = [](const char* name) { return rex::cvar::Query<std::string>(name); };
        renderer::KeyBindings k;
        k.a = bind("keybind_a");
        k.b = bind("keybind_b");
        k.x = bind("keybind_x");
        k.y = bind("keybind_y");
        k.left_shoulder = bind("keybind_left_shoulder");
        k.right_shoulder = bind("keybind_right_shoulder");
        k.left_trigger = bind("keybind_left_trigger");
        k.right_trigger = bind("keybind_right_trigger");
        k.back = bind("keybind_back");
        k.start = bind("keybind_start");
        k.lstick_up = bind("keybind_lstick_up");
        k.lstick_down = bind("keybind_lstick_down");
        k.lstick_left = bind("keybind_lstick_left");
        k.lstick_right = bind("keybind_lstick_right");
        k.lstick_press = bind("keybind_lstick_press");
        k.rstick_up = bind("keybind_rstick_up");
        k.rstick_down = bind("keybind_rstick_down");
        k.rstick_left = bind("keybind_rstick_left");
        k.rstick_right = bind("keybind_rstick_right");
        k.rstick_press = bind("keybind_rstick_press");
        k.mouse_look = rex::cvar::Query<bool>("mnk_mouse");
        return k;
      });
    }
    rex::chrono::Clock::set_guest_time_scalar(REXCVAR_GET(time_scalar));

    rex::cvar::RegisterChangeCallback("time_scalar",
                                      [](std::string_view, std::string_view new_value) {
                                        double scalar = std::stod(std::string(new_value));
                                        if (scalar < 0.0) scalar = 0.0;
                                        rex::chrono::Clock::set_guest_time_scalar(scalar);
                                      });

    rex::cvar::RegisterChangeCallback(
        "ultrawide_target_aspect", [](std::string_view, std::string_view new_value) {
          g_ultrawide_target_aspect = static_cast<float>(std::stod(std::string(new_value)));
        });

    rex::ui::RegisterBind("bind_exit_game", "Alt+F4", "Exit game to desktop",
                          [this] { app_context().RequestDeferredQuit(); });

    saved_vsync_ = rex::cvar::Query<bool>("vsync");

    if (REXCVAR_GET(show_fps_overlay)) CreateFpsOverlay();

#if defined(__ANDROID__)
    // Android: log the guest frame rate every 5 s (the Java side logs per-thread CPU use).
    perf_thread_ = std::thread([this] {
      uint32_t last = 0;
      auto last_t = std::chrono::steady_clock::now();
      // Polled every 2 ms so single long frames show up, not only the 5 s average.
      uint32_t seen = 0;
      auto seen_t = last_t;
      double worst_ms = 0;
      uint32_t slow = 0;
      while (!shutting_down_.load()) {
        auto* cp = GetCommandProcessor();
        for (int i = 0; i < 2500 && !shutting_down_.load(); ++i) {
          std::this_thread::sleep_for(std::chrono::milliseconds(2));
          if (!cp) continue;
          uint32_t c = cp->swap_counter();
          if (c == seen) continue;
          auto t = std::chrono::steady_clock::now();
          if (seen != 0) {
            double ms = std::chrono::duration<double, std::milli>(t - seen_t).count() / (c - seen);
            worst_ms = std::max(worst_ms, ms);
            if (ms > 50.0) ++slow;
          }
          seen = c;
          seen_t = t;
        }
        if (!cp) continue;
        uint32_t now = cp->swap_counter();
        auto t = std::chrono::steady_clock::now();
        double secs = std::chrono::duration<double>(t - last_t).count();
        if (last != 0 && secs > 0) {
          REXLOG_INFO("PERF: guest {:.1f} fps ({} frames in {:.1f} s), worst frame {:.0f} ms, "
                      "{} over 50 ms",
                      (now - last) / secs, now - last, secs, worst_ms, slow);
        }
        worst_ms = 0;
        slow = 0;
        last = now;
        last_t = t;
      }
    });
#endif

    rex::ui::RegisterBind("bind_fps_overlay", "F1", "Toggle FPS overlay", [this] {
      if (fps_overlay_) {
        fps_overlay_.reset();
      } else {
        CreateFpsOverlay();
      }
    });

    rex::ui::RegisterBind("bind_pass_trace", "F6", "Trace GPU passes for 5 seconds", [] {
      bool ok = rex::cvar::SetFlagByName("gpu_pass_trace_frames", "300");
      REXLOG_INFO("PASS-TRACE trigger: set={} value={}", ok,
                  rex::cvar::Query<int32_t>("gpu_pass_trace_frames"));
    });

    rex::ui::RegisterBind("bind_d3d_trace", "F8", "Trace D3D API calls for 3 frames", [] {
      bool ok = rex::cvar::SetFlagByName("d3d_trace_frames", "3");
      REXLOG_INFO("D3D-TRACE trigger: set={}", ok);
    });

    rex::ui::RegisterBind(
        "bind_host_rendering", "F10", "Toggle Xenos host rendering (sync keeps running)", [] {
          bool enable = !rex::cvar::Query<bool>("gpu_host_rendering");
          bool ok = rex::cvar::SetFlagByName("gpu_host_rendering", enable ? "true" : "false");
          REXLOG_INFO("gpu_host_rendering={} (set={})", enable, ok);
        });

    rex::ui::RegisterBind("bind_dante_debug_view", "F11",
                          "renderer=dante: cycle the presented render target", [] {
                            int32_t view = (rex::cvar::Query<int32_t>("dante_debug_view") + 1) % 9;
                            rex::cvar::SetFlagByName("dante_debug_view", std::to_string(view));
                            REXLOG_INFO("dante_debug_view={}", view);
                          });

    rex::ui::RegisterBind("bind_rdoc_capture", "Insert",
                          "Under RenderDoc: capture the next guest frames",
                          [] { rex::cvar::SetFlagByName("rdoc_capture_frame", "true"); });

    rex::ui::RegisterBind("bind_dante_smaa", "End", "renderer=dante: toggle SMAA", [] {
      bool enabled = !rex::cvar::Query<bool>("dante_smaa");
      rex::cvar::SetFlagByName("dante_smaa", enabled ? "true" : "false");
      REXLOG_INFO("SMAA {}", enabled ? "on" : "off");
    });

    rex::ui::RegisterBind("bind_dante_replay", "Home",
                          "renderer=dante: save the instant replay (last 3 seconds)",
                          [] { rex::cvar::SetFlagByName("dante_replay_dump", "true"); });

    rex::ui::RegisterBind(
        "bind_dante_record", "F5", "renderer=dante: record the next 3600 presented frames", [] {
          bool start = rex::cvar::Query<int32_t>("dante_record_frames") == 0;
          rex::cvar::SetFlagByName("dante_record_frames", start ? "3600" : "0");
          REXLOG_INFO("dante_record_frames: {}", start ? "recording 3600 frames" : "stopped");
        });

#ifdef DANTESINFERNO_DEV_CHEATS
    // Development cheats (src/damage_hooks.cpp).
    auto toggle_cheat = [](const char* cvar, const char* label) {
      bool enabled = !rex::cvar::Query<bool>(cvar);
      rex::cvar::SetFlagByName(cvar, enabled ? "true" : "false");
      REXLOG_INFO("CHEAT {} {}", label, enabled ? "on" : "off");
    };
    rex::ui::RegisterBind("bind_cheat_god", "PageUp", "Cheat: toggle no damage or hits on the player",
                          [toggle_cheat] { toggle_cheat("dante_cheat_god", "god mode"); });
    rex::ui::RegisterBind("bind_cheat_one_hit", "PageDown", "Cheat: toggle one-hit kills",
                          [toggle_cheat] { toggle_cheat("dante_cheat_one_hit", "one-hit kills"); });
#endif

    rex::ui::RegisterBind("bind_verify_gpu_state", "F9", "Verify the GPU state of 3 frames", [] {
      bool ok = rex::cvar::SetFlagByName("verify_gpu_state_frames", "3");
      REXLOG_INFO("NATIVE-OBS trigger: set={}", ok);
    });

    rex::ui::RegisterBind("bind_fast_forward", "F2", "Toggle 50x fast-forward", [this] {
      bool fast = REXCVAR_GET(time_scalar) > 1.0;
      double target = fast ? 1.0 : 50.0;
      rex::cvar::SetFlagByName("time_scalar", std::to_string(target));
      rex::chrono::Clock::set_guest_time_scalar(target);
      if (!fast) saved_vsync_ = rex::cvar::Query<bool>("vsync");
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
      return s.size() == len && s.find_first_not_of("0123456789abcdefABCDEF") == std::string::npos;
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

      const auto name = path_util::Utf8(entry.path().filename());
      std::filesystem::path dest;
      if (is_hex_name(name, 16)) {
        dest = user_root / name;
      } else if (is_hex_name(name, 8) && std::filesystem::exists(entry.path() / "Headers")) {
        dest = content_root / name;
      }
      if (!dest.empty()) {
        std::error_code ec;
        std::filesystem::create_directories(dest, ec);
        std::filesystem::copy(entry.path(), dest,
                              std::filesystem::copy_options::recursive |
                                  std::filesystem::copy_options::update_existing,
                              ec);
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
      auto rel = path_util::Utf8(std::filesystem::relative(pkg, root));
      if (installed.count(rel)) continue;

      std::array<uint8_t, 0x364> header{};
      std::ifstream file(pkg, std::ios::binary);
      file.read(reinterpret_cast<char*>(header.data()), header.size());
      auto be32 = [&](size_t offset) {
        return uint32_t(header[offset]) << 24 | uint32_t(header[offset + 1]) << 16 |
               uint32_t(header[offset + 2]) << 8 | uint32_t(header[offset + 3]);
      };
      const std::string magic(reinterpret_cast<const char*>(header.data()), 4);
      if (file.gcount() != std::streamsize(header.size()) ||
          (magic != "LIVE" && magic != "PIRS" && magic != "CON ") || be32(0x344) != 0x2 ||
          be32(0x360) != 0x454108CF) {
        REXLOG_INFO("Not Dante's Inferno DLC, skipped: {} (content type {:08X}, title {:08X})",
                    rel, be32(0x344), be32(0x360));
        continue;
      }

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
    shutting_down_ = true;
    if (perf_thread_.joinable()) perf_thread_.join();
    renderer::SetOverlayFrameRequest(nullptr);
    renderer::Stop();
    overlay::ShutdownAchievementSound();
    for (const char* bind : kBinds) rex::ui::UnregisterBind(bind);
    rex::cvar::UnregisterChangeCallbacks("time_scalar");
    rex::cvar::UnregisterChangeCallbacks("ultrawide_target_aspect");
    fps_overlay_.reset();
    shader_progress_.reset();
    rex::chrono::Clock::set_guest_time_scalar(1.0);
    rex::cvar::SetFlagByName("vsync", saved_vsync_ ? "true" : "false");
  }

 private:
  static constexpr const char* kBinds[] = {
      "bind_exit_game",      "bind_fps_overlay",      "bind_pass_trace",   "bind_d3d_trace",
      "bind_host_rendering", "bind_dante_debug_view", "bind_rdoc_capture", "bind_dante_smaa",
      "bind_dante_replay",   "bind_dante_record",     "bind_cheat_god",    "bind_cheat_one_hit",
      "bind_verify_gpu_state", "bind_fast_forward",
  };

  // Achievement icons are textures of the drawer that draws ImGui.
  rex::ui::ImmediateDrawer* IconDrawer() const {
    return overlay_drawer_ ? overlay_drawer_.get() : immediate_drawer();
  }

  void CreateFpsOverlay() {
    if (!imgui_drawer()) return;
    std::function<overlay::FpsOverlay::Sample()> counter;
    if (overlay_drawer_) {
      counter = [] {
        renderer::PresentCount count = renderer::PresentedFrames();
        return overlay::FpsOverlay::Sample{count.frames, count.last};
      };
    } else {
      counter = [cp = GetCommandProcessor()] {
        return overlay::FpsOverlay::Sample{cp ? cp->swap_counter() : 0,
                                           std::chrono::steady_clock::now()};
      };
    }
    fps_overlay_ = std::make_unique<overlay::FpsOverlay>(imgui_drawer(), std::move(counter));
  }

  // UI thread, after each presented frame (renderer=dante).
  void DrawOverlayFrame() {
    if (shutting_down_ || !overlay_drawer_ || !imgui_drawer() || !window()) return;
    if (REXCVAR_GET(achievement_preview) && achievement_toast_) {
      REXCVAR_SET(achievement_preview, false);
      auto list = achievements().ListAchievements();
      if (!list.empty()) achievement_toast_->Push({list.front(), 0});
    }
    if (!imgui_drawer()->HasDialogs()) {
      overlay_drawer_->PublishEmpty();
      return;
    }
    rex::ui::AppUIDrawContext context(window()->GetActualPhysicalWidth(),
                                      window()->GetActualPhysicalHeight());
    imgui_drawer()->Draw(context);
  }

  rex::graphics::CommandProcessor* GetCommandProcessor() {
#if defined(__ANDROID__)
    // GraphicsSystem lives in the rexgpu-xenos plugin (hidden typeinfo): no RTTI.
    auto* graphics = static_cast<rex::graphics::GraphicsSystem*>(
        runtime() ? runtime()->graphics_system() : nullptr);
#else
    auto* graphics = dynamic_cast<rex::graphics::GraphicsSystem*>(
        runtime() ? runtime()->graphics_system() : nullptr);
#endif
    return graphics ? graphics->command_processor() : nullptr;
  }

  std::unique_ptr<overlay::FpsOverlay> fps_overlay_;
  std::unique_ptr<overlay::ShaderProgress> shader_progress_;
  std::unique_ptr<renderer::OverlayDrawer> overlay_drawer_;
  overlay::AchievementToast* achievement_toast_ = nullptr;
  std::atomic<bool> overlay_frame_pending_{false};
  std::atomic<bool> shutting_down_{false};
  std::thread perf_thread_;
  bool saved_vsync_ = true;
};
