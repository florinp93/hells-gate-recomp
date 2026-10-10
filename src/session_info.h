#pragma once

// Session header for the log: port version, command line, system, display and
// the main settings, so a log on its own describes the session it came from.

#include <rex/cvar.h>
#include <rex/filesystem.h>
#include <rex/logging/macros.h>

#include <fmt/format.h>

#include <cstdint>
#include <cstring>
#include <fstream>
#include <string>
#include <thread>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <intrin.h>
#endif

namespace session_info {

inline std::string PortVersion() {
  std::ifstream file(rex::filesystem::GetExecutableFolder() / "version.txt");
  std::string version;
  std::getline(file, version);
  while (!version.empty() && (version.back() == '\r' || version.back() == ' ')) version.pop_back();
  return version.empty() ? "unknown (no version.txt)" : version;
}

#ifdef _WIN32
inline std::string Narrow(const wchar_t* text) {
  int size = WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
  if (size <= 1) return {};
  std::string out(size - 1, '\0');
  WideCharToMultiByte(CP_UTF8, 0, text, -1, out.data(), size, nullptr, nullptr);
  return out;
}

inline std::string WindowsVersion() {
  using RtlGetVersionFn = LONG(WINAPI*)(OSVERSIONINFOW*);
  auto rtl_get_version = reinterpret_cast<RtlGetVersionFn>(
      GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion"));
  OSVERSIONINFOW info = {sizeof(info)};
  if (!rtl_get_version || rtl_get_version(&info) != 0) return "unknown";
  return fmt::format("{}.{} build {}", info.dwMajorVersion, info.dwMinorVersion,
                     info.dwBuildNumber);
}

inline std::string CpuName() {
  int regs[4] = {};
  char brand[49] = {};
  __cpuid(regs, 0x80000000);
  if (unsigned(regs[0]) < 0x80000004u) return "unknown";
  for (int i = 0; i < 3; ++i) {
    __cpuid(regs, 0x80000002 + i);
    std::memcpy(brand + i * 16, regs, 16);
  }
  std::string name(brand);
  size_t first = name.find_first_not_of(' ');
  return first == std::string::npos ? "unknown" : name.substr(first);
}

inline std::string DisplayMode() {
  DEVMODEW mode = {};
  mode.dmSize = sizeof(mode);
  if (!EnumDisplaySettingsW(nullptr, ENUM_CURRENT_SETTINGS, &mode)) return "unknown";
  return fmt::format("{}x{} @ {} Hz", mode.dmPelsWidth, mode.dmPelsHeight,
                     mode.dmDisplayFrequency);
}
#endif

inline void Log() {
  REXLOG_INFO("SESSION: Dante's Inferno port {}", PortVersion());
#ifdef _WIN32
  REXLOG_INFO("SESSION: command line: {}", Narrow(GetCommandLineW()));
  MEMORYSTATUSEX memory = {sizeof(memory)};
  GlobalMemoryStatusEx(&memory);
  REXLOG_INFO("SESSION: Windows {}, CPU {} ({} threads), {} MB RAM", WindowsVersion(), CpuName(),
              std::thread::hardware_concurrency(), memory.ullTotalPhys >> 20);
  REXLOG_INFO("SESSION: primary display {}", DisplayMode());
#endif
  REXLOG_INFO("SESSION: renderer={} resolution={} aspect={} frame rate={} fullscreen={} "
              "anisotropic={}",
              rex::cvar::Query<std::string>("renderer"),
              rex::cvar::Query<std::string>("dante_resolution"),
              rex::cvar::Query<double>("ultrawide_target_aspect") > 0.0
                  ? fmt::format("{:.4f}", rex::cvar::Query<double>("ultrawide_target_aspect"))
                  : std::string("auto"),
              rex::cvar::Query<double>("video_mode_refresh_rate"),
              rex::cvar::Query<bool>("fullscreen"),
              rex::cvar::Query<int32_t>("anisotropic_override"));
}

}  // namespace session_info
