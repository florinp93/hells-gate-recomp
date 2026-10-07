// Diagnostic hooks on the game's XDK D3D layer (docs/NATIVE_D3D_MAP.md). Each
// hook logs its arguments for N frames and forwards to the original
// __imp__sub_X; behaviour is unchanged. --d3d_trace_frames=N or F8 (3 frames).
// Output: "D3D-TRACE" (calls), "D3D-OBJ" (first sighting of each object).

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging/macros.h>
#include <rex/ppc/context.h>

#include "native_observer.h"

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_set>

#include <fmt/format.h>

REXCVAR_DEFINE_INT32(d3d_trace_frames, 0, "Diagnostics",
                     "Log hooked D3D API calls (args, caller, return) for the "
                     "next N frames, then a per-frame call-count summary")
    .range(0, 1000);

namespace {

struct TraceSlot {
  const char* label;
  uint32_t guest_address;
  uint32_t max_logged_per_frame;
  int skip_null_reg;  // 3..10, or 0 = none
  int dump_reg;       // 3..10, or 0 = none
  uint32_t dump_dwords;
  std::atomic<uint32_t> frame_calls{0};
  std::atomic<uint32_t> frame_logged{0};
};

constexpr uint32_t kMaxDumpsPerSlot = 64;

std::atomic<bool> g_tracing{false};
std::atomic<uint32_t> g_frame_index{0};

TraceSlot* g_slots[64];
std::atomic<uint32_t> g_slot_count{0};

// Key: (slot address << 32) | object address.
std::mutex g_dump_mutex;
std::unordered_set<uint64_t> g_dumped;

struct SlotRegistrar {
  explicit SlotRegistrar(TraceSlot* slot) {
    uint32_t index = g_slot_count.fetch_add(1);
    if (index < 64) g_slots[index] = slot;
  }
};

struct SavedArgs {
  uint32_t r[8];  // r3..r10
  double f1, f2;
  uint32_t lr;
};

// PPCContext field order is r3, r0, r1, r2, r4..., so no array indexing.
inline uint32_t ArgReg(const PPCContext& ctx, int reg) {
  switch (reg) {
    case 3: return ctx.r3.u32;
    case 4: return ctx.r4.u32;
    case 5: return ctx.r5.u32;
    case 6: return ctx.r6.u32;
    case 7: return ctx.r7.u32;
    case 8: return ctx.r8.u32;
    case 9: return ctx.r9.u32;
    case 10: return ctx.r10.u32;
    default: return 0;
  }
}

inline uint32_t LoadGuestU32(const uint8_t* base, uint32_t addr) {
  uint32_t phys_offset = (addr >= 0xE0000000u) ? 0x1000u : 0u;
  return __builtin_bswap32(*reinterpret_cast<const uint32_t*>(base + addr + phys_offset));
}

void DumpObject(const TraceSlot& slot, const uint8_t* base, uint32_t obj) {
  if (obj < 0x10000u || obj >= 0xFFFF0000u) return;
  if (uint64_t(obj) + slot.dump_dwords * 4 + 0x1000 > 0x100000000ull) return;
  {
    std::lock_guard lock(g_dump_mutex);
    uint64_t prefix = uint64_t(slot.guest_address) << 32;
    uint32_t per_slot = 0;
    for (uint64_t key : g_dumped) {
      if ((key & 0xFFFFFFFF00000000ull) == prefix) ++per_slot;
    }
    if (per_slot >= kMaxDumpsPerSlot) return;
    if (!g_dumped.insert(prefix | obj).second) return;
  }
  for (uint32_t row = 0; row < slot.dump_dwords; row += 8) {
    std::string line;
    for (uint32_t i = row; i < row + 8 && i < slot.dump_dwords; ++i) {
      line += fmt::format(" {:08X}", LoadGuestU32(base, obj + i * 4));
    }
    REXLOG_INFO("D3D-OBJ f{} {} obj={:08X} +{:03X}:{}",
                g_frame_index.load(std::memory_order_relaxed), slot.label, obj,
                row * 4, line);
  }
}

inline bool TraceEnter(TraceSlot& slot, const PPCContext& ctx, const uint8_t* base,
                       SavedArgs& saved) {
  if (!g_tracing.load(std::memory_order_relaxed)) return false;
  slot.frame_calls.fetch_add(1, std::memory_order_relaxed);
  if (slot.dump_reg) DumpObject(slot, base, ArgReg(ctx, slot.dump_reg));
  if (slot.skip_null_reg && ArgReg(ctx, slot.skip_null_reg) == 0) return false;
  if (slot.frame_logged.fetch_add(1, std::memory_order_relaxed) >=
      slot.max_logged_per_frame) {
    return false;
  }
  for (int i = 0; i < 8; ++i) saved.r[i] = ArgReg(ctx, 3 + i);
  saved.f1 = ctx.f1.f64;
  saved.f2 = ctx.f2.f64;
  saved.lr = static_cast<uint32_t>(ctx.lr);
  return true;
}

inline void TraceExit(const TraceSlot& slot, const PPCContext& ctx, const SavedArgs& a) {
  REXLOG_INFO(
      "D3D-TRACE f{} {} [{:08X}] from {:08X}: r3={:08X} r4={:08X} r5={:08X} "
      "r6={:08X} r7={:08X} r8={:08X} r9={:08X} r10={:08X} f1={:.4f} f2={:.4f} "
      "-> r3={:08X}",
      g_frame_index.load(std::memory_order_relaxed), slot.label, slot.guest_address,
      a.lr, a.r[0], a.r[1], a.r[2], a.r[3], a.r[4], a.r[5], a.r[6], a.r[7], a.f1, a.f2,
      ctx.r3.u32);
}

void TraceFrameBoundary() {
  bool was_tracing = g_tracing.load(std::memory_order_relaxed);
  if (was_tracing) {
    uint32_t frame = g_frame_index.load(std::memory_order_relaxed);
    uint32_t count = g_slot_count.load();
    if (count > 64) count = 64;
    for (uint32_t i = 0; i < count; ++i) {
      TraceSlot* slot = g_slots[i];
      uint32_t calls = slot->frame_calls.exchange(0);
      slot->frame_logged.store(0);
      if (calls) {
        REXLOG_INFO("D3D-TRACE f{} summary {} [{:08X}] calls={}", frame, slot->label,
                    slot->guest_address, calls);
      }
    }
    int32_t remaining = REXCVAR_GET(d3d_trace_frames) - 1;
    REXCVAR_SET(d3d_trace_frames, remaining < 0 ? 0 : remaining);
    g_frame_index.fetch_add(1);
  }
  bool now_tracing = REXCVAR_GET(d3d_trace_frames) > 0;
  if (now_tracing && !was_tracing) {
    std::lock_guard lock(g_dump_mutex);
    g_dumped.clear();
  }
  g_tracing.store(now_tracing, std::memory_order_relaxed);
}

}  // namespace

