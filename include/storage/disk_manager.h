#pragma once

#include <filesystem>
#include <mutex>
#include <memory>
#include <unordered_set>
#include <vector>

#include "storage/page.h"

namespace tinydb {
class Wal;
// Exclusive file ownership; thread safe. Errors after a write starts poison the handle.
// No implicit durability in the destructor: use sync() or close() to check errors.
class DiskManager {
 public:
  explicit DiskManager(const std::filesystem::path& path);
  ~DiskManager();
  DiskManager(const DiskManager&) = delete;
  DiskManager& operator=(const DiskManager&) = delete;
  [[nodiscard]] Page read_page(PageId id) const;
  void write_page(Page page);
  [[nodiscard]] PageId allocate_page(PageType type = PageType::Heap);
  void deallocate_page(PageId id);
  [[nodiscard]] std::uint64_t reserve_generation();
  [[nodiscard]] std::vector<PageId> allocated_pages() const;
  [[nodiscard]] PageId page_count() const;
  void sync();
  void close();
  // Volatile physical undo. Caller must flush its cache before begin and discard
  // cached images after rollback. One writer; these are NOT crash recovery APIs.
  void begin_undo();
  void commit_undo();
  void rollback_undo();
  void attach_wal(std::shared_ptr<Wal> wal, std::uint32_t file_id);

 private:
  void check_open() const;
  void check_allocated(PageId id) const;
  Page read_raw(PageId id) const;
  void write_raw(Page page, bool logged = false);
  void write_metadata();
  void sync_unlocked();
  void load_existing();
  struct UndoState;
  std::unique_ptr<UndoState> undo_;
  std::shared_ptr<Wal> wal_;
  std::uint32_t file_id_ = 0;
  bool restoring_ = false;
  mutable std::mutex mutex_;
  int fd_ = -1;
  bool failed_ = false;
  PageId page_count_ = 1;
  PageId free_head_ = kInvalidPageId;
  std::uint64_t next_generation_ = 1;
  std::unordered_set<PageId> free_pages_;
  std::vector<PageType> page_types_{PageType::Metadata};
};
}  // namespace tinydb
