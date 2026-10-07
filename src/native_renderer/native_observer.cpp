// Render thread: each D3D draw queues its expected register state, keyed by
// the command-buffer range the call wrote. GPU thread: each Xenos draw is
// matched by its packet address and compared on the registers its shaders
// read. D3D-internal draws (clears, resolves) have no entry.

#include "native_observer.h"

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

// (first register, count, device offset)
struct ShadowRange {
  uint32_t reg;
  uint32_t count;
  uint32_t offset;
};
constexpr ShadowRange kShadowRanges[] = {
    {0x2000, 16, 10368},   // RB_SURFACE_INFO, RB_COLOR_INFO, RB_DEPTH_INFO, ...
                           // (+10432.. is bookkeeping, e.g. 0x02D00500 = 720/1280)
    {0x2100, 21, 10444},   // VGT_*, RB_COLOR_MASK, RB_STENCILREFMASK, ...
    {0x2180, 5, 10528},    // SQ_PROGRAM_CNTL, ...
    {0x2200, 12, 10548},   // RB_DEPTHCONTROL, RB_BLENDCONTROL*, RB_COLORCONTROL, ...
    {0x2280, 21, 10596},   // PA_SU_POINT_SIZE, ...
    {0x2300, 38, 10680},   // PA_SC_LINE_CNTL, RB_COPY_*, ...
    {0x2380, 8, 10832},    // PA_SU_POLY_OFFSET_*
    {0x4800, 192, 1152},   // fetch constants (32 x 6 dwords)
    {0x4000, 1024, 1920},  // VS float constants
    {0x4400, 1024, 6016},  // PS float constants
    {0x4900, 40, 10112},   // bool + loop constants
};
constexpr uint32_t kShadowDwords = [] {
  uint32_t total = 0;
  for (const auto& range : kShadowRanges) total += range.count;
  return total;
}();

// Device offsets of current objects.
constexpr uint32_t kDevRingWritePtr = 48;
constexpr uint32_t kDevVertexDecl = 11992;
constexpr uint32_t kDevIndexBuffer = 12612;
constexpr uint32_t kDevRenderTargets = 12616;  // RT0..3
constexpr uint32_t kDevDepthStencil = 12632;
constexpr uint32_t kDevStreams = 12636;  // 4 * stream
constexpr uint32_t kDevPixelShader = 12868;
constexpr uint32_t kDevVertexShader = 12872;

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
  std::array<uint32_t, kShadowDwords> shadow{};
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
};

struct RegStats {
  uint32_t count = 0;
  uint32_t after_internal = 0;  // mismatches right after a D3D-internal draw
  uint32_t first_seq = 0;
  uint32_t first_shadow = 0;
  uint32_t first_cp = 0;
};

enum class State { kIdle, kCapturing, kDraining };

State g_state = State::kIdle;  // render thread only
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
uint32_t g_buffer_constant_dwords = 0;

// Per vec4 (0-511): buffer address loaded by 0x8291D778, 0 = shadow.
std::array<uint32_t, 512> g_constant_source{};
bool g_internal_since_match = false;  // GPU thread

inline const uint8_t* GuestPtr(const uint8_t* base, uint32_t addr) {
  return base + addr + (addr >= 0xE0000000u ? 0x1000u : 0u);
}

inline uint32_t Load32(const uint8_t* base, uint32_t addr) {
  return __builtin_bswap32(*reinterpret_cast<const uint32_t*>(GuestPtr(base, addr)));
}

inline uint32_t Load16(const uint8_t* base, uint32_t addr) {
  return __builtin_bswap16(*reinterpret_cast<const uint16_t*>(GuestPtr(base, addr)));
}

inline bool Plausible(uint32_t addr) { return addr >= 0x10000u && addr < 0xFFFF0000u; }

constexpr int ShadowIndex(uint32_t reg) {
  int index = 0;
  for (const auto& range : kShadowRanges) {
    if (reg >= range.reg && reg < range.reg + range.count) return index + int(reg - range.reg);
    index += int(range.count);
  }
  return -1;
}

// Literal constants loaded by the program flush (0x827EA218): records
// {u16 vec4, u16 dwords, u32 offset} at blk + [blk+20] + 20, data at base + offset.
uint32_t ApplyShaderLiterals(const uint8_t* base, uint32_t blk, uint32_t ucode_base,
                             std::array<uint32_t, kShadowDwords>& expected) {
  if (!Plausible(blk) || !Plausible(ucode_base)) return 0;
  uint32_t table_offset = Load32(base, blk + 20);
  if (!table_offset) return 0;
  uint32_t table = blk + table_offset;
  uint32_t rec = table + 20;
  uint32_t end = rec + Load32(base, table + 16);
  uint32_t applied = 0;
  for (uint32_t n = 0; rec < end && n < 256; ++n) {
    uint32_t vec4_index = Load16(base, rec);
    uint32_t dword_count = Load16(base, rec + 2);
    if (!dword_count || dword_count > 1024) break;
    uint32_t data = ucode_base + Load32(base, rec + 4);
    rec += 8;
    if (!Plausible(data)) break;
    for (uint32_t d = 0; d < dword_count; ++d) {
      int index = ShadowIndex(0x4000 + vec4_index * 4 + d);
      if (index < 0) continue;
      expected[index] = Load32(base, data + d * 4);
      ++applied;
    }
  }
  return applied;
}

