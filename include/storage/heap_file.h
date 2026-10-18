#pragma once

#include <filesystem>
#include <mutex>
#include <utility>
#include <vector>

#include "buffer/buffer_pool.h"
#include "storage/slotted_page.h"

namespace tinydb {
// A single unindexed heap per file. Owns its disk manager and bounded page cache.
class HeapFile {
 public:
  explicit HeapFile(const std::filesystem::path& path, std::size_t buffer_frames = 64);
  RecordId insert(std::span<const std::byte> record);
  [[nodiscard]] Bytes get(RecordId id) const;
  void update(RecordId id, std::span<const std::byte> record);
  void erase(RecordId id);
  [[nodiscard]] std::vector<std::pair<RecordId, Bytes>> scan() const;
  [[nodiscard]] PageId page_count() const;
  [[nodiscard]] BufferPoolStats buffer_stats() const;
  void sync();
  void close();
 private:
  mutable std::mutex mutex_;
  DiskManager disk_;
  mutable BufferPool pool_;  // Reads pin/touch frames, even for const heap methods.
};
}  // namespace tinydb
