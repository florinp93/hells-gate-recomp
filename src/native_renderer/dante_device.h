// renderer=dante: the project-owned renderer that replaces Xenos rendering.
// Owns the game window's swapchain; driven from the D3D hooks on the guest
// render thread, after each original D3D call.

#pragma once

#include <cstdint>

namespace rex::ui {
class Window;
}

namespace native {

// D3D call arguments (r3..r10, f1), captured before the original runs.
struct CallArgs {
  uint32_t r[8];
  double f1;
  uint32_t ring_before = 0;  // command buffer write pointer before the call
  uint32_t kicks_before = 0;  // DanteRingKickCount() before the call
};

// Command buffer write pointer of a device (dev+48), 0 if implausible.
uint32_t DanteRingWritePtr(uint32_t device, const uint8_t* base);

// UI thread, after GPU setup. Detaches the SDK presenter from the window.
bool StartDanteDevice(rex::ui::Window* window);
void StopDanteDevice();

// Guest render thread, after the original call.
void DanteOnClear(const CallArgs& args, const uint8_t* base);
void DanteOnResolve(const CallArgs& args, const uint8_t* base);
void DanteOnDraw(uint32_t device, const uint8_t* base);
// After a top-level draw call has flushed its state. kind: 0 DrawIndexedVertices,
// 1 DrawVertices, 2 BeginVertices (drawn at EndVertices), 3 DrawVerticesUP.
void DanteOnDrawSubmitted(const CallArgs& args, int kind, const uint8_t* base);
// After every BeginVertices (also inside DrawVerticesUP): its arguments and the
// returned inline vertex pointer.
void DanteOnBeginVertices(const CallArgs& args, uint32_t data);
void DanteOnEndVertices(const uint8_t* base);
// Program flush (sub_827EA4A0): the vertex shader microcode the GPU runs is
// the last vertex IM_LOAD / IM_LOAD_IMMEDIATE the flush writes. D3D patches
// the VS fetches per declaration, in place or (variant busy) into a ring copy.
void DanteOnProgramFlushBegin(uint32_t device, const uint8_t* base);
void DanteOnProgramFlushEnd(uint32_t device, const uint8_t* base);
// Command buffer space reserved inside a flush (new segment): the returned
// write pointer.
void DanteOnCommandReserve(uint32_t write_ptr);

// Ring kick (0x827D3F10): the segment written up to old_ptr was submitted and
// writing continues at new_ptr. Render thread.
void DanteOnRingKick(uint32_t old_ptr, uint32_t new_ptr);
uint32_t DanteRingKickCount();
// Diagnostics (M2 observer): ucode hashes of the shaders the next draw uses.
void DanteShaderHashes(uint32_t device, const uint8_t* base, uint64_t& vs, uint64_t& ps);
// Diagnostics: index buffer of the last submitted draw (guest physical, 0 = not indexed).
void DanteLastIndexBuffer(uint32_t& base, uint32_t& count, uint32_t& format);
// log_next_frame: write the next frame's events as NATIVE-RT lines.
void DanteOnSwap(const CallArgs& args, const uint8_t* base, bool log_next_frame);

}  // namespace native
