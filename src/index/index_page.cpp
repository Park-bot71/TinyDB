#include "index/index_page.h"

#include <algorithm>
#include <bit>

#include "common/encoding.h"

namespace tinydb {
namespace {
bool valid_id(PageId id) { return id > kIndexHeaderPage && id != kInvalidPageId; }
void check_capacity(std::uint16_t capacity, ErrorCode code) {
  if (capacity < 3 || capacity > kIndexMaxKeys) throw Error(code, "invalid B+ tree node capacity");
}
void check_header(const IndexHeader& header, ErrorCode code) {
  check_capacity(header.max_keys, code);
  if ((header.size == 0) != (header.root == kInvalidPageId) ||
      (header.root != kInvalidPageId && !valid_id(header.root)))
    throw Error(code, "invalid B+ tree root/size metadata");
}
void check_node(const IndexNode& node, std::uint16_t capacity, ErrorCode code) {
  check_capacity(capacity, code);
  if (!valid_id(node.id) || (node.type != PageType::IndexLeaf && node.type != PageType::IndexInternal) ||
      node.keys.size() > capacity ||
      (node.parent != kInvalidPageId && (!valid_id(node.parent) || node.parent == node.id)) ||
      !std::is_sorted(node.keys.begin(), node.keys.end()) ||
      std::adjacent_find(node.keys.begin(), node.keys.end()) != node.keys.end())
    throw Error(code, "invalid B+ tree node header or key ordering");
  if (node.leaf()) {
    if (node.values.size() != node.keys.size() || !node.children.empty() ||
        (node.next != kInvalidPageId && (!valid_id(node.next) || node.next == node.id)))
      throw Error(code, "invalid B+ tree leaf layout");
    for (const auto rid : node.values)
      if (rid.page == 0 || rid.page == kInvalidPageId || rid.generation == 0)
        throw Error(code, "invalid indexed record ID");
  } else {
    if (!node.values.empty() || node.children.size() != node.keys.size() + 1 || node.next != kInvalidPageId)
      throw Error(code, "invalid B+ tree internal layout");
    for (const auto child : node.children)
      if (!valid_id(child) || child == node.id) throw Error(code, "invalid B+ tree child ID");
  }
}
}  // namespace

Page encode_index_header(const IndexHeader& header) {
  check_header(header, ErrorCode::InvalidArgument);
  Page page = Page::create(kIndexHeaderPage, PageType::IndexHeader);
  encoding::store<PageId>(page.bytes, 24, header.root);
  encoding::store<std::uint16_t>(page.bytes, 28, header.max_keys);
  encoding::store<std::uint64_t>(page.bytes, 32, header.size);
  page.seal();
  return page;
}
IndexHeader decode_index_header(const Page& page) {
  page.validate(kIndexHeaderPage);
  if (page.type() != PageType::IndexHeader || encoding::load<std::uint16_t>(page.bytes, 30) != 0)
    throw Error(ErrorCode::Corruption, "expected B+ tree header page");
  IndexHeader header{encoding::load<PageId>(page.bytes, 24),
                     encoding::load<std::uint16_t>(page.bytes, 28),
                     encoding::load<std::uint64_t>(page.bytes, 32)};
  check_header(header, ErrorCode::Corruption);
  return header;
}
Page encode_index_node(const IndexNode& node, std::uint16_t max_keys) {
  check_node(node, max_keys, ErrorCode::InvalidArgument);
  Page page = Page::create(node.id, node.type);
  encoding::store<std::uint16_t>(page.bytes, 24, static_cast<std::uint16_t>(node.keys.size()));
  encoding::store<PageId>(page.bytes, 28, node.parent);
  encoding::store<PageId>(page.bytes, 32, node.next);
  if (node.leaf()) {
    for (std::size_t i = 0; i < node.keys.size(); ++i) {
      const auto base = 40 + i * 22;
      encoding::store<std::uint64_t>(page.bytes, base, std::bit_cast<std::uint64_t>(node.keys[i]));
      encoding::store<PageId>(page.bytes, base + 8, node.values[i].page);
      encoding::store<std::uint16_t>(page.bytes, base + 12, node.values[i].slot);
      encoding::store<std::uint64_t>(page.bytes, base + 14, node.values[i].generation);
    }
  } else {
    encoding::store<PageId>(page.bytes, 40, node.children.front());
    for (std::size_t i = 0; i < node.keys.size(); ++i) {
      const auto base = 44 + i * 12;
      encoding::store<std::uint64_t>(page.bytes, base, std::bit_cast<std::uint64_t>(node.keys[i]));
      encoding::store<PageId>(page.bytes, base + 8, node.children[i + 1]);
    }
  }
  page.seal();
  return page;
}
IndexNode decode_index_node(const Page& page, std::uint16_t max_keys) {
  page.validate(page.id());
  check_capacity(max_keys, ErrorCode::Corruption);
  const auto count = encoding::load<std::uint16_t>(page.bytes, 24);
  if ((page.type() != PageType::IndexLeaf && page.type() != PageType::IndexInternal) || count > max_keys ||
      encoding::load<std::uint16_t>(page.bytes, 26) != 0 || encoding::load<std::uint32_t>(page.bytes, 36) != 0)
    throw Error(ErrorCode::Corruption, "invalid B+ tree node type or entry count");
  IndexNode node;
  node.id = page.id();
  node.type = page.type();
  node.parent = encoding::load<PageId>(page.bytes, 28);
  node.next = encoding::load<PageId>(page.bytes, 32);
  if (node.leaf()) {
    for (std::size_t i = 0; i < count; ++i) {
      const auto base = 40 + i * 22;
      node.keys.push_back(std::bit_cast<IndexKey>(encoding::load<std::uint64_t>(page.bytes, base)));
      node.values.push_back({encoding::load<PageId>(page.bytes, base + 8),
                             encoding::load<std::uint16_t>(page.bytes, base + 12),
                             encoding::load<std::uint64_t>(page.bytes, base + 14)});
    }
  } else {
    node.children.push_back(encoding::load<PageId>(page.bytes, 40));
    for (std::size_t i = 0; i < count; ++i) {
      const auto base = 44 + i * 12;
      node.keys.push_back(std::bit_cast<IndexKey>(encoding::load<std::uint64_t>(page.bytes, base)));
      node.children.push_back(encoding::load<PageId>(page.bytes, base + 8));
    }
  }
  check_node(node, max_keys, ErrorCode::Corruption);
  return node;
}
}  // namespace tinydb
