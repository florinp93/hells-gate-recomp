// Render thread: each D3D draw queues its expected register state, keyed by
// the command-buffer range the call wrote. GPU thread: each Xenos draw is
// matched by its packet address and compared on the registers its shaders
// read. D3D-internal draws (clears, resolves) have no entry.

#include "native_observer.h"

#include "dante_device.h"
#include "gpu_state.h"

#include <rex/cvar.h>
#include <rex/logging/macros.h>
#include <rex/ppc/context.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <fmt/format.h>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

REXCVAR_DEFINE_INT32(native_observe_frames, 0, "Diagnostics",
                     "M2 shadow observer: decode the guest D3D register shadow at "
                     "each draw for the next N frames and cross-check it against "
                     "the Xenos register file")
    .range(0, 100);

namespace native {
namespace {

using namespace native::gpu;

// Written straight to the command buffer, not shadowed.
constexpr uint32_t kUnshadowedRegs[] = {
    0x2325,  // RB_SAMPLE_COUNT_ADDR (occlusion query, Query::Issue)
};
// Inline draws: stream 0 (vertex fetch 95) points at the ring data.
constexpr uint32_t kInlineStreamFetchReg = 0x48BE;

constexpr uint32_t kMaxDecodedDrawsLogged = 12;
constexpr uint32_t kMaxUnmatchedLogged = 8;
constexpr uint32_t kMaxMissedLogged = 8;
constexpr size_t kMatchWindow = 64;

struct Entry {
  uint32_t seq = 0;
  DrawKind kind = DrawKind::kIndexed;
  uint32_t prim = 0;
  uint32_t count = 0;
  uint32_t device = 0;
  uint32_t ring_before = 0;  // guest physical
  uint32_t ring_end = 0;     // guest physical, just past the last dword written
  uint32_t inline_data = 0;  // BeginVertices return value (inline draws)
  uint32_t literal_dwords = 0;
  uint32_t buffer_constant_dwords = 0;
  std::atomic<bool> ready{false};
  State shadow{};
  uint64_t vs_hash = 0, ps_hash = 0;  // renderer=dante's shaders
  uint32_t vs_object = 0, decl = 0;
  uint32_t index_base = 0, index_count = 0, index_format = 0;
};

// Mirrors RexGpuDrawUsage in the SDK command processor.
struct RexGpuDrawUsage {
  uint32_t struct_size;
  uint32_t vs_float_base;
  uint32_t ps_float_base;
  uint32_t float_dynamic;
  uint64_t vs_float[4];
  uint64_t ps_float[4];
  uint32_t vertex_fetch[3];
  uint32_t texture_fetch;
  uint32_t memexport;
  uint64_t vs_ucode_hash;
  uint64_t ps_ucode_hash;
  uint32_t index_base;
  uint32_t index_count;
  uint32_t index_format;
};

struct RegStats {
  uint32_t count = 0;
  uint32_t after_internal = 0;  // mismatches right after a D3D-internal draw
  uint32_t first_seq = 0;
  uint32_t first_shadow = 0;
  uint32_t first_cp = 0;
};

enum class CaptureState { kIdle, kCapturing, kDraining };

CaptureState g_state = CaptureState::kIdle;  // render thread only
uint32_t g_frames_left = 0;
uint32_t g_drain_frames = 0;
bool g_observer_installed = false;

std::atomic<bool> g_observing{false};  // render thread records draws
std::atomic<bool> g_cp_active{false};  // GPU thread matches draws
uint32_t g_next_seq = 0;
Entry* g_pending = nullptr;

std::mutex g_queue_mutex;
std::deque<std::unique_ptr<Entry>> g_queue;

std::mutex g_stats_mutex;
std::map<uint32_t, RegStats> g_reg_stats;
uint32_t g_compared_draws = 0;
uint32_t g_clean_draws = 0;
uint32_t g_missed_entries = 0;
uint32_t g_unmatched_cp_draws = 0;
uint32_t g_wait_timeouts = 0;
uint32_t g_inline_ok = 0;
uint32_t g_inline_bad = 0;
uint32_t g_literal_dwords = 0;
uint32_t g_draws_without_usage = 0;
uint32_t g_vs_match = 0, g_vs_mismatch = 0, g_ps_mismatch = 0, g_shader_mismatch_logged = 0;
uint32_t g_index_match = 0, g_index_mismatch = 0, g_index_mismatch_logged = 0;
uint32_t g_buffer_constant_dwords = 0;
uint32_t g_memexport_draws = 0;

bool g_internal_since_match = false;  // GPU thread

void LogDecodedDraw(const Entry& e, const uint8_t* base) {
  const uint32_t dev = e.device;
  auto shadow_reg = [&e](uint32_t reg) -> uint32_t {
    uint32_t index = 0;
    for (const auto& range : kShadowRanges) {
      if (reg >= range.reg && reg < range.reg + range.count) return e.shadow[index + reg - range.reg];
      index += range.count;
    }
    return 0;
  };

  std::string line = fmt::format("NATIVE-OBS draw #{} {} prim={} count={} surface_info={:08X}",
                                 e.seq,
                                 e.kind == DrawKind::kIndexed  ? "indexed"
                                 : e.kind == DrawKind::kAuto   ? "auto"
                                 : e.kind == DrawKind::kInline ? "inline"
                                                               : "inline-up",
                                 e.prim, e.count, shadow_reg(0x2000));
  for (uint32_t i = 0; i < 4; ++i) {
    uint32_t rt = Load32(base, dev + kDevRenderTargets + i * 4);
    if (rt) line += fmt::format(" RT{}={:08X}", i, rt);
  }
  line += fmt::format(" color_info={:08X}", shadow_reg(0x2001));
  if (uint32_t ds = Load32(base, dev + kDevDepthStencil)) {
    line += fmt::format(" DS={:08X} depth_info={:08X}", ds, shadow_reg(0x2002));
  }

  uint32_t vs = Load32(base, dev + kDevVertexShader);
  uint32_t ps = Load32(base, dev + kDevPixelShader);
  line += fmt::format(" VS={:08X} PS={:08X}", vs, ps);
  if (Plausible(ps)) {
    uint32_t block = ps + Load32(base, ps + 0x40);
    if (Plausible(block)) {
      line += fmt::format("(ucode={:08X}+{})", Load32(base, ps + 0x18) + Load32(base, block + 40),
                          Load32(base, block + 44));
    }
  }
  line += fmt::format(" decl={:08X} IB={:08X}", Load32(base, dev + kDevVertexDecl),
                      Load32(base, dev + kDevIndexBuffer));
  for (uint32_t s = 0; s < 4; ++s) {
    if (uint32_t vb = Load32(base, dev + kDevStreams + s * 4)) {
      line += fmt::format(" s{}={:08X}", s, vb);
    }
  }
  uint32_t fetch_index = 0;
  for (const auto& range : kShadowRanges) {
    if (range.reg == 0x4800) break;
    fetch_index += range.count;
  }
  for (uint32_t slot = 0; slot < 16; ++slot) {
    const uint32_t* f = &e.shadow[fetch_index + slot * 6];
    if ((f[0] & 3) != 2) continue;
    line += fmt::format(" t{}=fmt{}:{}x{}@{:08X}", slot, f[1] & 0x3F, (f[2] & 0x1FFF) + 1,
                        ((f[2] >> 13) & 0x1FFF) + 1, f[1] & 0xFFFFF000u);
  }
  REXLOG_INFO("{}", line);
}

bool InstallCpObserver();

void Report() {
  std::lock_guard lock(g_stats_mutex);
  REXLOG_INFO(
      "NATIVE-OBS report: compared={} clean={} missed_entries={} unmatched_cp_draws={} "
      "wait_timeouts={} inline_stream ok={} bad={} literal_dwords={} buffer_constant_dwords={} "
      "draws_without_usage={} memexport_draws={} mismatching_registers={}",
      g_compared_draws, g_clean_draws, g_missed_entries, g_unmatched_cp_draws, g_wait_timeouts,
      g_inline_ok, g_inline_bad, g_literal_dwords, g_buffer_constant_dwords,
      g_draws_without_usage, g_memexport_draws, g_reg_stats.size());
  REXLOG_INFO("NATIVE-OBS shaders: vs match={} mismatch={} ps mismatch={}", g_vs_match,
              g_vs_mismatch, g_ps_mismatch);
  REXLOG_INFO("NATIVE-OBS index buffers: match={} mismatch={}", g_index_match, g_index_mismatch);
  std::vector<std::pair<uint32_t, RegStats>> sorted(g_reg_stats.begin(), g_reg_stats.end());
  std::sort(sorted.begin(), sorted.end(),
            [](const auto& a, const auto& b) { return a.second.count > b.second.count; });
  uint32_t logged = 0;
  for (const auto& [reg, stats] : sorted) {
    if (++logged > 48) break;
    REXLOG_INFO(
        "NATIVE-OBS mismatch reg={:04X} draws={} after_internal={} first: draw #{} "
        "expected={:08X} cp={:08X}",
        reg, stats.count, stats.after_internal, stats.first_seq, stats.first_shadow,
        stats.first_cp);
  }
}

void ResetStats() {
  std::lock_guard lock(g_stats_mutex);
  g_reg_stats.clear();
  g_compared_draws = g_clean_draws = g_missed_entries = 0;
  g_unmatched_cp_draws = g_wait_timeouts = 0;
  g_inline_ok = g_inline_bad = g_literal_dwords = g_draws_without_usage = 0;
  g_vs_match = g_vs_mismatch = g_ps_mismatch = g_shader_mismatch_logged = 0;
  g_index_match = g_index_mismatch = g_index_mismatch_logged = 0;
  g_buffer_constant_dwords = g_memexport_draws = 0;
  g_internal_since_match = false;
}

inline bool BitSet(const uint64_t* bits, uint32_t i) { return (bits[i >> 6] >> (i & 63)) & 1; }

// Constants and fetch constants only matter where the shaders read them.
bool ShouldCompare(uint32_t reg, const RexGpuDrawUsage* usage) {
  if (reg >= 0x4000 && reg < 0x4800) {
    if (!usage) return false;
    uint32_t vec4 = (reg - 0x4000) / 4;
    if (vec4 >= usage->vs_float_base && vec4 < usage->vs_float_base + 256) {
      uint32_t i = vec4 - usage->vs_float_base;
      if ((usage->float_dynamic & 1) || BitSet(usage->vs_float, i)) return true;
    }
    if (vec4 >= usage->ps_float_base && vec4 < usage->ps_float_base + 256) {
      uint32_t i = vec4 - usage->ps_float_base;
      if ((usage->float_dynamic & 2) || BitSet(usage->ps_float, i)) return true;
    }
    return false;
  }
  if (reg >= 0x4800 && reg < 0x48C0) {
    if (!usage) return false;
    uint32_t dword = reg - 0x4800;
    if ((usage->texture_fetch >> (dword / 6)) & 1) return true;
    uint32_t vfetch = dword / 2;
    return (usage->vertex_fetch[vfetch >> 5] >> (vfetch & 31)) & 1;
  }
  return true;
}

size_t FindEntry(uint32_t packet_end) {
  for (size_t i = 0; i < g_queue.size() && i < kMatchWindow; ++i) {
    const Entry& e = *g_queue[i];
    if (!e.ready.load(std::memory_order_acquire)) break;
    bool in_range = e.ring_before < packet_end && packet_end <= e.ring_end &&
                    e.ring_end - e.ring_before < 0x100000u;
    if (packet_end == e.ring_end || in_range) return i;
  }
  return SIZE_MAX;
}

void OnCpDraw(void*, const uint32_t* regs, uint32_t reg_count, uint32_t draw_initiator,
              uint32_t packet_end, const RexGpuDrawUsage* usage) {
  if (!g_cp_active.load(std::memory_order_acquire)) return;
  if (usage && usage->struct_size < sizeof(RexGpuDrawUsage)) usage = nullptr;

  std::unique_lock lock(g_queue_mutex);
  size_t found = FindEntry(packet_end);
  // The newest draw may still be inside its D3D call; give it a moment.
  auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(20);
  while (found == SIZE_MAX && !g_queue.empty() &&
         !g_queue.back()->ready.load(std::memory_order_acquire)) {
    if (std::chrono::steady_clock::now() > deadline) {
      std::lock_guard stats_lock(g_stats_mutex);
      ++g_wait_timeouts;
      break;
    }
    lock.unlock();
    std::this_thread::yield();
    lock.lock();
    found = FindEntry(packet_end);
  }
  if (found == SIZE_MAX) {
    lock.unlock();
    std::lock_guard stats_lock(g_stats_mutex);
    g_internal_since_match = true;
    if (++g_unmatched_cp_draws <= kMaxUnmatchedLogged) {
      REXLOG_INFO(
          "NATIVE-OBS unmatched xenos draw: prim={} count={} packet_end={:08X} "
          "RB_MODECONTROL={:08X} RB_SURFACE_INFO={:08X}",
          draw_initiator & 0x3F, draw_initiator >> 16, packet_end, regs[0x2208], regs[0x2000]);
    }
    return;
  }
  std::unique_ptr<Entry> entry = std::move(g_queue[found]);
  std::lock_guard stats_lock(g_stats_mutex);
  for (size_t i = 0; i < found; ++i) {
    const Entry& missed = *g_queue[i];
    if (++g_missed_entries <= kMaxMissedLogged) {
      REXLOG_INFO(
          "NATIVE-OBS missed draw #{} kind={} prim={} count={} ring={:08X}..{:08X} "
          "(next xenos draw packet_end={:08X} matched #{})",
          missed.seq, int(missed.kind), missed.prim, missed.count, missed.ring_before,
          missed.ring_end, packet_end, entry->seq);
    }
  }
  g_queue.erase(g_queue.begin(), g_queue.begin() + found + 1);
  lock.unlock();

  bool after_internal = g_internal_since_match;
  g_internal_since_match = false;
  g_literal_dwords += entry->literal_dwords;
  g_buffer_constant_dwords += entry->buffer_constant_dwords;
  bool is_inline = entry->kind == DrawKind::kInline || entry->kind == DrawKind::kInlineUP;
  if (!usage) ++g_draws_without_usage;
  if (usage && usage->struct_size >= sizeof(RexGpuDrawUsage) && usage->index_base) {
    bool ok = usage->index_base == entry->index_base && usage->index_count == entry->index_count &&
              usage->index_format == entry->index_format;
    ++(ok ? g_index_match : g_index_mismatch);
    if (!ok && ++g_index_mismatch_logged <= 24) {
      REXLOG_INFO("NATIVE-OBS index mismatch draw #{} prim={} ours={:08X}x{} f{} xenos={:08X}x{} f{}",
                  entry->seq, entry->prim, entry->index_base, entry->index_count,
                  entry->index_format, usage->index_base, usage->index_count,
                  usage->index_format);
    }
  }
  if (usage && usage->struct_size >= sizeof(RexGpuDrawUsage) && entry->vs_hash) {
    bool vs_ok = usage->vs_ucode_hash == entry->vs_hash;
    bool ps_ok = !entry->ps_hash || usage->ps_ucode_hash == entry->ps_hash;
    ++(vs_ok ? g_vs_match : g_vs_mismatch);
    if (!ps_ok) ++g_ps_mismatch;
    if ((!vs_ok || !ps_ok) && ++g_shader_mismatch_logged <= 24) {
      REXLOG_INFO(
          "NATIVE-OBS shader mismatch draw #{} prim={} count={} vs={:08X} decl={:08X} "
          "ours vs={:016X} ps={:016X} xenos vs={:016X} ps={:016X}",
          entry->seq, entry->prim, entry->count, entry->vs_object, entry->decl, entry->vs_hash,
          entry->ps_hash, usage->vs_ucode_hash, usage->ps_ucode_hash);
    }
  }
  if (usage && usage->memexport && ++g_memexport_draws <= 4) {
    REXLOG_INFO("NATIVE-OBS memexport draw #{} prim={} count={} shaders={}", entry->seq,
                entry->prim, entry->count, usage->memexport);
  }
  if (is_inline && kInlineStreamFetchReg < reg_count) {
    bool ok = (regs[kInlineStreamFetchReg] & ~3u) == GpuAddress(entry->inline_data);
    ++(ok ? g_inline_ok : g_inline_bad);
  }
  ++g_compared_draws;
  bool clean = true;
  uint32_t index = 0;
  for (const auto& range : kShadowRanges) {
    for (uint32_t i = 0; i < range.count; ++i, ++index) {
      uint32_t reg = range.reg + i;
      if (reg >= reg_count) continue;
      if (is_inline && (reg == kInlineStreamFetchReg || reg == kInlineStreamFetchReg + 1)) continue;
      if (!ShouldCompare(reg, usage)) continue;
      if (std::find(std::begin(kUnshadowedRegs), std::end(kUnshadowedRegs), reg) !=
          std::end(kUnshadowedRegs)) {
        continue;
      }
      uint32_t shadow = entry->shadow[index];
      uint32_t cp = regs[reg];
      if (shadow == cp) continue;
      clean = false;
      RegStats& stats = g_reg_stats[reg];
      if (after_internal) ++stats.after_internal;
      if (stats.count++ == 0) {
        stats.first_seq = entry->seq;
        stats.first_shadow = shadow;
        stats.first_cp = cp;
      }
    }
  }
  if (clean) ++g_clean_draws;
}

using SetDrawObserverFn = void (*)(void (*)(void*, const uint32_t*, uint32_t, uint32_t, uint32_t,
                                            const RexGpuDrawUsage*),
                                   void*);

bool InstallCpObserver() {
  if (g_observer_installed) return true;
#ifdef _WIN32
  // renderer=xenos: plugin DLL; renderer=native: linked into the exe.
  SetDrawObserverFn set_observer = nullptr;
  for (HMODULE module : {GetModuleHandleA("rexgpu-xenos.dll"), GetModuleHandleA(nullptr)}) {
    if (!module) continue;
    set_observer = reinterpret_cast<SetDrawObserverFn>(
        GetProcAddress(module, "rex_gpu_set_draw_observer"));
    if (set_observer) break;
  }
  if (!set_observer) return false;
  set_observer(&OnCpDraw, nullptr);
  g_observer_installed = true;
#endif
  return g_observer_installed;
}

void Finalize(Entry* e, const uint8_t* base) {
  g_pending = nullptr;
  // dev+48 points at the last dword written.
  e->ring_end = GpuAddress(Load32(base, e->device + kDevRingWritePtr)) + 4;
  CaptureStats stats = Capture(e->device, base, e->shadow);
  e->literal_dwords = stats.literal_dwords;
  DanteShaderHashes(e->device, base, e->vs_hash, e->ps_hash);
  e->vs_object = Load32(base, e->device + kDevVertexShader);
  e->decl = Load32(base, e->device + kDevVertexDecl);
  DanteLastIndexBuffer(e->index_base, e->index_count, e->index_format);
  e->buffer_constant_dwords = stats.buffer_constant_dwords;
  if (e->seq <= kMaxDecodedDrawsLogged) LogDecodedDraw(*e, base);
  e->ready.store(true, std::memory_order_release);
}

}  // namespace

uint32_t ObserveDrawBegin(DrawKind kind, const PPCContext& ctx, const uint8_t* base) {
  if (!g_observing.load(std::memory_order_relaxed)) return 0;
  if (g_pending) return 0;
  auto entry = std::make_unique<Entry>();
  entry->seq = ++g_next_seq;
  entry->kind = kind;
  entry->prim = ctx.r4.u32;
  entry->count = kind == DrawKind::kIndexed ? ctx.r7.u32
                 : kind == DrawKind::kAuto  ? ctx.r6.u32
                                            : ctx.r5.u32;
  entry->device = ctx.r3.u32;
  entry->ring_before = GpuAddress(Load32(base, entry->device + kDevRingWritePtr));
  g_pending = entry.get();
  {
    std::lock_guard lock(g_queue_mutex);
    g_queue.push_back(std::move(entry));
  }
  return g_pending->seq;
}

void ObserveDrawEnd(uint32_t ticket, const PPCContext& ctx, const uint8_t* base) {
  Entry* e = g_pending;
  if (!ticket || !e || e->seq != ticket) return;
  if (e->kind == DrawKind::kInline) return;
  Finalize(e, base);
}

void ObserveBeginVerticesReturned(const PPCContext& ctx) {
  Entry* e = g_pending;
  if (e && (e->kind == DrawKind::kInline || e->kind == DrawKind::kInlineUP)) {
    e->inline_data = ctx.r3.u32;
  }
}

void ObserveEndVertices(const uint8_t* base) {
  Entry* e = g_pending;
  if (e && e->kind == DrawKind::kInline) Finalize(e, base);
}

void ObserveFrameBoundary() {
  switch (g_state) {
    case CaptureState::kIdle: {
      int32_t frames = REXCVAR_GET(native_observe_frames);
      if (frames <= 0) return;
      ResetStats();
      g_next_seq = 0;
      bool cp = InstallCpObserver();
      REXLOG_INFO("NATIVE-OBS start: {} frames, xenos cross-check {}", frames,
                  cp ? "on" : "unavailable (rexgpu-xenos.dll lacks rex_gpu_set_draw_observer)");
      g_frames_left = uint32_t(frames);
      g_cp_active.store(cp, std::memory_order_release);
      g_observing.store(true, std::memory_order_relaxed);
      g_state = CaptureState::kCapturing;
    } break;
    case CaptureState::kCapturing: {
      REXCVAR_SET(native_observe_frames, int32_t(--g_frames_left));
      if (g_frames_left == 0) {
        g_observing.store(false, std::memory_order_relaxed);
        g_drain_frames = 0;
        g_state = CaptureState::kDraining;
      }
    } break;
    case CaptureState::kDraining: {
      bool empty;
      {
        std::lock_guard lock(g_queue_mutex);
        empty = g_queue.empty();
      }
      if (!empty && ++g_drain_frames < 8) return;
      g_cp_active.store(false, std::memory_order_release);
      {
        std::lock_guard lock(g_queue_mutex);
        if (!g_queue.empty()) {
          std::lock_guard stats_lock(g_stats_mutex);
          g_missed_entries += uint32_t(g_queue.size());
        }
        g_queue.clear();
      }
      Report();
      REXCVAR_SET(native_observe_frames, 0);
      g_state = CaptureState::kIdle;
    } break;
  }
}

}  // namespace native