inline uint32_t GpuAddress(uint32_t addr) {
  return (addr & 0x1FFFFFFFu) + ((((addr >> 20) + 0x200u)) & 0x1000u);
}

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
      "draws_without_usage={} mismatching_registers={}",
      g_compared_draws, g_clean_draws, g_missed_entries, g_unmatched_cp_draws, g_wait_timeouts,
      g_inline_ok, g_inline_bad, g_literal_dwords, g_buffer_constant_dwords,
      g_draws_without_usage, g_reg_stats.size());
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
  g_buffer_constant_dwords = 0;
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
  uint32_t index = 0;
  for (const auto& range : kShadowRanges) {
    for (uint32_t i = 0; i < range.count; ++i) {
      e->shadow[index++] = Load32(base, e->device + range.offset + i * 4);
    }
  }
  // Shadow, then buffer-loaded constants, then shader literals (loaded last).
  for (uint32_t vec4 = 0; vec4 < 512; ++vec4) {
    uint32_t source = g_constant_source[vec4];
    if (!source) continue;
    for (uint32_t c = 0; c < 4; ++c) {
      int index = ShadowIndex(0x4000 + vec4 * 4 + c);
      if (index < 0) continue;
      e->shadow[index] = Load32(base, source + c * 4);
      ++e->buffer_constant_dwords;
    }
  }
  uint32_t vs = Load32(base, e->device + kDevVertexShader);
  uint32_t ps = Load32(base, e->device + kDevPixelShader);
  if (Plausible(vs)) {
    e->literal_dwords += ApplyShaderLiterals(base, vs + 872, Load32(base, vs + 0x20), e->shadow);
  }
  if (Plausible(ps)) {
    e->literal_dwords += ApplyShaderLiterals(base, ps + 40, Load32(base, ps + 0x18), e->shadow);
  }
  if (e->seq <= kMaxDecodedDrawsLogged) LogDecodedDraw(*e, base);
  e->ready.store(true, std::memory_order_release);
}

}  // namespace

// Dirty groups (dev+0 VS / dev+8 PS, bit 63-g) are uploaded from the shadow.
void TrackShadowConstantFlush(uint32_t device, const uint8_t* base) {
  for (uint32_t type = 0; type < 2; ++type) {
    uint32_t mask_addr = device + type * 8;
    uint64_t dirty = (uint64_t(Load32(base, mask_addr)) << 32) | Load32(base, mask_addr + 4);
    if (!dirty) continue;
    for (uint32_t group = 0; group < 64; ++group) {
      if (!((dirty >> (63 - group)) & 1)) continue;
      for (uint32_t i = 0; i < 4; ++i) g_constant_source[type * 256 + group * 4 + i] = 0;
    }
  }
}

uint32_t ObserveDrawBegin(DrawKind kind, const PPCContext& ctx, const uint8_t* base) {
  TrackShadowConstantFlush(ctx.r3.u32, base);
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

void ObserveConstantBufferLoad(const PPCContext& ctx, const uint8_t* base) {
  uint32_t start = (ctx.r4.u32 << 8) + ctx.r5.u32;
  uint32_t count = ctx.r6.u32;
  uint32_t buffer = ctx.r7.u32;
  if (!Plausible(buffer)) return;
  uint32_t source = Load32(base, buffer + 0x18) + ctx.r8.u32 * 16;
  for (uint32_t i = 0; i < count && start + i < 512; ++i) {
    g_constant_source[start + i] = source + i * 16;
  }
}

void ObserveEndVertices(const uint8_t* base) {
  Entry* e = g_pending;
  if (e && e->kind == DrawKind::kInline) Finalize(e, base);
}

void ObserveFrameBoundary() {
  switch (g_state) {
    case State::kIdle: {
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
      g_state = State::kCapturing;
    } break;
    case State::kCapturing: {
      REXCVAR_SET(native_observe_frames, int32_t(--g_frames_left));
      if (g_frames_left == 0) {
        g_observing.store(false, std::memory_order_relaxed);
        g_drain_frames = 0;
        g_state = State::kDraining;
      }
    } break;
    case State::kDraining: {
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
      g_state = State::kIdle;
    } break;
  }
}

}  // namespace native
