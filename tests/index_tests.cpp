#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <csignal>
#include <filesystem>
#include <fstream>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <numeric>
#include <random>
#include <sys/resource.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

#include "common/encoding.h"
#include "index/b_plus_tree.h"
#include "storage/heap_file.h"
#include "storage/record.h"

namespace tinydb {
namespace {
RecordId value_for(IndexKey key) {
  // Values are opaque to the index; record referential integrity belongs to the table layer.
  return {static_cast<PageId>(static_cast<std::uint64_t>(key) % 10000 + 1), 7,
          static_cast<std::uint64_t>(key) % 100000 + 1};
}
void expect_error(ErrorCode code, const std::function<void()>& operation) {
  try { operation(); ADD_FAILURE() << "expected database error"; }
  catch (const Error& error) { EXPECT_EQ(error.code(), code) << error.what(); }
}
class IndexTest : public ::testing::Test {
 protected:
  void SetUp() override {
    auto pattern = (std::filesystem::temp_directory_path() / "tinydb-index-XXXXXX").string();
    const auto created = ::mkdtemp(pattern.data());
    ASSERT_NE(created, nullptr);
    directory_ = created;
    path_ = directory_ / "test.idx";
  }
  void TearDown() override { std::filesystem::remove_all(directory_); }
  std::filesystem::path directory_, path_;
};
void check_model(BPlusTree& tree, const std::map<IndexKey, RecordId>& model) {
  EXPECT_EQ(tree.size(), model.size());
  const auto validation = tree.validate();
  EXPECT_EQ(validation.entries, model.size());
  EXPECT_EQ(validation.height == 0, model.empty());
  EXPECT_EQ(tree.scan(), (std::vector<BPlusTree::Entry>(model.begin(), model.end())));
  EXPECT_EQ(tree.buffer_stats().pins, 0U);
}

TEST(IndexPageTest, FullLeafAndInternalPagesRoundTrip) {
  IndexNode leaf;
  leaf.id = 2;
  leaf.parent = 4;
  leaf.next = 3;
  for (int i = 0; i < kIndexMaxKeys; ++i) {
    leaf.keys.push_back(i - 90);
    leaf.values.push_back(value_for(i - 90));
  }
  auto encoded = encode_index_node(leaf, kIndexMaxKeys);
  const auto decoded = decode_index_node(encoded, kIndexMaxKeys);
  EXPECT_EQ(decoded.keys, leaf.keys);
  EXPECT_EQ(decoded.values, leaf.values);
  EXPECT_EQ(decoded.parent, 4U);
  EXPECT_EQ(decoded.next, 3U);
  EXPECT_EQ(encoded.bytes[24], std::byte{184});
  IndexNode internal;
  internal.id = 1000;
  internal.type = PageType::IndexInternal;
  internal.keys = leaf.keys;
  for (PageId i = 2; i < 2 + kIndexMaxKeys + 1U; ++i) internal.children.push_back(i);
  const auto roundtrip = decode_index_node(encode_index_node(internal, kIndexMaxKeys), kIndexMaxKeys);
  EXPECT_EQ(roundtrip.children, internal.children);
  EXPECT_EQ(roundtrip.keys, internal.keys);
}

TEST(IndexPageTest, SignedExtremesAndHeaderRoundTrip) {
  IndexNode node;
  node.id = 2;
  node.keys = {std::numeric_limits<IndexKey>::min(), 0, std::numeric_limits<IndexKey>::max()};
  for (const auto key : node.keys) node.values.push_back(value_for(key));
  const auto decoded = decode_index_node(encode_index_node(node, 3), 3);
  EXPECT_EQ(decoded.keys, node.keys);
  const IndexHeader header{2, 3, 1234};
  const auto decoded_header = decode_index_header(encode_index_header(header));
  EXPECT_EQ(decoded_header.root, 2U);
  EXPECT_EQ(decoded_header.max_keys, 3U);
  EXPECT_EQ(decoded_header.size, 1234U);
  EXPECT_EQ(decode_index_header(encode_index_header({})).root, kInvalidPageId);
}

TEST(IndexPageTest, RejectsMalformedCountOrderingReferencesAndReservedFields) {
  IndexNode leaf;
  leaf.id = 2;
  leaf.keys = {1, 2};
  leaf.values = {value_for(1), value_for(2)};
  const auto good = encode_index_node(leaf, 3);
  auto check = [&](auto edit) {
    auto damaged = good;
    edit(damaged);
    damaged.seal();
    expect_error(ErrorCode::Corruption, [&] { (void)decode_index_node(damaged, 3); });
  };
  check([](Page& p) { encoding::store<std::uint16_t>(p.bytes, 24, 65535); });
  check([](Page& p) { encoding::store<std::uint16_t>(p.bytes, 26, 1); });
  check([](Page& p) { encoding::store<PageId>(p.bytes, 28, p.id()); });
  check([](Page& p) { encoding::store<PageId>(p.bytes, 32, p.id()); });
  check([](Page& p) { encoding::store<std::uint32_t>(p.bytes, 36, 1); });
  check([](Page& p) { encoding::store<std::uint64_t>(p.bytes, 62, 1); });  // Duplicate key.
  check([](Page& p) { encoding::store<PageId>(p.bytes, 48, 0); });
  check([](Page& p) { encoding::store<std::uint64_t>(p.bytes, 54, 0); });
  expect_error(ErrorCode::Corruption, [&] { (void)decode_index_node(good, 2); });
  expect_error(ErrorCode::InvalidArgument, [&] { (void)encode_index_header({2, 3, 0}); });
  expect_error(ErrorCode::InvalidArgument, [&] { (void)encode_index_header({kInvalidPageId, 3, 1}); });
  leaf.keys.push_back(0); leaf.values.push_back(value_for(0));
  expect_error(ErrorCode::InvalidArgument, [&] { (void)encode_index_node(leaf, 3); });
}

TEST_F(IndexTest, TypedAllocationPersistsAndCannotChangeType) {
  {
    DiskManager disk(path_);
    expect_error(ErrorCode::InvalidArgument, [&] { (void)disk.allocate_page(PageType::Free); });
    expect_error(ErrorCode::InvalidArgument, [&] { (void)disk.allocate_page(PageType::Metadata); });
    EXPECT_EQ(disk.page_count(), 1U);
    const auto id = disk.allocate_page(PageType::IndexLeaf);
    EXPECT_EQ(disk.read_page(id).type(), PageType::IndexLeaf);
    expect_error(ErrorCode::InvalidArgument, [&] { disk.write_page(Page::create(id, PageType::Heap)); });
    disk.close();
  }
  DiskManager disk(path_);
  BufferPool pool(disk, 1);
  expect_error(ErrorCode::InvalidArgument, [&] { (void)pool.new_page(PageType::Free); });
  auto guard = pool.fetch_write(1);
  expect_error(ErrorCode::InvalidArgument, [&] { guard.replace(Page::create(1, PageType::IndexInternal)); });
  guard.drop();
  pool.delete_page(1);
  auto reused = pool.new_page(PageType::IndexInternal);
  EXPECT_EQ(reused.page().id(), 1U);
  EXPECT_EQ(reused.page().type(), PageType::IndexInternal);
}

TEST_F(IndexTest, EmptyTreeDuplicatesMissingKeysAndInvalidValues) {
  BPlusTree tree(path_, {1, 3});
  EXPECT_EQ(tree.validate().nodes, 0U);
  EXPECT_TRUE(tree.scan().empty());
  EXPECT_FALSE(tree.find(0));
  EXPECT_FALSE(tree.erase(0));
  for (const auto invalid : {RecordId{}, RecordId{0, 0, 1}, RecordId{1, 0, 0}})
    expect_error(ErrorCode::InvalidArgument, [&] { (void)tree.insert(1, invalid); });
  ASSERT_TRUE(tree.insert(1, value_for(1)));
  EXPECT_FALSE(tree.insert(1, value_for(99)));
  EXPECT_EQ(tree.find(1), value_for(1));
  EXPECT_EQ(tree.size(), 1U);
  EXPECT_EQ(tree.validate().entries, 1U);
  EXPECT_FALSE(tree.erase(2));
  EXPECT_TRUE(tree.erase(1));
  EXPECT_EQ(tree.validate().height, 0U);
  EXPECT_EQ(tree.size(), 0U);
  EXPECT_TRUE(tree.insert(2, value_for(2)));
  EXPECT_EQ(tree.find(2), value_for(2));
  tree.close();
}

TEST_F(IndexTest, IndexPagesUseWalBeforePageFlushOrdering) {
  DiskManager disk(path_);
  (void)disk.allocate_page(PageType::IndexHeader);
  const auto id = disk.allocate_page(PageType::IndexLeaf);
  std::vector<Lsn> forced;
  BufferPool pool(disk, 1, [&](Lsn lsn) {
    EXPECT_EQ(encoding::load<Lsn>(disk.read_page(id).bytes, 16), 0U);
    forced.push_back(lsn);
  });
  IndexNode node;
  node.id = id;
  node.keys = {1}; node.values = {value_for(1)};
  {
    auto guard = pool.fetch_write(id);
    guard.replace(encode_index_node(node, 3), 42);
  }
  pool.flush_all();
  EXPECT_EQ(forced, (std::vector<Lsn>{42}));
  const auto page = disk.read_page(id);
  EXPECT_EQ(encoding::load<Lsn>(page.bytes, 16), 42U);
  EXPECT_EQ(decode_index_node(page, 3).values, node.values);
}

TEST_F(IndexTest, AscendingDescendingAndRandomInsertionValidateEveryStep) {
  for (const int order : {0, 1, 2}) {
    const auto file = directory_ / ("order" + std::to_string(order) + ".idx");
    BPlusTree tree(file, {1, 3});
    std::vector<IndexKey> keys(140);
    std::iota(keys.begin(), keys.end(), -70);
    if (order == 1) std::reverse(keys.begin(), keys.end());
    if (order == 2) { std::mt19937 rng(12345); std::shuffle(keys.begin(), keys.end(), rng); }
    std::map<IndexKey, RecordId> model;
    for (const auto key : keys) {
      ASSERT_TRUE(tree.insert(key, value_for(key)));
      model.emplace(key, value_for(key));
      check_model(tree, model);
      EXPECT_EQ(tree.find(key), value_for(key));
    }
    EXPECT_GE(tree.validate().height, 4U);
    EXPECT_GT(tree.stats().leaf_splits, 0U);
    EXPECT_GT(tree.stats().internal_splits, 0U);
    tree.close();
  }
}

TEST_F(IndexTest, LeafRedistributionFromBothSides) {
  for (const bool from_left : {true, false}) {
    BPlusTree tree(directory_ / (from_left ? "left.idx" : "right.idx"), {1, 3});
    std::map<IndexKey, RecordId> model;
    for (IndexKey key = 1; key <= 4; ++key) {
      ASSERT_TRUE(tree.insert(key, value_for(key))); model.emplace(key, value_for(key));
      check_model(tree, model);
    }
    const IndexKey added = from_left ? 0 : 5, removed = from_left ? 3 : 1;
    ASSERT_TRUE(tree.insert(added, value_for(added))); model.emplace(added, value_for(added));
    check_model(tree, model);
    ASSERT_TRUE(tree.erase(removed)); model.erase(removed);
    check_model(tree, model);
    if (from_left) EXPECT_EQ(tree.stats().borrowed_from_left, 1U);
    else EXPECT_EQ(tree.stats().borrowed_from_right, 1U);
    EXPECT_EQ(tree.stats().leaf_merges, 0U);
    tree.close();
  }
}

TEST_F(IndexTest, LeafMergeFromEitherSideCollapsesRoot) {
  for (const IndexKey removed : {1, 4}) {
    BPlusTree tree(directory_ / ("merge" + std::to_string(removed)), {1, 3});
    for (IndexKey key = 1; key <= 4; ++key) ASSERT_TRUE(tree.insert(key, value_for(key)));
    ASSERT_EQ(tree.validate().height, 2U);
    ASSERT_TRUE(tree.erase(removed));
    EXPECT_EQ(tree.validate().height, 1U);
    EXPECT_EQ(tree.validate().leaves, 1U);
    EXPECT_EQ(tree.stats().leaf_merges, 1U);
    EXPECT_EQ(tree.stats().root_collapses, 1U);
    EXPECT_FALSE(tree.find(removed));
    tree.close();
  }
}

TEST_F(IndexTest, InternalRebalancingAndCompleteDeletionCheckEveryInvariant) {
  BPlusTree tree(path_, {2, 3});
  std::vector<IndexKey> keys(240);
  std::iota(keys.begin(), keys.end(), 0);
  std::mt19937 rng(0xBEEFU);
  std::shuffle(keys.begin(), keys.end(), rng);
  std::map<IndexKey, RecordId> model;
  for (const auto key : keys) {
    ASSERT_TRUE(tree.insert(key, value_for(key))); model.emplace(key, value_for(key));
    check_model(tree, model);
  }
  ASSERT_GE(tree.validate().height, 4U);
  std::shuffle(keys.begin(), keys.end(), rng);
  for (const auto key : keys) {
    ASSERT_TRUE(tree.erase(key)); model.erase(key);
    check_model(tree, model);
  }
  const auto stats = tree.stats();
  EXPECT_GT(stats.internal_merges, 0U);
  EXPECT_GT(stats.internal_redistributions, 0U);
  EXPECT_GT(stats.borrowed_from_left, 0U);
  EXPECT_GT(stats.borrowed_from_right, 0U);
  EXPECT_GT(stats.root_collapses, 1U);
  tree.close();
  BPlusTree reopened(path_, {1, 0});
  EXPECT_EQ(reopened.validate().nodes, 0U);
}

TEST_F(IndexTest, RandomCrudAgainstMapForOddAndEvenCapacities) {
  for (const std::uint16_t capacity : {3, 4, 5, 8}) {
    const auto path = directory_ / ("random" + std::to_string(capacity));
    auto tree = std::make_unique<BPlusTree>(path, BPlusTreeOptions{1, capacity});
    std::map<IndexKey, RecordId> model;
    std::mt19937 rng(20261019 + capacity);
    for (int step = 0; step < 400; ++step) {
      const IndexKey key = static_cast<IndexKey>(rng() % 100) - 50;
      switch (rng() % 3) {
        case 0: EXPECT_EQ(tree->insert(key, value_for(key)), model.emplace(key, value_for(key)).second); break;
        case 1: EXPECT_EQ(tree->erase(key), model.erase(key) != 0); break;
        default: {
          const auto found = model.find(key);
          const auto expected = found == model.end() ? std::optional<RecordId>{} : std::optional<RecordId>{found->second};
          EXPECT_EQ(tree->find(key), expected);
        }
      }
      check_model(*tree, model);
      if (step % 50 == 49) {
        tree->close();
        tree = std::make_unique<BPlusTree>(path, BPlusTreeOptions{1, 0});
        check_model(*tree, model);
      }
    }
    tree->close();
  }
}

TEST_F(IndexTest, DefaultCapacitySplitsFullPhysicalLeafPages) {
  BPlusTree tree(path_, {2, 0});
  std::vector<IndexKey> keys(600);
  std::iota(keys.begin(), keys.end(), -300);
  std::mt19937 rng(12); std::shuffle(keys.begin(), keys.end(), rng);
  std::map<IndexKey, RecordId> model;
  for (const auto key : keys) {
    ASSERT_TRUE(tree.insert(key, value_for(key))); model.emplace(key, value_for(key));
    check_model(tree, model);
  }
  EXPECT_GT(tree.stats().leaf_splits, 0U);
  EXPECT_EQ(tree.validate().height, 2U);
  std::shuffle(keys.begin(), keys.end(), rng);
  for (const auto key : keys) {
    ASSERT_TRUE(tree.erase(key)); model.erase(key);
    check_model(tree, model);
  }
  tree.close();
}

TEST_F(IndexTest, RangeScansCrossLeavesWithLimitsAndSignedExtremes) {
  BPlusTree tree(path_, {1, 4});
  std::map<IndexKey, RecordId> model;
  for (IndexKey key = -100; key <= 100; key += 2) model.emplace(key, value_for(key));
  model.emplace(std::numeric_limits<IndexKey>::min(), value_for(std::numeric_limits<IndexKey>::min()));
  model.emplace(std::numeric_limits<IndexKey>::max(), value_for(std::numeric_limits<IndexKey>::max()));
  for (const auto& [key, rid] : model) ASSERT_TRUE(tree.insert(key, rid));
  check_model(tree, model);
  for (IndexKey lower = -110; lower <= 110; lower += 11) {
    for (const std::size_t limit : {0U, 1U, 7U, 1000U}) {
      const auto upper = lower + 39;
      std::vector<BPlusTree::Entry> expected;
      for (auto it = model.lower_bound(lower); it != model.end() && it->first < upper && expected.size() < limit; ++it)
        expected.push_back(*it);
      EXPECT_EQ(tree.range(lower, upper, limit), expected);
    }
  }
  EXPECT_TRUE(tree.range(5, 5).empty());
  expect_error(ErrorCode::InvalidArgument, [&] { (void)tree.range(10, 9); });
  EXPECT_EQ(tree.scan(std::numeric_limits<IndexKey>::max()).size(), 1U);
  EXPECT_EQ(tree.find(std::numeric_limits<IndexKey>::min()), value_for(std::numeric_limits<IndexKey>::min()));
  EXPECT_EQ(tree.find(std::numeric_limits<IndexKey>::max()), value_for(std::numeric_limits<IndexKey>::max()));
  tree.close();
}

TEST_F(IndexTest, ReusesFreedPagesAfterRestartWithoutFileGrowth) {
  std::uintmax_t original_size;
  {
    BPlusTree tree(path_, {1, 3});
    for (IndexKey i = 0; i < 100; ++i) ASSERT_TRUE(tree.insert(i, value_for(i)));
    tree.sync(); original_size = std::filesystem::file_size(path_);
    for (IndexKey i = 0; i < 100; ++i) ASSERT_TRUE(tree.erase(i));
    EXPECT_EQ(tree.validate().nodes, 0U);
    tree.close();
  }
  BPlusTree reopened(path_, {1, 0});
  for (IndexKey i = 0; i < 100; ++i) ASSERT_TRUE(reopened.insert(i, value_for(i)));
  EXPECT_EQ(reopened.validate().entries, 100U);
  reopened.close();
  EXPECT_EQ(std::filesystem::file_size(path_), original_size);
}

TEST_F(IndexTest, OptionsPersistAndInvalidOptionsDoNotCreateFiles) {
  for (const auto options : {BPlusTreeOptions{0, 3}, BPlusTreeOptions{1, 2}, BPlusTreeOptions{1, 185}}) {
    expect_error(ErrorCode::InvalidArgument, [&] { BPlusTree tree(path_, options); });
    EXPECT_FALSE(std::filesystem::exists(path_));
  }
  { BPlusTree tree(path_, {1, 4}); tree.close(); }
  expect_error(ErrorCode::InvalidArgument, [&] { BPlusTree tree(path_, {1, 3}); });
  BPlusTree tree(path_, {1, 0});
  EXPECT_EQ(tree.validate().entries, 0U);
}

TEST_F(IndexTest, ClosedTreeRejectsOperationsAndFileKindsCannotBeMixed) {
  {
    BPlusTree tree(path_);
    tree.close();
    EXPECT_NO_THROW(tree.close());
    expect_error(ErrorCode::Closed, [&] { (void)tree.insert(1, value_for(1)); });
    expect_error(ErrorCode::Closed, [&] { (void)tree.erase(1); });
    expect_error(ErrorCode::Closed, [&] { (void)tree.find(1); });
    expect_error(ErrorCode::Closed, [&] { (void)tree.scan(); });
    expect_error(ErrorCode::Closed, [&] { (void)tree.validate(); });
    expect_error(ErrorCode::Closed, [&] { (void)tree.size(); });
    expect_error(ErrorCode::Closed, [&] { tree.sync(); });
  }
  expect_error(ErrorCode::Corruption, [&] { HeapFile heap(path_); });
  const auto heap_path = directory_ / "heap.db";
  {
    HeapFile heap(heap_path);
    (void)heap.insert(serialize({1, "Alice", 1})); heap.close();
  }
  expect_error(ErrorCode::Corruption, [&] { BPlusTree tree(heap_path); });
}

void edit_page(const std::filesystem::path& path, PageId id, const std::function<void(Page&)>& edit) {
  std::fstream file(path, std::ios::in | std::ios::out | std::ios::binary);
  ASSERT_TRUE(file.good());
  Page page;
  file.seekg(static_cast<std::streamoff>(id) * kPageSize);
  file.read(reinterpret_cast<char*>(page.bytes.data()), kPageSize);
  ASSERT_TRUE(file.good());
  edit(page); page.seal();
  file.seekp(static_cast<std::streamoff>(id) * kPageSize);
  file.write(reinterpret_cast<const char*>(page.bytes.data()), kPageSize);
  ASSERT_TRUE(file.good());
}

TEST_F(IndexTest, StartupRejectsStructuralCorruptionWithValidChecksums) {
  { BPlusTree tree(path_, {2, 3}); for (IndexKey i = 0; i < 12; ++i) ASSERT_TRUE(tree.insert(i, value_for(i))); tree.close(); }
  PageId root, leaf;
  {
    DiskManager disk(path_);
    root = decode_index_header(disk.read_page(1)).root;
    auto node = decode_index_node(disk.read_page(root), 3);
    while (!node.leaf()) node = decode_index_node(disk.read_page(node.children.front()), 3);
    leaf = node.id;
    disk.close();
  }
  std::vector<std::pair<PageId, std::function<void(Page&)>>> changes{
    {1, [](Page& p) { encoding::store<std::uint64_t>(p.bytes, 32, 999); }},
    {root, [](Page& p) { encoding::store<PageId>(p.bytes, 40, kInvalidPageId); }},
    {root, [](Page& p) { encoding::store<PageId>(p.bytes, 52, encoding::load<PageId>(p.bytes, 40)); }},
    {root, [](Page& p) { encoding::store<std::uint64_t>(p.bytes, 44, 7777); }},
    {leaf, [](Page& p) { encoding::store<PageId>(p.bytes, 28, kInvalidPageId); }},
    {leaf, [](Page& p) { encoding::store<PageId>(p.bytes, 32, kInvalidPageId); }},
    {leaf, [](Page& p) { encoding::store<std::uint16_t>(p.bytes, 24, 1); }}
  };
  for (std::size_t i = 0; i < changes.size(); ++i) {
    const auto damaged = directory_ / ("damaged" + std::to_string(i));
    std::filesystem::copy_file(path_, damaged);
    edit_page(damaged, changes[i].first, changes[i].second);
    expect_error(ErrorCode::Corruption, [&] { BPlusTree tree(damaged); });
  }
  const auto orphan = directory_ / "orphan";
  std::filesystem::copy_file(path_, orphan);
  { DiskManager disk(orphan); (void)disk.allocate_page(PageType::IndexLeaf); disk.close(); }
  expect_error(ErrorCode::Corruption, [&] { BPlusTree tree(orphan); });
}

TEST_F(IndexTest, IndexLookupFindsActualHeapRecordsAcrossRestarts) {
  const auto heap_path = directory_ / "accounts.db";
  {
    HeapFile heap(heap_path, 1);
    BPlusTree tree(path_, {1, 4});
    for (IndexKey key = 40; key >= -40; --key) {
      const auto rid = heap.insert(serialize({key, "account", key * 100}));
      ASSERT_TRUE(tree.insert(key, rid));
    }
    heap.close(); tree.close();
  }
  HeapFile heap(heap_path, 1);
  BPlusTree tree(path_, {1, 0});
  for (IndexKey key = -40; key <= 40; ++key) {
    const auto found = tree.find(key);
    ASSERT_TRUE(found);
    EXPECT_EQ(deserialize(heap.get(*found)), (Record{key, "account", key * 100}));
  }
  for (const auto& [key, rid] : tree.range(-10, 10)) EXPECT_EQ(deserialize(heap.get(rid)).key, key);
  const auto rid = *tree.find(0);
  heap.erase(rid); ASSERT_TRUE(tree.erase(0));
  EXPECT_FALSE(tree.find(0));
  EXPECT_EQ(tree.validate().entries, 80U);
  heap.close(); tree.close();
}

TEST_F(IndexTest, ConcurrentReadersWritersAndScansRemainConsistent) {
  BPlusTree tree(path_, {4, 4});
  std::atomic<bool> okay{true};
  std::vector<std::thread> clients;
  for (IndexKey client = 0; client < 4; ++client) clients.emplace_back([&, client] {
    try {
      for (IndexKey i = 0; i < 50; ++i) {
        const auto key = client * 1000 + i;
        if (!tree.insert(key, value_for(key)) || tree.find(key) != value_for(key)) okay = false;
        if (i % 2 == 0 && !tree.erase(key)) okay = false;
        const auto rows = tree.scan({}, {}, 10);
        if (!std::is_sorted(rows.begin(), rows.end(), [](const auto& a, const auto& b) { return a.first < b.first; })) okay = false;
      }
    } catch (...) { okay = false; }
  });
  for (auto& client : clients) client.join();
  EXPECT_TRUE(okay);
  EXPECT_EQ(tree.validate().entries, 100U);
  for (IndexKey client = 0; client < 4; ++client)
    for (IndexKey i = 0; i < 50; ++i) EXPECT_EQ(tree.find(client * 1000 + i).has_value(), i % 2 != 0);
  tree.close();
}

int wait_child(pid_t child) {
  int status = 0;
  pid_t result;
  do { result = ::waitpid(child, &status, 0); } while (result < 0 && errno == EINTR);
  return result == child && WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}
TEST_F(IndexTest, SyncedRootAndSplitsSurviveChildExitWithoutDestructors) {
  const auto child = ::fork(); ASSERT_GE(child, 0);
  if (child == 0) {
    try {
      BPlusTree tree(path_, {1, 3});
      for (IndexKey key = 0; key < 100; ++key) if (!tree.insert(key, value_for(key))) ::_exit(11);
      for (IndexKey key = 0; key < 20; ++key) if (!tree.erase(key)) ::_exit(12);
      tree.sync(); ::_exit(0);
    } catch (...) { ::_exit(13); }
  }
  ASSERT_EQ(wait_child(child), 0);
  BPlusTree tree(path_, {1, 0});
  EXPECT_EQ(tree.validate().entries, 80U);
  for (IndexKey key = 0; key < 100; ++key) EXPECT_EQ(tree.find(key).has_value(), key >= 20);
}

TEST_F(IndexTest, RealWriteFailurePoisonsIndexInsteadOfAcknowledgingMutation) {
  const auto child = ::fork(); ASSERT_GE(child, 0);
  if (child == 0) {
    try {
      BPlusTree tree(path_, {1, 3});
      ::signal(SIGXFSZ, SIG_IGN);
      struct rlimit limit {};
      if (::getrlimit(RLIMIT_FSIZE, &limit) != 0) ::_exit(11);
      limit.rlim_cur = 2 * kPageSize;
      if (::setrlimit(RLIMIT_FSIZE, &limit) != 0) ::_exit(12);
      try { (void)tree.insert(1, value_for(1)); ::_exit(13); }
      catch (const Error& e) { if (e.code() != ErrorCode::Io) ::_exit(14); }
      try { (void)tree.find(1); ::_exit(15); }
      catch (const Error& e) { if (e.code() != ErrorCode::Io) ::_exit(16); }
      ::_exit(0);
    } catch (...) { ::_exit(17); }
  }
  ASSERT_EQ(wait_child(child), 0);
  BPlusTree tree(path_);
  EXPECT_EQ(tree.validate().entries, 0U);
}
}  // namespace
}  // namespace tinydb
