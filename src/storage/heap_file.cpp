#include "storage/heap_file.h"

#include "common/error.h"
#include "recovery/wal.h"

namespace tinydb {
HeapFile::HeapFile(const std::filesystem::path& path, std::size_t buffer_frames)
    : disk_(path), pool_(disk_, buffer_frames) {
  for (const auto id : pool_.allocated_pages()) {
    const auto guard = pool_.fetch_read(id);
    SlottedPage(guard.page()).validate();
  }
}
RecordId HeapFile::insert(std::span<const std::byte> record) {
  if (record.empty() || record.size() > SlottedPage::kMaxRecordSize)
    throw Error(ErrorCode::InvalidArgument, "record must contain 1..4052 bytes");
  std::lock_guard lock(mutex_);
  for (const auto id : pool_.allocated_pages()) {
    auto guard = pool_.fetch_write(id);
    SlottedPage page(guard.page());
    if (page.can_insert(record.size())) {
      const auto rid = page.insert(record, pool_.reserve_generation());
      guard.replace(page.page());
      return rid;
    }
  }
  auto guard = pool_.new_page();
  SlottedPage page(guard.page());
  const auto rid = page.insert(record, pool_.reserve_generation());
  guard.replace(page.page());
  return rid;
}
Bytes HeapFile::get(RecordId id) const {
  std::lock_guard lock(mutex_);
  const auto guard = pool_.fetch_read(id.page);
  return SlottedPage(guard.page()).get(id);
}
void HeapFile::update(RecordId id, std::span<const std::byte> record) {
  std::lock_guard lock(mutex_);
  auto guard = pool_.fetch_write(id.page);
  SlottedPage page(guard.page());
  page.update(id, record);
  guard.replace(page.page());
}
void HeapFile::erase(RecordId id) {
  std::lock_guard lock(mutex_);
  auto guard = pool_.fetch_write(id.page);
  SlottedPage page(guard.page());
  page.erase(id);
  if (page.live_count() == 0) {
    guard.drop();
    pool_.delete_page(id.page);
  } else guard.replace(page.page());
}
std::vector<std::pair<RecordId, Bytes>> HeapFile::scan() const {
  std::lock_guard lock(mutex_);
  std::vector<std::pair<RecordId, Bytes>> result;
  for (const auto id : pool_.allocated_pages()) {
    const auto guard = pool_.fetch_read(id);
    SlottedPage page(guard.page());
    for (const auto rid : page.records()) result.emplace_back(rid, page.get(rid));
  }
  return result;
}
PageId HeapFile::page_count() const { std::lock_guard lock(mutex_); return pool_.page_count(); }
BufferPoolStats HeapFile::buffer_stats() const { std::lock_guard lock(mutex_); return pool_.stats(); }
void HeapFile::sync() { std::lock_guard lock(mutex_); pool_.sync(); }
void HeapFile::close() { std::lock_guard lock(mutex_); pool_.close(); disk_.close(); }
void HeapFile::begin_undo() { std::lock_guard lock(mutex_); pool_.sync(); disk_.begin_undo(); }
void HeapFile::commit_undo() { std::lock_guard lock(mutex_); disk_.commit_undo(); }
void HeapFile::rollback_undo() {
  std::lock_guard lock(mutex_);
  disk_.rollback_undo();
  pool_.discard_all();
}
void HeapFile::attach_wal(std::shared_ptr<Wal> wal) {
  std::lock_guard lock(mutex_); pool_.sync();
  disk_.attach_wal(wal, kHeapFileId); pool_.attach_wal(std::move(wal), kHeapFileId);
}
}  // namespace tinydb
