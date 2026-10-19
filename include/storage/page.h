#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>

namespace tinydb {
using PageId = std::uint32_t;
inline constexpr std::size_t kPageSize = 4096;
inline constexpr std::size_t kPageHeaderSize = 32;
inline constexpr PageId kInvalidPageId = std::numeric_limits<PageId>::max();
inline constexpr std::uint16_t kFormatVersion = 1;
enum class PageType : std::uint16_t {
  Metadata = 0, Heap = 1, Free = 2, IndexHeader = 3, IndexLeaf = 4, IndexInternal = 5
};
constexpr bool is_data_page_type(PageType type) {
  return type == PageType::Heap || type == PageType::IndexHeader ||
         type == PageType::IndexLeaf || type == PageType::IndexInternal;
}

struct Page {
  std::array<std::byte, kPageSize> bytes{};
  static Page create(PageId id, PageType type);
  [[nodiscard]] PageId id() const;
  [[nodiscard]] PageType type() const;
  void seal();
  void validate(PageId expected_id) const;
};
}  // namespace tinydb
