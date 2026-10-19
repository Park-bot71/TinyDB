#pragma once

#include <cstdint>
#include <vector>

#include "storage/slotted_page.h"

namespace tinydb {
using IndexKey = std::int64_t;
inline constexpr PageId kIndexHeaderPage = 1;
inline constexpr std::uint16_t kIndexMaxKeys = 184;  // (4096 - 40) / 22

struct IndexHeader {
  PageId root = kInvalidPageId;
  std::uint16_t max_keys = kIndexMaxKeys;
  std::uint64_t size = 0;
};

struct IndexNode {
  PageId id = kInvalidPageId;
  PageType type = PageType::IndexLeaf;
  PageId parent = kInvalidPageId;
  PageId next = kInvalidPageId;
  std::vector<IndexKey> keys;
  std::vector<RecordId> values;   // Leaves only.
  std::vector<PageId> children;   // Internal nodes: keys.size() + 1.
  [[nodiscard]] bool leaf() const noexcept { return type == PageType::IndexLeaf; }
};

Page encode_index_header(const IndexHeader& header);
IndexHeader decode_index_header(const Page& page);
Page encode_index_node(const IndexNode& node, std::uint16_t max_keys);
IndexNode decode_index_node(const Page& page, std::uint16_t max_keys);
}  // namespace tinydb
