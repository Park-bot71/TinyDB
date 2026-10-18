#pragma once

#include <filesystem>
#include <mutex>
#include <utility>
#include <vector>

#include "storage/disk_manager.h"
#include "storage/slotted_page.h"

namespace tinydb {
// A single unindexed heap per file in Phase 1. Owns its disk manager.
class HeapFile {
 public:
  explicit HeapFile(const std::filesystem::path& path);
  RecordId insert(std::span<const std::byte> record);
  [[nodiscard]] Bytes get(RecordId id) const;
  void update(RecordId id, std::span<const std::byte> record);
  void erase(RecordId id);
  [[nodiscard]] std::vector<std::pair<RecordId, Bytes>> scan() const;
  [[nodiscard]] PageId page_count() const;
  void sync();
  void close();
 private:
  mutable std::mutex mutex_;
  DiskManager disk_;
};
}  // namespace tinydb
