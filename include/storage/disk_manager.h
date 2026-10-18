#pragma once

#include <filesystem>
#include <mutex>
#include <unordered_set>
#include <vector>

#include "storage/page.h"

namespace tinydb {
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
  [[nodiscard]] PageId allocate_page();
  void deallocate_page(PageId id);
  [[nodiscard]] std::uint64_t reserve_generation();
  [[nodiscard]] std::vector<PageId> allocated_pages() const;
  [[nodiscard]] PageId page_count() const;
  void sync();
  void close();

 private:
  void check_open() const;
  void check_allocated(PageId id) const;
  Page read_raw(PageId id) const;
  void write_raw(const Page& page);
  void write_metadata();
  void sync_unlocked();
  mutable std::mutex mutex_;
  int fd_ = -1;
  bool failed_ = false;
  PageId page_count_ = 1;
  PageId free_head_ = kInvalidPageId;
  std::uint64_t next_generation_ = 1;
  std::unordered_set<PageId> free_pages_;
};
}  // namespace tinydb
