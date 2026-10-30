#include "thread_cpu.h"

#include <fmt/format.h>

#include <rex/cvar.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <map>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <tlhelp32.h>
#endif

REXCVAR_DEFINE_BOOL(profile_threads, false, "Diagnostics",
                    "Sample the busiest threads every millisecond and log their hottest code "
                    "addresses with each PERF summary");

namespace renderer {

#if defined(_WIN32)

namespace {

using GetThreadDescriptionFn = HRESULT(WINAPI*)(HANDLE, PWSTR*);

std::string ThreadName(HANDLE thread, DWORD id) {
  static auto get_description = reinterpret_cast<GetThreadDescriptionFn>(
      GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "GetThreadDescription"));
  std::string name;
  PWSTR wide = nullptr;
  if (get_description && SUCCEEDED(get_description(thread, &wide)) && wide) {
    int size = WideCharToMultiByte(CP_UTF8, 0, wide, -1, nullptr, 0, nullptr, nullptr);
    if (size > 1) {
      name.resize(size - 1);
      WideCharToMultiByte(CP_UTF8, 0, wide, -1, name.data(), size, nullptr, nullptr);
    }
    LocalFree(wide);
  }
  if (name.empty()) name = fmt::format("tid {}", id);
  return name;
}

struct Sample {
  uint64_t cpu_100ns = 0;
  std::string name;
};

std::unordered_map<DWORD, Sample> g_previous;

struct HotKey {
  uint64_t leaf;
  uint64_t caller;
  bool operator<(const HotKey& o) const {
    return leaf != o.leaf ? leaf < o.leaf : caller < o.caller;
  }
};

struct ThreadProfile {
  std::string name;
  uint64_t samples = 0;
  std::map<HotKey, uint64_t> hits;
};

std::mutex g_profile_mutex;
std::unordered_map<DWORD, ThreadProfile> g_profile;
std::atomic<bool> g_profiling{false};

std::string Address(uint64_t address) {
  HMODULE module = nullptr;
  if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                              GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                          reinterpret_cast<LPCWSTR>(address), &module)) {
    return fmt::format("{:X}", address);
  }
  uint64_t rva = address - reinterpret_cast<uint64_t>(module);
  if (module == GetModuleHandleW(nullptr)) return fmt::format("{:X}", rva);
  wchar_t path[MAX_PATH];
  DWORD length = GetModuleFileNameW(module, path, MAX_PATH);
  std::wstring file(path, length);
  file = file.substr(file.find_last_of(L"\\/") + 1);
  return fmt::format("{}+{:X}", std::string(file.begin(), file.end()), rva);
}

void SampleThread(HANDLE thread, ThreadProfile& profile) {
  const uint64_t exe = reinterpret_cast<uint64_t>(GetModuleHandleW(nullptr));
  if (SuspendThread(thread) == DWORD(-1)) return;
  CONTEXT context{};
  context.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
  bool ok = GetThreadContext(thread, &context);
  HotKey key{context.Rip, 0};
  if (ok) {
    for (int frame = 0; frame < 24; ++frame) {
      if (frame > 0 && context.Rip >= exe && context.Rip < exe + 0x10000000) {
        key.caller = context.Rip;
        break;
      }
      DWORD64 image = 0;
      PRUNTIME_FUNCTION function = RtlLookupFunctionEntry(context.Rip, &image, nullptr);
      if (!function) break;
      void* handler_data = nullptr;
      DWORD64 establisher = 0;
      RtlVirtualUnwind(UNW_FLAG_NHANDLER, image, context.Rip, function, &context,
                       &handler_data, &establisher, nullptr);
      if (!context.Rip) break;
    }
  }
  ResumeThread(thread);
  if (!ok) return;
  ++profile.samples;
  ++profile.hits[key];
}

void ProfileLoop() {
  const DWORD pid = GetCurrentProcessId();
  const DWORD self = GetCurrentThreadId();
  std::unordered_map<DWORD, std::pair<HANDLE, uint64_t>> threads;
  uint32_t tick = 0;
  while (g_profiling.load()) {
    if (tick++ % 1000 == 0) {
      HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
      THREADENTRY32 entry{};
      entry.dwSize = sizeof(entry);
      for (BOOL ok = Thread32First(snapshot, &entry); ok; ok = Thread32Next(snapshot, &entry)) {
        if (entry.th32OwnerProcessID != pid || entry.th32ThreadID == self ||
            threads.count(entry.th32ThreadID)) {
          continue;
        }
        HANDLE thread = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT |
                                       THREAD_QUERY_LIMITED_INFORMATION,
                                   FALSE, entry.th32ThreadID);
        if (thread) threads[entry.th32ThreadID] = {thread, 0};
      }
      CloseHandle(snapshot);
    }
    for (auto it = threads.begin(); it != threads.end();) {
      ULONG64 cycles = 0;
      DWORD code = 0;
      if (!GetExitCodeThread(it->second.first, &code) || code != STILL_ACTIVE) {
        CloseHandle(it->second.first);
        it = threads.erase(it);
        continue;
      }
      QueryThreadCycleTime(it->second.first, &cycles);
      if (cycles - it->second.second > 100000) {
        std::lock_guard lock(g_profile_mutex);
        ThreadProfile& profile = g_profile[it->first];
        if (profile.name.empty()) profile.name = ThreadName(it->second.first, it->first);
        SampleThread(it->second.first, profile);
      }
      QueryThreadCycleTime(it->second.first, &it->second.second);
      ++it;
    }
    Sleep(1);
  }
  for (auto& [id, thread] : threads) CloseHandle(thread.first);
}

}  // namespace

