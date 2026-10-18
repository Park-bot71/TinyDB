#include "storage/heap_file.h"

#include "common/error.h"

namespace tinydb {
HeapFile::HeapFile(const std::filesystem::path& path) : disk_(path) {
  for (const auto id : disk_.allocated_pages()) SlottedPage(disk_.read_page(id)).validate();
}
RecordId HeapFile::insert(std::span<const std::byte> record) {
  if (record.empty() || record.size() > SlottedPage::kMaxRecordSize)
    throw Error(ErrorCode::InvalidArgument, "record must contain 1..4052 bytes");
  std::lock_guard lock(mutex_);
  for (const auto id : disk_.allocated_pages()) {
    SlottedPage page(disk_.read_page(id));
    if (page.can_insert(record.size())) {
      const auto rid = page.insert(record, disk_.reserve_generation());
      disk_.write_page(page.page());
      return rid;
    }
  }
  const auto id = disk_.allocate_page();
  SlottedPage page(disk_.read_page(id));
  const auto rid = page.insert(record, disk_.reserve_generation());
  disk_.write_page(page.page());
  return rid;
}
Bytes HeapFile::get(RecordId id) const {
  std::lock_guard lock(mutex_);
  return SlottedPage(disk_.read_page(id.page)).get(id);
}
void HeapFile::update(RecordId id, std::span<const std::byte> record) {
  std::lock_guard lock(mutex_);
  SlottedPage page(disk_.read_page(id.page));
  page.update(id, record);
  disk_.write_page(page.page());
}
void HeapFile::erase(RecordId id) {
  std::lock_guard lock(mutex_);
  SlottedPage page(disk_.read_page(id.page));
  page.erase(id);
  if (page.live_count() == 0) disk_.deallocate_page(id.page);
  else disk_.write_page(page.page());
}
std::vector<std::pair<RecordId, Bytes>> HeapFile::scan() const {
  std::lock_guard lock(mutex_);
  std::vector<std::pair<RecordId, Bytes>> result;
  for (const auto id : disk_.allocated_pages()) {
    SlottedPage page(disk_.read_page(id));
    for (const auto rid : page.records()) result.emplace_back(rid, page.get(rid));
  }
  return result;
}
PageId HeapFile::page_count() const { std::lock_guard lock(mutex_); return disk_.page_count(); }
void HeapFile::sync() { std::lock_guard lock(mutex_); disk_.sync(); }
void HeapFile::close() { std::lock_guard lock(mutex_); disk_.close(); }
}  // namespace tinydb
