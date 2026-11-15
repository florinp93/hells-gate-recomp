#pragma once

#include <cstdint>
#include <string>

namespace renderer {

std::string BusiestThreads(double window_seconds);
std::string HotSpots();
bool ProcessMemory(uint64_t& private_bytes, uint64_t& working_set);

}  // namespace renderer