std::string HotSpots() {
  if (!REXCVAR_GET(profile_threads)) return {};
  if (!g_profiling.exchange(true)) {
    std::thread(ProfileLoop).detach();
    return {};
  }
  std::unordered_map<DWORD, ThreadProfile> profile;
  {
    std::lock_guard lock(g_profile_mutex);
    profile.swap(g_profile);
  }
  std::vector<ThreadProfile*> order;
  for (auto& [id, p] : profile) order.push_back(&p);
  std::sort(order.begin(), order.end(),
            [](auto* a, auto* b) { return a->samples > b->samples; });
  std::string out;
  if (FILE* raw = std::fopen("logs/profile_samples.txt", "a")) {
    for (ThreadProfile* p : order) {
      for (auto& [key, count] : p->hits) {
        std::fprintf(raw, "%s\t%s\t%s\t%llu\n", p->name.c_str(), Address(key.leaf).c_str(),
                     key.caller ? Address(key.caller).c_str() : "-",
                     static_cast<unsigned long long>(count));
      }
    }
    std::fclose(raw);
  }
  for (size_t i = 0; i < std::min<size_t>(order.size(), 6); ++i) {
    ThreadProfile& p = *order[i];
    std::vector<std::pair<uint64_t, HotKey>> hits;
    for (auto& [key, count] : p.hits) hits.emplace_back(count, key);
    std::sort(hits.begin(), hits.end(),
              [](const auto& a, const auto& b) { return a.first > b.first; });
    out += fmt::format("\n  {} ({} samples):", p.name, p.samples);
    for (size_t j = 0; j < std::min<size_t>(hits.size(), 12); ++j) {
      out += fmt::format(" {}<{} {:.1f}%,", Address(hits[j].second.leaf),
                         hits[j].second.caller ? Address(hits[j].second.caller) : "-",
                         100.0 * hits[j].first / p.samples);
    }
    if (out.back() == ',') out.pop_back();
  }
  return out;
}

std::string BusiestThreads(double window_seconds) {
  HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
  if (snapshot == INVALID_HANDLE_VALUE) return {};
  const DWORD pid = GetCurrentProcessId();
  std::unordered_map<DWORD, Sample> current;
  std::vector<std::pair<double, std::string>> busy;
  THREADENTRY32 entry{};
  entry.dwSize = sizeof(entry);
  for (BOOL ok = Thread32First(snapshot, &entry); ok; ok = Thread32Next(snapshot, &entry)) {
    if (entry.th32OwnerProcessID != pid) continue;
    HANDLE thread = OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, entry.th32ThreadID);
    if (!thread) continue;
    FILETIME created, exited, kernel, user;
    if (GetThreadTimes(thread, &created, &exited, &kernel, &user)) {
      Sample& s = current[entry.th32ThreadID];
      s.cpu_100ns = (uint64_t(kernel.dwHighDateTime) << 32 | kernel.dwLowDateTime) +
                    (uint64_t(user.dwHighDateTime) << 32 | user.dwLowDateTime);
      auto previous = g_previous.find(entry.th32ThreadID);
      if (previous != g_previous.end()) {
        s.name = previous->second.name;
        double used = double(s.cpu_100ns - previous->second.cpu_100ns) * 1e-7;
        busy.emplace_back(used / window_seconds * 100.0, s.name);
      } else {
        s.name = ThreadName(thread, entry.th32ThreadID);
      }
    }
    CloseHandle(thread);
  }
  CloseHandle(snapshot);
  g_previous = std::move(current);
  if (busy.empty()) return {};
  double total = 0.0;
  for (const auto& b : busy) total += b.first;
  std::sort(busy.begin(), busy.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
  std::string out = fmt::format("CPU {:.0f}% of one core:", total);
  for (size_t i = 0; i < std::min<size_t>(busy.size(), 8) && busy[i].first >= 1.0; ++i) {
    out += fmt::format(" {} {:.0f}%,", busy[i].second, busy[i].first);
  }
  if (out.back() == ',') out.pop_back();
  return out;
}

#else

std::string BusiestThreads(double) { return {}; }
std::string HotSpots() { return {}; }

#endif

}  // namespace renderer
