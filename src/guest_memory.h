#pragma once

// Big-endian access to guest memory. `base` is the start of the guest's 4 GB
// virtual address space; addresses from 0xE0000000 map 4 KB higher.

#include <cstdint>
#include <cstring>

namespace guest {

// Guest physical memory, mirrored at 0xA0000000 in the virtual address space.
inline constexpr uint32_t kPhysicalMirror = 0xA0000000u;
inline constexpr uint32_t kPhysicalMemorySize = 512u * 1024 * 1024;

inline const uint8_t* Ptr(const uint8_t* base, uint32_t address) {
  return base + address + (address >= 0xE0000000u ? 0x1000u : 0u);
}
inline uint8_t* Ptr(uint8_t* base, uint32_t address) {
  return base + address + (address >= 0xE0000000u ? 0x1000u : 0u);
}

inline const uint8_t* PhysicalPtr(const uint8_t* base, uint32_t physical) {
  return base + kPhysicalMirror + (physical & 0x1FFFFFFFu);
}

// [physical, physical + size) is non-empty and inside physical memory.
inline bool IsPhysicalRange(uint32_t physical, uint32_t size) {
  return size && physical < kPhysicalMemorySize && size <= kPhysicalMemorySize - physical;
}

inline uint16_t Load16(const uint8_t* base, uint32_t address) {
  uint16_t value;
  std::memcpy(&value, Ptr(base, address), sizeof(value));
  return __builtin_bswap16(value);
}
inline uint32_t Load32(const uint8_t* base, uint32_t address) {
  uint32_t value;
  std::memcpy(&value, Ptr(base, address), sizeof(value));
  return __builtin_bswap32(value);
}
inline uint64_t Load64(const uint8_t* base, uint32_t address) {
  uint64_t value;
  std::memcpy(&value, Ptr(base, address), sizeof(value));
  return __builtin_bswap64(value);
}
inline float LoadFloat(const uint8_t* base, uint32_t address) {
  uint32_t bits = Load32(base, address);
  float value;
  std::memcpy(&value, &bits, sizeof(value));
  return value;
}

inline void Store16(uint8_t* base, uint32_t address, uint16_t value) {
  value = __builtin_bswap16(value);
  std::memcpy(Ptr(base, address), &value, sizeof(value));
}
inline void Store32(uint8_t* base, uint32_t address, uint32_t value) {
  value = __builtin_bswap32(value);
  std::memcpy(Ptr(base, address), &value, sizeof(value));
}
inline void StoreFloat(uint8_t* base, uint32_t address, float value) {
  uint32_t bits;
  std::memcpy(&bits, &value, sizeof(bits));
  Store32(base, address, bits);
}

// Rejects null and obviously invalid pointers read from guest structures.
inline bool IsPlausibleAddress(uint32_t address) {
  return address >= 0x10000u && address < 0xFFFF0000u;
}

// Guest virtual -> GPU physical address, as the game's D3D layer computes it.
inline uint32_t GpuAddress(uint32_t address) {
  return (address & 0x1FFFFFFFu) + (((address >> 20) + 0x200u) & 0x1000u);
}

}  // namespace guest
