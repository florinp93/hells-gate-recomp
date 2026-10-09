#pragma once

namespace native {

// Called at every guest Swap: when rdoc_capture_frame is set and the process
// runs under RenderDoc, captures the GPU work between this swap and the next
// guest swaps, independent of host presents.
void RenderDocOnGuestSwap();

}  // namespace native
