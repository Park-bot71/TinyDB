#pragma once

#include <span>
#include <vector>

#include "storage/page.h"

namespace tinydb {
using Bytes = std::vector<std::byte>;
struct RecordId {
  PageId page = kInvalidPageId;
  std::uint16_t slot = 0;
  std::uint64_t generation = 0;
  bool operator==(const RecordId&) const = default;
};

class SlottedPage {
 public:
  static constexpr std::size_t kSlotSize = 12;
  static constexpr std::size_t kMaxRecordSize = kPageSize - kPageHeaderSize - kSlotSize;
  explicit SlottedPage(Page page);
  [[nodiscard]] const Page& page() const noexcept { return page_; }
  [[nodiscard]] std::uint16_t slot_count() const;
  [[nodiscard]] std::uint16_t live_count() const;
  [[nodiscard]] std::size_t free_space() const;
  [[nodiscard]] bool can_insert(std::size_t size) const;
  RecordId insert(std::span<const std::byte> record, std::uint64_t generation);
  [[nodiscard]] Bytes get(RecordId id) const;
  void update(RecordId id, std::span<const std::byte> record);
  void erase(RecordId id);
  [[nodiscard]] std::vector<RecordId> records() const;
  void validate() const;

 private:
  struct Slot { std::uint16_t offset, length; std::uint64_t generation; };
  Slot slot(std::uint16_t index) const;
  Slot checked_slot(RecordId id) const;
  void rewrite(const std::vector<Bytes>& records, const std::vector<std::uint64_t>& generations);
  void unpack(std::vector<Bytes>& records, std::vector<std::uint64_t>& generations) const;
  Page page_;
};
}  // namespace tinydb
