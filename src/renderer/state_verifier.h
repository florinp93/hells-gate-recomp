// GPU state verifier: rebuilds the GPU state of each D3D draw from the D3D
// device and compares it with the Xenos register file the command processor
// sees. --verify_gpu_state_frames=N or F9; logs as NATIVE-OBS.

#pragma once

#include "renderer.h"

#include <cstdint>

struct PPCContext;

namespace renderer {

// Around each draw call; the ticket is 0 when not verifying.
uint32_t ObserveDrawBegin(DrawKind kind, const PPCContext& ctx, const uint8_t* base);
void ObserveDrawEnd(uint32_t ticket, const PPCContext& ctx, const uint8_t* base);

// The inline draw packet is written by EndVertices, not BeginVertices.
void ObserveBeginVerticesReturned(const PPCContext& ctx);
void ObserveEndVertices(const uint8_t* base);

void ObserveFrameBoundary();

}  // namespace renderer
