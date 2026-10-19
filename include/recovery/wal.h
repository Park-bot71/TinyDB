#pragma once

#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string_view>

#include "storage/page.h"

namespace tinydb {
using FileId = std::uint32_t;
using FaultHook = std::function<void(std::string_view)>;
inline constexpr FileId kHeapFileId = 1, kIndexFileId = 2;
enum class LogType : std::uint16_t { Begin = 1, Update = 2, Commit = 3, Abort = 4 };
struct WalStats {
  std::uint64_t records = 0, updates = 0, durable_lsn = 0;
  std::uint64_t recovered_commits = 0, recovered_losers = 0, redo_pages = 0, trimmed_bytes = 0;
};

// Full-history physical WAL. Recovery reconstructs a committed state from the
// immutable baseline before normal page/allocator validation. No log truncation
// or incremental ARIES/CLR protocol is claimed.
class Wal {
 public:
  Wal(const std::filesystem::path& directory, std::size_t frames, std::uint16_t max_keys, FaultHook hook = {});
  ~Wal();
  Wal(const Wal&) = delete;
  Wal& operator=(const Wal&) = delete;
  void begin(std::uint64_t tx);
  void start_writer(std::uint64_t tx);
  void finish_writer(std::uint64_t tx);
  Page log_page(FileId file, std::optional<Page> before, Page after);
  void force(std::uint64_t lsn);
  void commit(std::uint64_t tx);
  void abort(std::uint64_t tx);
  void close();
  [[nodiscard]] bool rollback_allowed() const;
  [[nodiscard]] bool committed(std::uint64_t tx) const;
  [[nodiscard]] std::uint64_t max_transaction_id() const;
  [[nodiscard]] WalStats stats() const;
  void fault(std::string_view point) const;
 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
}  // namespace tinydb
