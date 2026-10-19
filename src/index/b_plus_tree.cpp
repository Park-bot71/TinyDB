#include "index/b_plus_tree.h"

#include <algorithm>
#include <functional>
#include <unordered_set>

#include "common/error.h"
#include "recovery/wal.h"

namespace tinydb {
namespace {
constexpr std::size_t kMaxHeight = 64;
std::filesystem::path checked_path(const std::filesystem::path& path, BPlusTreeOptions options) {
  if (options.buffer_frames == 0 || (options.max_keys != 0 && (options.max_keys < 3 || options.max_keys > kIndexMaxKeys)))
    throw Error(ErrorCode::InvalidArgument, "B+ tree requires positive frames and capacity 3..184 (or zero for default)");
  return path;
}
template <typename T> auto position(std::vector<T>& values, std::size_t index) {
  return values.begin() + static_cast<std::ptrdiff_t>(index);
}
std::size_t child_position(const IndexNode& parent, PageId child) {
  const auto found = std::find(parent.children.begin(), parent.children.end(), child);
  if (found == parent.children.end()) throw Error(ErrorCode::Corruption, "B+ tree parent does not reference child");
  return static_cast<std::size_t>(found - parent.children.begin());
}
}  // namespace

BPlusTree::BPlusTree(const std::filesystem::path& path, BPlusTreeOptions options)
    : disk_(checked_path(path, options)), pool_(disk_, options.buffer_frames) {
  if (pool_.page_count() == 1) {
    header_.max_keys = options.max_keys == 0 ? kIndexMaxKeys : options.max_keys;
    {
      auto guard = pool_.new_page(PageType::IndexHeader);
      if (guard.page().id() != kIndexHeaderPage) throw Error(ErrorCode::Corruption, "index header must be page 1");
      guard.replace(encode_index_header(header_));
    }
    pool_.sync();  // Opening a new index leaves a durable, valid empty index.
  } else {
    const auto guard = pool_.fetch_read(kIndexHeaderPage);
    header_ = decode_index_header(guard.page());
    if (options.max_keys != 0 && options.max_keys != header_.max_keys)
      throw Error(ErrorCode::InvalidArgument, "requested node capacity differs from persisted index capacity");
  }
  (void)validate_unlocked();
}
void BPlusTree::check_open() const {
  if (closed_) throw Error(ErrorCode::Closed, "B+ tree is closed");
  if (failed_) throw Error(ErrorCode::Io, "B+ tree mutation failed; stop using it and reopen (repair may be required)");
}
IndexNode BPlusTree::read_node(PageId id) const {
  try {
    const auto guard = pool_.fetch_read(id);
    return decode_index_node(guard.page(), header_.max_keys);
  } catch (const Error& error) {
    if (error.code() == ErrorCode::NotFound)
      throw Error(ErrorCode::Corruption, "B+ tree references an unallocated page");
    throw;
  }
}
void BPlusTree::write_node(const IndexNode& node) {
  const auto image = encode_index_node(node, header_.max_keys);
  auto guard = pool_.fetch_write(node.id);
  guard.replace(image);
}
PageId BPlusTree::allocate_node(IndexNode node) {
  auto guard = pool_.new_page(node.type);
  node.id = guard.page().id();
  guard.replace(encode_index_node(node, header_.max_keys));
  return node.id;
}
void BPlusTree::write_header() {
  auto guard = pool_.fetch_write(kIndexHeaderPage);
  guard.replace(encode_index_header(header_));
}
IndexNode BPlusTree::find_leaf(IndexKey key) const {
  auto id = header_.root;
  for (std::size_t depth = 0; depth < kMaxHeight; ++depth) {
    auto node = read_node(id);
    if (node.leaf()) return node;
    const auto index = static_cast<std::size_t>(std::upper_bound(node.keys.begin(), node.keys.end(), key) - node.keys.begin());
    id = node.children[index];
  }
  throw Error(ErrorCode::Corruption, "B+ tree path exceeds maximum height (possible cycle)");
}
IndexKey BPlusTree::minimum(PageId id) const {
  for (std::size_t depth = 0; depth < kMaxHeight; ++depth) {
    const auto node = read_node(id);
    if (node.leaf()) {
      if (node.keys.empty()) throw Error(ErrorCode::Corruption, "empty leaf in a nonempty subtree");
      return node.keys.front();
    }
    id = node.children.front();
  }
  throw Error(ErrorCode::Corruption, "cyclic B+ tree minimum path");
}
void BPlusTree::set_parent(PageId child, PageId parent) {
  auto node = read_node(child);
  node.parent = parent;
  write_node(node);
}
void BPlusTree::refresh_ancestors(PageId id) {
  for (std::size_t depth = 0; depth < kMaxHeight; ++depth) {
    const auto node = read_node(id);
    if (node.parent == kInvalidPageId) return;
    auto parent = read_node(node.parent);
    const auto index = child_position(parent, id);
    if (index != 0) {
      const auto key = minimum(id);
      if (parent.keys[index - 1] != key) {
        parent.keys[index - 1] = key;
        write_node(parent);
      }
    }
    id = parent.id;
  }
  throw Error(ErrorCode::Corruption, "cyclic B+ tree parent chain");
}
void BPlusTree::insert_parent(PageId left_id, PageId right_id, IndexKey separator) {
  const auto left = read_node(left_id);
  if (left.parent == kInvalidPageId) {
    IndexNode root;
    root.type = PageType::IndexInternal;
    root.keys = {separator};
    root.children = {left_id, right_id};
    header_.root = allocate_node(root);
    set_parent(left_id, header_.root);
    set_parent(right_id, header_.root);
    ++stats_.root_splits;
    return;
  }
  auto parent = read_node(left.parent);
  const auto index = child_position(parent, left_id);
  parent.keys.insert(position(parent.keys, index), separator);
  parent.children.insert(position(parent.children, index + 1), right_id);
  if (index != 0) parent.keys[index - 1] = minimum(left_id);
  if (parent.keys.size() <= header_.max_keys) { write_node(parent); return; }

  const auto middle = parent.keys.size() / 2;
  const auto promoted = parent.keys[middle];
  IndexNode right;
  right.type = PageType::IndexInternal;
  right.parent = parent.parent;
  right.keys.assign(position(parent.keys, middle + 1), parent.keys.end());
  right.children.assign(position(parent.children, middle + 1), parent.children.end());
  const auto new_id = allocate_node(right);
  for (const auto child : right.children) set_parent(child, new_id);
  parent.keys.resize(middle);
  parent.children.resize(middle + 1);
  write_node(parent);
  ++stats_.internal_splits;
  insert_parent(parent.id, new_id, promoted);
}
bool BPlusTree::insert(IndexKey key, RecordId value) {
  std::unique_lock lock(mutex_);
  check_open();
  if (value.page == 0 || value.page == kInvalidPageId || value.generation == 0)
    throw Error(ErrorCode::InvalidArgument, "index value must be a valid record ID");
  IndexNode leaf;
  std::size_t index = 0;
  if (header_.root != kInvalidPageId) {
    leaf = find_leaf(key);
    index = static_cast<std::size_t>(std::lower_bound(leaf.keys.begin(), leaf.keys.end(), key) - leaf.keys.begin());
    if (index < leaf.keys.size() && leaf.keys[index] == key) return false;
  }
  try {
    leaf.keys.insert(position(leaf.keys, index), key);
    leaf.values.insert(position(leaf.values, index), value);
    if (header_.root == kInvalidPageId) {
      header_.root = allocate_node(leaf);
    } else if (leaf.keys.size() <= header_.max_keys) {
      write_node(leaf);
      refresh_ancestors(leaf.id);
    } else {
      const auto middle = leaf.keys.size() / 2;
      IndexNode right;
      right.parent = leaf.parent;
      right.next = leaf.next;
      right.keys.assign(position(leaf.keys, middle), leaf.keys.end());
      right.values.assign(position(leaf.values, middle), leaf.values.end());
      const auto right_id = allocate_node(right);
      leaf.next = right_id;
      leaf.keys.resize(middle);
      leaf.values.resize(middle);
      write_node(leaf);
      ++stats_.leaf_splits;
      insert_parent(leaf.id, right_id, right.keys.front());
      refresh_ancestors(leaf.id);
    }
    ++header_.size;
    write_header();
    return true;
  } catch (...) { failed_ = true; throw; }
}
std::size_t BPlusTree::min_keys(const IndexNode& node) const {
  return node.leaf() ? (header_.max_keys + 1U) / 2U : header_.max_keys / 2U;
}
void BPlusTree::rebalance(PageId id) {
  auto node = read_node(id);
  if (node.parent == kInvalidPageId) {
    if (!node.leaf() && node.keys.empty()) {
      header_.root = node.children.front();
      set_parent(header_.root, kInvalidPageId);
      pool_.delete_page(node.id);
      ++stats_.root_collapses;
    }
    return;
  }
  if (node.keys.size() >= min_keys(node)) return;
  auto parent = read_node(node.parent);
  const auto index = child_position(parent, id);
  if (index > 0) {
    auto left = read_node(parent.children[index - 1]);
    if (left.keys.size() > min_keys(left)) {
      if (node.leaf()) {
        node.keys.insert(node.keys.begin(), left.keys.back());
        node.values.insert(node.values.begin(), left.values.back());
        left.keys.pop_back(); left.values.pop_back();
        parent.keys[index - 1] = node.keys.front();
      } else {
        node.keys.insert(node.keys.begin(), parent.keys[index - 1]);
        node.children.insert(node.children.begin(), left.children.back());
        parent.keys[index - 1] = left.keys.back();
        set_parent(left.children.back(), node.id);
        left.keys.pop_back(); left.children.pop_back();
        ++stats_.internal_redistributions;
      }
      write_node(left); write_node(node); write_node(parent);
      ++stats_.borrowed_from_left;
      return;
    }
  }
  if (index + 1 < parent.children.size()) {
    auto right = read_node(parent.children[index + 1]);
    if (right.keys.size() > min_keys(right)) {
      if (node.leaf()) {
        node.keys.push_back(right.keys.front());
        node.values.push_back(right.values.front());
        right.keys.erase(right.keys.begin()); right.values.erase(right.values.begin());
        parent.keys[index] = right.keys.front();
      } else {
        node.keys.push_back(parent.keys[index]);
        node.children.push_back(right.children.front());
        parent.keys[index] = right.keys.front();
        set_parent(right.children.front(), node.id);
        right.keys.erase(right.keys.begin()); right.children.erase(right.children.begin());
        ++stats_.internal_redistributions;
      }
      write_node(node); write_node(right); write_node(parent);
      ++stats_.borrowed_from_right;
      return;
    }
  }

  // Merge into the left member of an adjacent pair, then repair the parent.
  const auto separator_index = index > 0 ? index - 1 : index;
  auto left = index > 0 ? read_node(parent.children[index - 1]) : node;
  auto right = index > 0 ? node : read_node(parent.children[index + 1]);
  if (left.leaf()) {
    left.keys.insert(left.keys.end(), right.keys.begin(), right.keys.end());
    left.values.insert(left.values.end(), right.values.begin(), right.values.end());
    left.next = right.next;
    ++stats_.leaf_merges;
  } else {
    left.keys.push_back(parent.keys[separator_index]);
    left.keys.insert(left.keys.end(), right.keys.begin(), right.keys.end());
    left.children.insert(left.children.end(), right.children.begin(), right.children.end());
    for (const auto child : right.children) set_parent(child, left.id);
    ++stats_.internal_merges;
  }
  parent.keys.erase(position(parent.keys, separator_index));
  parent.children.erase(position(parent.children, separator_index + 1));
  write_node(left); write_node(parent);
  pool_.delete_page(right.id);
  refresh_ancestors(parent.id);
  rebalance(parent.id);
}
bool BPlusTree::erase(IndexKey key) {
  std::unique_lock lock(mutex_);
  check_open();
  if (header_.root == kInvalidPageId) return false;
  auto leaf = find_leaf(key);
  const auto found = std::lower_bound(leaf.keys.begin(), leaf.keys.end(), key);
  if (found == leaf.keys.end() || *found != key) return false;
  const auto index = static_cast<std::size_t>(found - leaf.keys.begin());
  try {
    leaf.keys.erase(found);
    leaf.values.erase(position(leaf.values, index));
    if (leaf.id == header_.root && leaf.keys.empty()) {
      pool_.delete_page(leaf.id);
      header_.root = kInvalidPageId;
    } else {
      write_node(leaf);
      refresh_ancestors(leaf.id);  // Keep exact subtree-minimum separators before rotating.
      rebalance(leaf.id);
    }
    --header_.size;
    write_header();
    return true;
  } catch (...) { failed_ = true; throw; }
}
std::optional<RecordId> BPlusTree::find(IndexKey key) const {
  std::shared_lock lock(mutex_);
  check_open();
  if (header_.root == kInvalidPageId) return {};
  const auto leaf = find_leaf(key);
  const auto found = std::lower_bound(leaf.keys.begin(), leaf.keys.end(), key);
  if (found == leaf.keys.end() || *found != key) return {};
  return leaf.values[static_cast<std::size_t>(found - leaf.keys.begin())];
}
std::vector<BPlusTree::Entry> BPlusTree::scan(std::optional<IndexKey> lower, std::optional<IndexKey> upper,
                                           std::size_t limit) const {
  std::shared_lock lock(mutex_);
  check_open();
  if (lower && upper && *lower > *upper) throw Error(ErrorCode::InvalidArgument, "range lower bound exceeds upper bound");
  std::vector<Entry> result;
  if (limit == 0 || header_.root == kInvalidPageId || (lower && upper && *lower == *upper)) return result;
  auto leaf = find_leaf(lower.value_or(std::numeric_limits<IndexKey>::min()));
  std::unordered_set<PageId> visited;
  std::optional<IndexKey> previous;
  for (;;) {
    if (!leaf.leaf() || leaf.keys.empty() || !visited.insert(leaf.id).second ||
        (previous && *previous >= leaf.keys.front()))
      throw Error(ErrorCode::Corruption, "invalid or cyclic B+ tree leaf chain");
    for (std::size_t i = 0; i < leaf.keys.size(); ++i) {
      const auto key = leaf.keys[i];
      if (lower && key < *lower) continue;
      if (upper && key >= *upper) return result;
      result.emplace_back(key, leaf.values[i]);
      if (result.size() == limit) return result;
    }
    if (leaf.next == kInvalidPageId) return result;
    previous = leaf.keys.back();
    leaf = read_node(leaf.next);
  }
}
TreeValidation BPlusTree::validate_unlocked() const {
  {
    const auto guard = pool_.fetch_read(kIndexHeaderPage);
    const auto persisted = decode_index_header(guard.page());
    if (persisted.root != header_.root || persisted.size != header_.size || persisted.max_keys != header_.max_keys)
      throw Error(ErrorCode::Corruption, "B+ tree in-memory and page metadata disagree");
  }
  TreeValidation result;
  std::unordered_set<PageId> visited;
  std::vector<std::pair<PageId, PageId>> leaves;
  struct Bounds { IndexKey min, max; };
  std::function<Bounds(PageId, PageId, std::size_t)> visit = [&](PageId id, PageId parent, std::size_t depth) -> Bounds {
    if (depth > kMaxHeight || !visited.insert(id).second)
      throw Error(ErrorCode::Corruption, "B+ tree has a cycle or shared child");
    const auto node = read_node(id);
    if (node.parent != parent || node.keys.empty() || (parent != kInvalidPageId && node.keys.size() < min_keys(node)))
      throw Error(ErrorCode::Corruption, "B+ tree parent or occupancy invariant violated");
    ++result.nodes;
    if (node.leaf()) {
      if (result.height != 0 && result.height != depth) throw Error(ErrorCode::Corruption, "B+ tree leaves have unequal depths");
      result.height = depth;
      ++result.leaves;
      result.entries += node.keys.size();
      leaves.emplace_back(node.id, node.next);
      return {node.keys.front(), node.keys.back()};
    }
    auto bounds = visit(node.children.front(), id, depth + 1);
    for (std::size_t i = 1; i < node.children.size(); ++i) {
      const auto child = visit(node.children[i], id, depth + 1);
      if (bounds.max >= child.min || node.keys[i - 1] != child.min)
        throw Error(ErrorCode::Corruption, "B+ tree separator or child range invariant violated");
      bounds.max = child.max;
    }
    return bounds;
  };
  if (header_.root != kInvalidPageId) (void)visit(header_.root, kInvalidPageId, 1);
  if (result.entries != header_.size) throw Error(ErrorCode::Corruption, "B+ tree entry count disagrees with header");
  for (std::size_t i = 0; i < leaves.size(); ++i) {
    const auto next = i + 1 == leaves.size() ? kInvalidPageId : leaves[i + 1].first;
    if (leaves[i].second != next) throw Error(ErrorCode::Corruption, "B+ tree leaf chain disagrees with tree order");
  }
  const auto allocated = pool_.allocated_pages();
  if (allocated.size() != visited.size() + 1)
    throw Error(ErrorCode::Corruption, "B+ tree has orphan or unexpected allocated pages");
  for (const auto id : allocated)
    if (id != kIndexHeaderPage && !visited.contains(id)) throw Error(ErrorCode::Corruption, "unreachable index page");
  return result;
}
TreeValidation BPlusTree::validate() const {
  std::shared_lock lock(mutex_);
  check_open();
  return validate_unlocked();
}
std::uint64_t BPlusTree::size() const { std::shared_lock lock(mutex_); check_open(); return header_.size; }
TreeStats BPlusTree::stats() const { std::shared_lock lock(mutex_); return stats_; }
BufferPoolStats BPlusTree::buffer_stats() const { std::shared_lock lock(mutex_); return pool_.stats(); }
void BPlusTree::sync() {
  std::unique_lock lock(mutex_);
  check_open();
  try { pool_.sync(); } catch (...) { failed_ = true; throw; }
}
void BPlusTree::close() {
  std::unique_lock lock(mutex_);
  if (closed_) return;
  check_open();
  try { pool_.close(); disk_.close(); closed_ = true; }
  catch (...) { failed_ = true; throw; }
}
void BPlusTree::begin_undo() {
  std::unique_lock lock(mutex_);
  check_open();
  pool_.sync();
  disk_.begin_undo();
}
void BPlusTree::commit_undo() { std::unique_lock lock(mutex_); disk_.commit_undo(); }
void BPlusTree::rollback_undo() {
  std::unique_lock lock(mutex_);
  disk_.rollback_undo();
  pool_.discard_all();
  {
    const auto guard = pool_.fetch_read(kIndexHeaderPage);
    header_ = decode_index_header(guard.page());
  }
  (void)validate_unlocked();
  failed_ = false;
}
void BPlusTree::attach_wal(std::shared_ptr<Wal> wal) {
  std::unique_lock lock(mutex_); pool_.sync();
  disk_.attach_wal(wal, kIndexFileId); pool_.attach_wal(std::move(wal), kIndexFileId);
}
}  // namespace tinydb
