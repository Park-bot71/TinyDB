#include "storage/slotted_page.h"

#include <algorithm>
#include <utility>

#include "common/encoding.h"

namespace tinydb {
namespace {
void check_size(std::size_t size) {
  if (size == 0 || size > SlottedPage::kMaxRecordSize)
    throw Error(ErrorCode::InvalidArgument, "record must contain 1..4052 bytes");
}
}  // namespace
SlottedPage::SlottedPage(Page page) : page_(std::move(page)) { validate(); }
std::uint16_t SlottedPage::slot_count() const { return encoding::load<std::uint16_t>(page_.bytes, 24); }
std::uint16_t SlottedPage::live_count() const { return encoding::load<std::uint16_t>(page_.bytes, 28); }
SlottedPage::Slot SlottedPage::slot(std::uint16_t index) const {
  const std::size_t base = kPageHeaderSize + index * kSlotSize;
  return {encoding::load<std::uint16_t>(page_.bytes, base),
          encoding::load<std::uint16_t>(page_.bytes, base + 2),
          encoding::load<std::uint64_t>(page_.bytes, base + 4)};
}
std::size_t SlottedPage::free_space() const {
  return encoding::load<std::uint16_t>(page_.bytes, 26) - kPageHeaderSize - slot_count() * kSlotSize;
}
bool SlottedPage::can_insert(std::size_t size) const {
  return size > 0 && size <= kMaxRecordSize &&
         size + (live_count() < slot_count() ? 0 : kSlotSize) <= free_space();
}
void SlottedPage::validate() const {
  page_.validate(page_.id());
  if (page_.id() == 0 || page_.id() == kInvalidPageId || page_.type() != PageType::Heap)
    throw Error(ErrorCode::Corruption, "expected heap page");
  const auto count = slot_count();
  const auto end = encoding::load<std::uint16_t>(page_.bytes, 26);
  if (count > (kPageSize - kPageHeaderSize) / kSlotSize ||
      end < kPageHeaderSize + count * kSlotSize || end > kPageSize ||
      live_count() > count || encoding::load<std::uint16_t>(page_.bytes, 30) != 0)
    throw Error(ErrorCode::Corruption, "invalid slot directory bounds");
  std::vector<std::pair<std::size_t, std::size_t>> intervals;
  for (std::uint16_t i = 0; i < count; ++i) {
    const auto s = slot(i);
    if (s.generation == 0 || (s.length == 0 && s.offset != 0) ||
        (s.length > 0 && (s.offset < end || static_cast<std::size_t>(s.offset) + s.length > kPageSize)))
      throw Error(ErrorCode::Corruption, "invalid record slot");
    if (s.length) intervals.emplace_back(s.offset, static_cast<std::size_t>(s.offset) + s.length);
  }
  if (intervals.size() != live_count()) throw Error(ErrorCode::Corruption, "incorrect live record count");
  std::sort(intervals.begin(), intervals.end());
  std::size_t cursor = end;
  for (const auto& [begin, finish] : intervals) {
    if (begin != cursor) throw Error(ErrorCode::Corruption, "overlapping or fragmented record payload");
    cursor = finish;
  }
  if (cursor != kPageSize) throw Error(ErrorCode::Corruption, "invalid payload boundary");
}
SlottedPage::Slot SlottedPage::checked_slot(RecordId id) const {
  if (id.page != page_.id() || id.slot >= slot_count() || id.generation == 0)
    throw Error(ErrorCode::NotFound, "invalid record ID");
  const Slot s = slot(id.slot);
  if (s.length == 0 || s.generation != id.generation)
    throw Error(ErrorCode::NotFound, "deleted or stale record ID");
  return s;
}
Bytes SlottedPage::get(RecordId id) const {
  const auto s = checked_slot(id);
  return Bytes(page_.bytes.begin() + s.offset, page_.bytes.begin() + s.offset + s.length);
}
std::vector<RecordId> SlottedPage::records() const {
  std::vector<RecordId> result;
  for (std::uint16_t i = 0; i < slot_count(); ++i) {
    const auto s = slot(i);
    if (s.length) result.push_back({page_.id(), i, s.generation});
  }
  return result;
}
void SlottedPage::unpack(std::vector<Bytes>& records, std::vector<std::uint64_t>& generations) const {
  for (std::uint16_t i = 0; i < slot_count(); ++i) {
    const auto s = slot(i);
    records.emplace_back(page_.bytes.begin() + s.offset, page_.bytes.begin() + s.offset + s.length);
    generations.push_back(s.generation);
  }
}
void SlottedPage::rewrite(const std::vector<Bytes>& records, const std::vector<std::uint64_t>& generations) {
  std::size_t needed = kPageHeaderSize + records.size() * kSlotSize;
  for (const auto& record : records) needed += record.size();
  if (needed > kPageSize) throw Error(ErrorCode::PageFull, "record does not fit on its page");
  Page updated = Page::create(page_.id(), PageType::Heap);
  encoding::store<std::uint64_t>(updated.bytes, 16, encoding::load<std::uint64_t>(page_.bytes, 16));
  std::size_t end = kPageSize;
  std::uint16_t live = 0;
  for (std::size_t i = 0; i < records.size(); ++i) {
    const auto& record = records[i];
    const auto base = kPageHeaderSize + i * kSlotSize;
    if (!record.empty()) {
      end -= record.size();
      std::copy(record.begin(), record.end(), updated.bytes.begin() + end);
      encoding::store<std::uint16_t>(updated.bytes, base, static_cast<std::uint16_t>(end));
      encoding::store<std::uint16_t>(updated.bytes, base + 2, static_cast<std::uint16_t>(record.size()));
      ++live;
    }
    encoding::store<std::uint64_t>(updated.bytes, base + 4, generations[i]);
  }
  encoding::store<std::uint16_t>(updated.bytes, 24, static_cast<std::uint16_t>(records.size()));
  encoding::store<std::uint16_t>(updated.bytes, 26, static_cast<std::uint16_t>(end));
  encoding::store<std::uint16_t>(updated.bytes, 28, live);
  updated.seal();
  page_ = updated;
}
RecordId SlottedPage::insert(std::span<const std::byte> record, std::uint64_t generation) {
  check_size(record.size());
  if (generation == 0) throw Error(ErrorCode::InvalidArgument, "generation must be nonzero");
  if (!can_insert(record.size())) throw Error(ErrorCode::PageFull, "heap page is full");
  std::vector<Bytes> records;
  std::vector<std::uint64_t> generations;
  unpack(records, generations);
  auto found = std::find_if(records.begin(), records.end(), [](const Bytes& r) { return r.empty(); });
  const auto index = static_cast<std::uint16_t>(found - records.begin());
  if (found == records.end()) {
    records.emplace_back(record.begin(), record.end());
    generations.push_back(generation);
  } else {
    *found = Bytes(record.begin(), record.end());
    generations[index] = generation;
  }
  rewrite(records, generations);
  return {page_.id(), index, generation};
}
void SlottedPage::update(RecordId id, std::span<const std::byte> record) {
  (void)checked_slot(id);
  check_size(record.size());
  std::vector<Bytes> records;
  std::vector<std::uint64_t> generations;
  unpack(records, generations);
  records[id.slot] = Bytes(record.begin(), record.end());
  rewrite(records, generations);
}
void SlottedPage::erase(RecordId id) {
  (void)checked_slot(id);
  std::vector<Bytes> records;
  std::vector<std::uint64_t> generations;
  unpack(records, generations);
  records[id.slot].clear();
  rewrite(records, generations);
}
}  // namespace tinydb