#define D3D_TRACE_HOOK_EX(addr, label, max_logged, skip_null_reg, dump_reg, dump_dwords) \
  REX_EXTERN(__imp__sub_##addr);                                                         \
  static TraceSlot g_slot_##addr{label,         0x##addr, max_logged,                    \
                                 skip_null_reg, dump_reg, dump_dwords};                  \
  static SlotRegistrar g_reg_##addr{&g_slot_##addr};                                     \
  REX_HOOK_RAW(sub_##addr) {                                                             \
    SavedArgs saved;                                                                     \
    bool log = TraceEnter(g_slot_##addr, ctx, base, saved);                              \
    __imp__sub_##addr(ctx, base);                                                        \
    if (log) TraceExit(g_slot_##addr, ctx, saved);                                       \
  }

#define D3D_TRACE_HOOK(addr, label) D3D_TRACE_HOOK_EX(addr, label, 6, 0, 0, 0)

// Draw hooks also feed the M2 observer (native_observer.cpp).
#define D3D_DRAW_HOOK_AFTER(addr, label, kind, after)                               \
  REX_EXTERN(__imp__sub_##addr);                                                    \
  static TraceSlot g_slot_##addr{label, 0x##addr, 6, 0, 0, 0};                      \
  static SlotRegistrar g_reg_##addr{&g_slot_##addr};                                \
  REX_HOOK_RAW(sub_##addr) {                                                        \
    SavedArgs saved;                                                                \
    bool log = TraceEnter(g_slot_##addr, ctx, base, saved);                         \
    uint32_t ticket = native::ObserveDrawBegin(native::DrawKind::kind, ctx, base);  \
    __imp__sub_##addr(ctx, base);                                                   \
    native::ObserveDrawEnd(ticket, ctx, base);                                      \
    after;                                                                          \
    if (log) TraceExit(g_slot_##addr, ctx, saved);                                  \
  }
#define D3D_DRAW_HOOK(addr, label, kind) D3D_DRAW_HOOK_AFTER(addr, label, kind, (void)0)

#define D3D_TRACE_HOOK_AFTER(addr, label, after)                                    \
  REX_EXTERN(__imp__sub_##addr);                                                    \
  static TraceSlot g_slot_##addr{label, 0x##addr, 6, 0, 0, 0};                      \
  static SlotRegistrar g_reg_##addr{&g_slot_##addr};                                \
  REX_HOOK_RAW(sub_##addr) {                                                        \
    SavedArgs saved;                                                                \
    bool log = TraceEnter(g_slot_##addr, ctx, base, saved);                         \
    __imp__sub_##addr(ctx, base);                                                   \
    after;                                                                          \
    if (log) TraceExit(g_slot_##addr, ctx, saved);                                  \
  }

#define D3D_TRACE_HOOK_BEFORE(addr, label, before)                                  \
  REX_EXTERN(__imp__sub_##addr);                                                    \
  static TraceSlot g_slot_##addr{label, 0x##addr, 6, 0, 0, 0};                      \
  static SlotRegistrar g_reg_##addr{&g_slot_##addr};                                \
  REX_HOOK_RAW(sub_##addr) {                                                        \
    SavedArgs saved;                                                                \
    bool log = TraceEnter(g_slot_##addr, ctx, base, saved);                         \
    before;                                                                         \
    __imp__sub_##addr(ctx, base);                                                   \
    if (log) TraceExit(g_slot_##addr, ctx, saved);                                  \
  }

// Draws (TU2 copies at 0x8291xxxx).
D3D_DRAW_HOOK(8291D328, "DrawIndexedVertices", kIndexed)
D3D_DRAW_HOOK(8291CF38, "DrawVertices", kAuto)
// BeginVertices..EndVertices is one draw; DrawVerticesUP inlines the End.
D3D_DRAW_HOOK_AFTER(8291CA58, "BeginVertices", kInline, native::ObserveBeginVerticesReturned(ctx))
D3D_TRACE_HOOK_AFTER(8291CA48, "EndVertices", native::ObserveEndVertices(base))
D3D_DRAW_HOOK(8291CEF0, "DrawVerticesUP", kInlineUP)

D3D_TRACE_HOOK(827E7B10, "SetVertexShaderConstantF")
D3D_TRACE_HOOK(827E7BE8, "SetPixelShaderConstantF")
D3D_TRACE_HOOK_BEFORE(8291D778, "LoadShaderConstantsFromBuffer",
                      native::ObserveConstantBufferLoad(ctx, base))

D3D_TRACE_HOOK(827EDC18, "Clear")
D3D_TRACE_HOOK_EX(827E5B18, "Resolve", 6, 0, 6, 16)  // r6 = dest texture
D3D_TRACE_HOOK(827DCF68, "EndTiling")
D3D_TRACE_HOOK(827DCBF8, "BeginTiling")
D3D_TRACE_HOOK(827DCA88, "SetPredication")
D3D_TRACE_HOOK_EX(827D2278, "SetRenderTarget", 12, 0, 5, 24)  // (index, surface)
D3D_TRACE_HOOK_EX(827D25C8, "SetDepthStencilSurface", 12, 0, 4, 24)

D3D_TRACE_HOOK_EX(827E8018, "SetVertexShader", 8, 4, 4, 32)
D3D_TRACE_HOOK_EX(827E7E58, "SetPixelShader", 8, 4, 4, 32)
D3D_TRACE_HOOK_EX(827E81E8, "SetVertexDeclaration", 6, 4, 4, 24)
D3D_TRACE_HOOK(827E82F0, "CreateVertexDeclaration")
D3D_TRACE_HOOK_EX(827D1B30, "SetIndices", 6, 4, 4, 12)
D3D_TRACE_HOOK_EX(827D1A10, "SetStreamSource", 12, 5, 5, 12)
D3D_TRACE_HOOK_EX(827D92A0, "SetTexture", 16, 5, 5, 16)
D3D_TRACE_HOOK(8291BC38, "GetResourceGpuAddress?")

D3D_TRACE_HOOK(827D0EC8, "SetSampler_0EC8")
D3D_TRACE_HOOK(827D1070, "SetSampler_1070")
D3D_TRACE_HOOK(827D1310, "SetSampler_1310")
D3D_TRACE_HOOK(827D1430, "SetSampler_LodBias")

D3D_TRACE_HOOK(827D7128, "Release?")
D3D_TRACE_HOOK_EX(827D71A8, "VB_Lock?", 6, 0, 3, 12)
D3D_TRACE_HOOK(827D3178, "BlockOnFence")
D3D_TRACE_HOOK(827D31D8, "unk_kernel_31D8")
D3D_TRACE_HOOK(827D6268, "VB_Unlock?")
D3D_TRACE_HOOK(827D6278, "unk_6278")
D3D_TRACE_HOOK(827D4010, "InsertFence")
D3D_TRACE_HOOK(827D5778, "unk_5778(dev+60)")
D3D_TRACE_HOOK_EX(827DAA98, "Query_Issue", 6, 0, 3, 16)

D3D_TRACE_HOOK(827E2FC0, "CreateDevice?")
D3D_TRACE_HOOK(827D4AB0, "unk_4AB0(pre-Swap)")

// Swap ends the frame for both the trace and the observer.
REX_EXTERN(__imp__sub_827D4EE0);
static TraceSlot g_slot_827D4EE0{"Swap", 0x827D4EE0, 6, 0, 4, 16};
static SlotRegistrar g_reg_827D4EE0{&g_slot_827D4EE0};
REX_HOOK_RAW(sub_827D4EE0) {
  SavedArgs saved;
  bool log = TraceEnter(g_slot_827D4EE0, ctx, base, saved);
  __imp__sub_827D4EE0(ctx, base);
  if (log) TraceExit(g_slot_827D4EE0, ctx, saved);
  TraceFrameBoundary();
  native::ObserveFrameBoundary();
}
