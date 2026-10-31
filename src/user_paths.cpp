#include "user_paths.h"
#include "path_util.h"

#include <rex/cvar.h>
#include <rex/filesystem.h>
#include <rex/logging/macros.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

namespace user_paths {
namespace {

enum class Choice { kDefault, kPortable, kDocumentsBlocked };
Choice g_choice = Choice::kDefault;
std::filesystem::path g_blocked_documents;

constexpr char kPortableMarker[] = "portable.txt";
constexpr char kBlockedNoticeMarker[] = "documents_blocked_notice_shown";

bool IsWritableFolder(const std::filesystem::path& dir) {
  std::error_code ec;
  std::filesystem::create_directories(dir, ec);
  auto probe = dir / ".write_test";
  bool ok = false;
  {
    std::ofstream file(probe, std::ios::binary | std::ios::trunc);
    ok = file && (file << 'x') && file.flush();
  }
  std::filesystem::remove(probe, ec);
  return ok;
}

}  // namespace

void Configure(rex::PathConfig& paths) {
  if (!rex::cvar::Query<std::string>("user_data_root").empty()) return;
  const bool own_cache = rex::cvar::Query<std::string>("cache_root").empty();
  const auto exe_dir = rex::filesystem::GetExecutableFolder();
  std::error_code ec;
  std::filesystem::path user_dir;
  if (std::filesystem::exists(exe_dir / kPortableMarker, ec)) {
    user_dir = exe_dir / "userdata";
    g_choice = Choice::kPortable;
  } else if (!IsWritableFolder(paths.user_data_root)) {
#ifdef _WIN32
    if (const char* local = std::getenv("LOCALAPPDATA"); local && *local) {
      user_dir = std::filesystem::path(local) / paths.user_data_root.filename();
      g_choice = Choice::kDocumentsBlocked;
      g_blocked_documents = paths.user_data_root;
    }
#endif
  }
  if (user_dir.empty()) return;
  paths.user_data_root = user_dir;
  if (own_cache) paths.cache_root = user_dir / "cache";
}

void Report(const std::filesystem::path& user_data_root) {
  switch (g_choice) {
    case Choice::kPortable:
      REXLOG_INFO("PATHS: portable mode ({} next to the exe): saves and cache in {}",
                  kPortableMarker, path_util::Utf8(user_data_root));
      return;
    case Choice::kDocumentsBlocked:
      REXLOG_WARN("PATHS: {} is not writable (Windows Controlled Folder Access?); saves and "
                  "cache in {}",
                  path_util::Utf8(g_blocked_documents), path_util::Utf8(user_data_root));
      break;
    default:
      REXLOG_INFO("PATHS: saves and cache in {}", path_util::Utf8(user_data_root));
      return;
  }
#ifdef _WIN32
  std::error_code ec;
  auto marker = user_data_root / kBlockedNoticeMarker;
  if (std::filesystem::exists(marker, ec)) return;
  std::ofstream(marker) << "shown\n";
  std::wstring text =
      L"Windows is blocking Dante's Inferno from writing to your Documents folder "
      L"(usually Controlled Folder Access in Windows Security).\n\n"
      L"Your saves and settings will be kept in:\n" +
      user_data_root.wstring() +
      L"\n\nSaves already in Documents are not used while it is blocked. To keep using them, "
      L"allow dantes_inferno.exe in Windows Security > Virus & threat protection > "
      L"Ransomware protection > Allow an app through Controlled folder access.";
  MessageBoxW(nullptr, text.c_str(), L"Dante's Inferno", MB_OK | MB_ICONINFORMATION);
#endif
}

}  // namespace user_paths
