#pragma once

#include <filesystem>
#include <limits>
#include <optional>
#include <shared_mutex>
#include <utility>

#include "buffer/buffer_pool.h"
#include "index/index_page.h"

namespace tinydb {
namespace detail { struct DatabaseState; }
struct BPlusTreeOptions {
  std::size_t buffer_frames = 64;
  // Zero uses the persisted capacity on reopen, or the physical maximum for a new file.
  std::uint16_t max_keys = 0;
};
struct TreeValidation {
  std::size_t height = 0, nodes = 0, leaves = 0;
  std::uint64_t entries = 0;
};
struct TreeStats {
  std::uint64_t leaf_splits = 0, internal_splits = 0, leaf_merges = 0, internal_merges = 0;
  std::uint64_t borrowed_from_left = 0, borrowed_from_right = 0, internal_redistributions = 0;
  std::uint64_t root_splits = 0, root_collapses = 0;
};

// One independently persisted unique-key index per file. Coarse tree latch:
// shared readers, exclusive mutations; at most one buffer guard at a time.
// Heap/index maintenance is caller-owned and is NOT transactionally atomic yet.
class BPlusTree {
 public:
  explicit BPlusTree(const std::filesystem::path& path, BPlusTreeOptions options = {});
  bool insert(IndexKey key, RecordId value);  // false on duplicate, original value preserved.
  bool erase(IndexKey key);                  // false if absent.
  [[nodiscard]] std::optional<RecordId> find(IndexKey key) const;
  using Entry = std::pair<IndexKey, RecordId>;
  // Half-open [lower, upper); omitted bounds include the full signed key domain.
  [[nodiscard]] std::vector<Entry> scan(std::optional<IndexKey> lower = {},
                                      std::optional<IndexKey> upper = {},
                                      std::size_t limit = std::numeric_limits<std::size_t>::max()) const;
  [[nodiscard]] std::vector<Entry> range(IndexKey lower, IndexKey upper,
                                       std::size_t limit = std::numeric_limits<std::size_t>::max()) const {
    return scan(lower, upper, limit);
  }
  [[nodiscard]] std::uint64_t size() const;
  [[nodiscard]] TreeValidation validate() const;
  [[nodiscard]] TreeStats stats() const;
  [[nodiscard]] BufferPoolStats buffer_stats() const;
  void sync();
  void close();
 private:
  friend struct detail::DatabaseState;
  void begin_undo();
  void commit_undo();
  void rollback_undo();
  void attach_wal(std::shared_ptr<Wal> wal);
  void check_open() const;
  IndexNode read_node(PageId id) const;
  void write_node(const IndexNode& node);
  PageId allocate_node(IndexNode node);
  void write_header();
  IndexNode find_leaf(IndexKey key) const;
  IndexKey minimum(PageId id) const;
  void set_parent(PageId child, PageId parent);
  void refresh_ancestors(PageId id);
  void insert_parent(PageId left, PageId right, IndexKey separator);
  void rebalance(PageId id);
  std::size_t min_keys(const IndexNode& node) const;
  TreeValidation validate_unlocked() const;
  mutable std::shared_mutex mutex_;
  DiskManager disk_;
  mutable BufferPool pool_;
  IndexHeader header_;
  TreeStats stats_;
  bool closed_ = false, failed_ = false;
};
}  // namespace tinydb
