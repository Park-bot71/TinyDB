#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <vector>

#include "storage/disk_manager.h"

namespace tinydb {
using Lsn = std::uint64_t;
namespace detail { struct BufferPoolState; }

class ReadPageGuard {
 public:
  ReadPageGuard() = default;
  ~ReadPageGuard();
  ReadPageGuard(ReadPageGuard&& other) noexcept;
  ReadPageGuard& operator=(ReadPageGuard&& other) noexcept;
  ReadPageGuard(const ReadPageGuard&) = delete;
  ReadPageGuard& operator=(const ReadPageGuard&) = delete;
  [[nodiscard]] const Page& page() const;
  [[nodiscard]] explicit operator bool() const noexcept { return state_ != nullptr; }
  void drop() noexcept;
 private:
  friend class BufferPool;
  ReadPageGuard(std::shared_ptr<detail::BufferPoolState> state, std::size_t frame);
  std::shared_ptr<detail::BufferPoolState> state_;
  std::size_t frame_ = 0;
  std::shared_lock<std::shared_mutex> latch_;
};

class WritePageGuard {
 public:
  WritePageGuard() = default;
  ~WritePageGuard();
  WritePageGuard(WritePageGuard&& other) noexcept;
  WritePageGuard& operator=(WritePageGuard&& other) noexcept;
  WritePageGuard(const WritePageGuard&) = delete;
  WritePageGuard& operator=(const WritePageGuard&) = delete;
  [[nodiscard]] const Page& page() const;
  // Copies a validated image, sets its LSN and checksum, and marks it dirty.
  // No mutable page reference escapes; failed replacement leaves the frame unchanged.
  void replace(Page image, Lsn lsn = 0);
  [[nodiscard]] explicit operator bool() const noexcept { return state_ != nullptr; }
  void drop() noexcept;
 private:
  friend class BufferPool;
  WritePageGuard(std::shared_ptr<detail::BufferPoolState> state, std::size_t frame);
  std::shared_ptr<detail::BufferPoolState> state_;
  std::size_t frame_ = 0;
  std::unique_lock<std::shared_mutex> latch_;
};

struct BufferPoolStats {
  std::uint64_t hits = 0, misses = 0, disk_reads = 0, page_writes = 0, evictions = 0;
  std::size_t capacity = 0, resident_pages = 0, pinned_pages = 0, pins = 0, dirty_pages = 0;
  [[nodiscard]] double hit_rate() const noexcept {
    return hits + misses == 0 ? 0.0 : static_cast<double>(hits) / static_cast<double>(hits + misses);
  }
};

// Fixed frames, CLOCK replacement, synchronous I/O. Disk must outlive pool operations.
// Do not bypass this cache to mutate/close its DiskManager. The WAL callback must
// force through the supplied LSN or throw; it must not reenter the pool or latch pages.
class BufferPool {
 public:
  using ForceWal = std::function<void(Lsn)>;
  BufferPool(DiskManager& disk, std::size_t frames, ForceWal force_wal = {});
  ~BufferPool();  // No implicit flush: call sync()/close() and check errors.
  BufferPool(const BufferPool&) = delete;
  BufferPool& operator=(const BufferPool&) = delete;
  [[nodiscard]] ReadPageGuard fetch_read(PageId id);
  [[nodiscard]] WritePageGuard fetch_write(PageId id);
  [[nodiscard]] WritePageGuard new_page(PageType type = PageType::Heap);
  void delete_page(PageId id);  // Busy if pinned; discards any cached image after free.
  void flush_page(PageId id);   // Busy if pinned; nonresident page is validated, then a no-op.
  void flush_all();             // Busy if ANY frame is pinned, before writing anything.
  void sync();                  // flush_all + fsync; flush alone is not a durability boundary.
  void close();                 // sync + reject future operations; does not close the disk.
  // Discard images after out-of-cache physical undo. Requires exclusive owner
  // quiescence and no pins; resets the pool failure flag after disk restoration.
  void discard_all();
  void attach_wal(std::shared_ptr<Wal> wal, std::uint32_t file_id);
  [[nodiscard]] BufferPoolStats stats() const;
  [[nodiscard]] std::vector<PageId> allocated_pages() const;
  [[nodiscard]] PageId page_count() const;
  [[nodiscard]] std::uint64_t reserve_generation();
 private:
  std::shared_ptr<detail::BufferPoolState> state_;
};
}  // namespace tinydb
