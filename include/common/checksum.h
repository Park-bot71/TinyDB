#pragma once
#include <cstddef>
#include <cstdint>
#include <span>

namespace tinydb {
inline std::uint32_t crc32(std::span<const std::byte> bytes) {
  std::uint32_t crc = 0xffffffffU;
  for (const auto byte : bytes) {
    crc ^= std::to_integer<std::uint32_t>(byte);
    for (int bit = 0; bit < 8; ++bit)
      crc = (crc >> 1) ^ (0xedb88320U & (0U - (crc & 1U)));
  }
  return ~crc;
}
}  // namespace tinydb
