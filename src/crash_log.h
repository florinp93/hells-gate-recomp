#pragma once

#include <rex/logging.h>
#include "path_util.h"
#include <rex/logging/macros.h>

#include <cstdint>
#include <exception>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <bcrypt.h>
#include <psapi.h>
#pragma comment(lib, "bcrypt.lib")
#endif

namespace crash_log {

#ifdef _WIN32

inline std::string ModuleOffset(const void* address) {
  HMODULE module = nullptr;
  if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                              GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                          static_cast<LPCWSTR>(address), &module) ||
      !module) {
    return "unknown module";
  }
  char path[MAX_PATH] = {};
  GetModuleFileNameA(module, path, MAX_PATH);
  std::string name = path_util::Utf8(std::filesystem::path(path).filename());
  return fmt::format("{}+0x{:X}", name,
                     reinterpret_cast<uintptr_t>(address) - reinterpret_cast<uintptr_t>(module));
}

inline void Report(EXCEPTION_POINTERS* info, const char* kind) {
  static volatile LONG reported = 0;
  if (InterlockedExchange(&reported, 1)) return;
  const EXCEPTION_RECORD* record = info->ExceptionRecord;
  std::string detail;
  if (record->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && record->NumberParameters >= 2) {
    const char* op = record->ExceptionInformation[0] == 0   ? "read"
                     : record->ExceptionInformation[0] == 8 ? "execute"
                                                            : "write";
    detail = fmt::format(", {} of 0x{:X}", op, record->ExceptionInformation[1]);
  }
  REXLOG_CRITICAL("CRASH ({}): exception 0x{:08X} at {} (0x{:X}) on thread {}{}", kind,
                  uint32_t(record->ExceptionCode), ModuleOffset(record->ExceptionAddress),
                  reinterpret_cast<uintptr_t>(record->ExceptionAddress), GetCurrentThreadId(),
                  detail);
  void* frames[32] = {};
  USHORT count = RtlCaptureStackBackTrace(0, 32, frames, nullptr);
  for (USHORT i = 0; i < count; ++i) {
    REXLOG_CRITICAL("CRASH stack {}: {}", i, ModuleOffset(frames[i]));
  }
  if (auto logger = rex::GetLogger()) logger->flush();
}

inline LONG WINAPI UnhandledFilter(EXCEPTION_POINTERS* info) {
  Report(info, "unhandled");
  return EXCEPTION_CONTINUE_SEARCH;
}

inline LONG WINAPI FailFastHandler(EXCEPTION_POINTERS* info) {
  DWORD code = info->ExceptionRecord->ExceptionCode;
  if (code == 0xC0000374 || code == 0xC0000409 || code == EXCEPTION_STACK_OVERFLOW) {
    Report(info, "fatal");
  }
  return EXCEPTION_CONTINUE_SEARCH;
}

inline void OnTerminate() {
  std::string what = "unknown";
  if (auto current = std::current_exception()) {
    try {
      std::rethrow_exception(current);
    } catch (const std::exception& e) {
      what = e.what();
    } catch (...) {
      what = "non-standard exception";
    }
  }
  REXLOG_CRITICAL("CRASH (uncaught C++ exception): {}", what);
  void* frames[32] = {};
  USHORT count = RtlCaptureStackBackTrace(0, 32, frames, nullptr);
  for (USHORT i = 0; i < count; ++i) {
    REXLOG_CRITICAL("CRASH stack {}: {}", i, ModuleOffset(frames[i]));
  }
  if (auto logger = rex::GetLogger()) logger->flush();
  std::abort();
}

inline void Install() {
  SetUnhandledExceptionFilter(UnhandledFilter);
  AddVectoredExceptionHandler(0, FailFastHandler);
  std::set_terminate(OnTerminate);
}

inline std::string Sha1(const std::filesystem::path& file) {
  std::ifstream in(file, std::ios::binary);
  if (!in) return "missing";
  BCRYPT_ALG_HANDLE alg = nullptr;
  BCRYPT_HASH_HANDLE hash = nullptr;
  if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA1_ALGORITHM, nullptr, 0) != 0) return "unknown";
  std::string out = "unknown";
  if (BCryptCreateHash(alg, &hash, nullptr, 0, nullptr, 0, 0) == 0) {
    std::vector<char> buffer(1 << 20);
    while (in.read(buffer.data(), buffer.size()) || in.gcount() > 0) {
      BCryptHashData(hash, reinterpret_cast<PUCHAR>(buffer.data()), ULONG(in.gcount()), 0);
    }
    UCHAR digest[20] = {};
    if (BCryptFinishHash(hash, digest, sizeof(digest), 0) == 0) {
      out.clear();
      for (UCHAR b : digest) out += fmt::format("{:02X}", b);
    }
    BCryptDestroyHash(hash);
  }
  BCryptCloseAlgorithmProvider(alg, 0);
  return out;
}

#else

inline void Install() {}

inline std::string Sha1(const std::filesystem::path&) { return "unknown"; }

#endif

inline void LogGameFiles(const std::filesystem::path& game_data_root) {
  REXLOG_INFO("SESSION: default.xex SHA-1 {}, default.xexp SHA-1 {}",
              Sha1(game_data_root / "default.xex"), Sha1(game_data_root / "default.xexp"));
}

}  // namespace crash_log
