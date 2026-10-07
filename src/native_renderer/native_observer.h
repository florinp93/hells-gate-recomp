// M2 observer: rebuilds the GPU state of each D3D draw and compares it with
// the Xenos register file. --native_observe_frames=N or F9; "NATIVE-OBS" logs.
// Rules: docs/NATIVE_D3D_MAP.md, "Effective GPU state at a draw".

#pragma once

#include <cstdint>

struct PPCContext;

namespace native {

enum class DrawKind : uint8_t {
  kIndexed,  // DrawIndexedVertices(prim, baseVertex, startIndex, indexCount)
  kAuto,     // DrawVertices(prim, startVertex, vertexCount)
  kInline,   // BeginVertices(prim, vertexCount, stride) .. EndVertices()
  kInlineUP, // DrawVerticesUP(prim, vertexCount, pData, stride)
};

// Around each draw call; the ticket is 0 when not observing.
uint32_t ObserveDrawBegin(DrawKind kind, const PPCContext& ctx, const uint8_t* base);
void ObserveDrawEnd(uint32_t ticket, const PPCContext& ctx, const uint8_t* base);

// The inline draw packet is written by EndVertices, not BeginVertices.
void ObserveBeginVerticesReturned(const PPCContext& ctx);
void ObserveEndVertices(const uint8_t* base);

// 0x8291D778: float constants loaded from an engine buffer. Call before it.
void ObserveConstantBufferLoad(const PPCContext& ctx, const uint8_t* base);

void ObserveFrameBoundary();

}  // namespace native
