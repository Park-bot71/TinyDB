#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <type_traits>

#include "common/error.h"

namespace tinydb::encoding {
template <typename T>
T load(std::span<const std::byte> bytes, std::size_t offset) {
  static_assert(std::is_integral_v<T> && std::is_unsigned_v<T> && sizeof(T) <= sizeof(std::uint64_t));
  if (offset > bytes.size() || sizeof(T) > bytes.size() - offset)
    throw Error(ErrorCode::Corruption, "binary field outside buffer");
  std::uint64_t result = 0;
  for (std::size_t i = 0; i < sizeof(T); ++i)
    result |= static_cast<std::uint64_t>(std::to_integer<std::uint8_t>(bytes[offset + i])) << (i * 8);
  return static_cast<T>(result);
}

template <typename T>
void store(std::span<std::byte> bytes, std::size_t offset, T value) {
  static_assert(std::is_integral_v<T> && std::is_unsigned_v<T> && sizeof(T) <= sizeof(std::uint64_t));
  if (offset > bytes.size() || sizeof(T) > bytes.size() - offset)
    throw Error(ErrorCode::InvalidArgument, "binary field outside buffer");
  for (std::size_t i = 0; i < sizeof(T); ++i)
    bytes[offset + i] = static_cast<std::byte>((static_cast<std::uint64_t>(value) >> (i * 8)) & 0xffU);
}
}  // namespace tinydb::encoding
