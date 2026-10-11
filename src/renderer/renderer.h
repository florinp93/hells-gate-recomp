// renderer=dante: draws the game's D3D calls with DiligentCore (Vulkan) on the
// game window. Driven by the D3D hooks (d3d_hooks.cpp) on the guest render
// thread, after each original D3D call; the Xenos command processor keeps
// running for the game's sync only.

#pragma once

#include "button_glyphs.h"

#include <cstdint>
#include <functional>

namespace rex::ui {
class Window;
}

namespace renderer {

// The game's draw calls.
enum class DrawKind : uint8_t {
  kIndexed,   // DrawIndexedVertices(prim, baseVertex, startIndex, indexCount)
  kAuto,      // DrawVertices(prim, startVertex, vertexCount)
  kInline,    // BeginVertices(prim, vertexCount, stride) .. EndVertices()
  kInlineUP,  // DrawVerticesUP(prim, vertexCount, pData, stride)
};

// D3D call arguments (r3..r10, f1), captured before the original runs.
struct CallArgs {
  uint32_t r[8];
  double f1;
  uint32_t write_ptr_before = 0;         // command buffer write pointer
  uint32_t segment_switches_before = 0;  // SegmentSwitchCount()
};

// Command buffer write pointer of a D3D device (dev+48), 0 if implausible.
uint32_t CommandBufferWritePtr(uint32_t device, const uint8_t* base);

// UI thread, after GPU setup. Detaches the SDK presenter from the window.
bool Start(rex::ui::Window* window);
void Stop();

// Guest render thread.
void AfterClear(const CallArgs& args, const uint8_t* base);
void AfterResolve(const CallArgs& args, const uint8_t* base);
void BeforeDraw(uint32_t device, const uint8_t* base);
// After a top-level draw call has written its packets. An inline draw
// (BeginVertices) is drawn at EndVertices instead.
void AfterDraw(const CallArgs& args, DrawKind kind, const uint8_t* base);
// After every BeginVertices (also inside DrawVerticesUP), with the inline
// vertex pointer it returned.
void AfterBeginVertices(const CallArgs& args, uint32_t data);
void AfterEndVertices(const uint8_t* base);
// Program flush (sub_827EA4A0): the vertex shader microcode the GPU runs is
// the last vertex IM_LOAD / IM_LOAD_IMMEDIATE the flush writes. D3D patches
// the VS fetches per declaration, in place or (variant busy) into a copy in
// the command buffer.
void BeforeProgramFlush(uint32_t device, const uint8_t* base);
void AfterProgramFlush(uint32_t device, const uint8_t* base);
// Command buffer space reserved (sub_827D3F10 / sub_827D4148): the returned
// write pointer.
void AfterCommandBufferReserve(uint32_t write_ptr);

// Each reservation by sub_827D3F10: writing stopped after old_ptr and continues
// at new_ptr, in a new command buffer segment when they differ.
void RecordSegmentSwitch(uint32_t old_ptr, uint32_t new_ptr);
uint32_t SegmentSwitchCount();

// For the state verifier: ucode hashes of the shaders the next draw uses, and
// the index buffer of the last draw (guest physical, 0 = not indexed).
void BoundShaderHashes(uint32_t device, const uint8_t* base, uint64_t& vs, uint64_t& ps);
void LastIndexBuffer(uint32_t& base, uint32_t& count, uint32_t& format);

// Button glyph style, asked once per frame.
void SetButtonGlyphStyleSource(std::function<GlyphStyle()> source);
// Keyboard / mouse bindings, asked once per frame while the keyboard style shows.
void SetButtonGlyphBindingsSource(std::function<KeyBindings()> source);

// log_next_frame: write the next frame's events as NATIVE-RT lines.
void AfterSwap(const CallArgs& args, const uint8_t* base, bool log_next_frame);

}  // namespace renderer
