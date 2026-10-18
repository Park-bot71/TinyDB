#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <csignal>
#include <filesystem>
#include <fstream>
#include <functional>
#include <limits>
#include <memory>
#include <random>
#include <sys/resource.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

#include "common/encoding.h"
#include "storage/heap_file.h"
#include "storage/record.h"

namespace tinydb {
namespace {
Bytes bytes(std::size_t size, unsigned char value = 42) {
  return Bytes(size, static_cast<std::byte>(value));
}
void expect_error(ErrorCode expected, const std::function<void()>& operation) {
  try {
    operation();
    ADD_FAILURE() << "expected database error";
  } catch (const Error& error) {
    EXPECT_EQ(error.code(), expected) << error.what();
  }
}

class StorageTest : public ::testing::Test {
 protected:
  void SetUp() override {
    std::string pattern = (std::filesystem::temp_directory_path() / "tinydb-tests-XXXXXX").string();
    char* created = ::mkdtemp(pattern.data());
    ASSERT_NE(created, nullptr);
    directory_ = created;
    path_ = directory_ / "test.db";
  }
  void TearDown() override { std::filesystem::remove_all(directory_); }
  std::filesystem::path directory_, path_;
};

TEST(PageTest, HeaderRoundTripAndEveryByteCoveredByChecksum) {
  const Page page = Page::create(7, PageType::Heap);
  EXPECT_EQ(page.id(), 7U);
  EXPECT_EQ(page.type(), PageType::Heap);
  EXPECT_NO_THROW(page.validate(7));
  expect_error(ErrorCode::Corruption, [&] { page.validate(8); });
  for (std::size_t i = 0; i < kPageSize; ++i) {
    Page damaged = page;
    damaged.bytes[i] ^= std::byte{1};
    expect_error(ErrorCode::Corruption, [&] { damaged.validate(7); });
  }
}

TEST(RecordTest, PortableRoundTripWithSignedExtremesAndEmbeddedNull) {
  const Record record{std::numeric_limits<std::int64_t>::min(), std::string("A\0B", 3),
                      std::numeric_limits<std::int64_t>::max()};
  const auto encoded = serialize(record);
  EXPECT_EQ(encoded.size(), 23U);
  EXPECT_EQ(encoded[0], std::byte{1});
  EXPECT_EQ(encoded[11], std::byte{0x80});
  EXPECT_EQ(deserialize(encoded), record);
  EXPECT_EQ(deserialize(serialize({0, "", -1})), (Record{0, "", -1}));
}

TEST(RecordTest, RejectsInvalidLengthVersionAndOversizedNames) {
  auto encoded = serialize({1, "Alice", 100});
  for (std::size_t i = 0; i < encoded.size(); ++i)
    expect_error(ErrorCode::Corruption, [&] { (void)deserialize(std::span(encoded).first(i)); });
  auto extra = encoded;
  extra.push_back(std::byte{0});
  expect_error(ErrorCode::Corruption, [&] { (void)deserialize(extra); });
  encoded[0] = std::byte{2};
  expect_error(ErrorCode::Corruption, [&] { (void)deserialize(encoded); });
  expect_error(ErrorCode::InvalidArgument, [&] { (void)serialize({1, std::string(4033, 'x'), 1}); });
  EXPECT_EQ(serialize({1, std::string(4032, 'x'), 1}).size(), SlottedPage::kMaxRecordSize);
}

TEST(SlottedPageTest, ExactCapacityAndOversizedRecords) {
  SlottedPage page(Page::create(1, PageType::Heap));
  EXPECT_EQ(page.free_space(), 4064U);
  const auto rid = page.insert(bytes(SlottedPage::kMaxRecordSize), 1);
  EXPECT_EQ(page.free_space(), 0U);
  EXPECT_EQ(page.get(rid), bytes(4052));
  expect_error(ErrorCode::PageFull, [&] { (void)page.insert(bytes(1), 2); });
  expect_error(ErrorCode::InvalidArgument, [&] { (void)page.insert(bytes(0), 2); });
  expect_error(ErrorCode::InvalidArgument, [&] { (void)page.insert(bytes(4053), 2); });
  page.erase(rid);
  EXPECT_TRUE(page.can_insert(4052));
  EXPECT_EQ(page.insert(bytes(4052, 9), 3).slot, rid.slot);
  page.validate();
}

TEST(SlottedPageTest, CompactionAndVariableSizeUpdateKeepIdsStable) {
  SlottedPage page(Page::create(9, PageType::Heap));
  const auto a = page.insert(bytes(100, 1), 1);
  const auto b = page.insert(bytes(1000, 2), 2);
  const auto c = page.insert(bytes(100, 3), 3);
  page.erase(b);
  page.update(a, bytes(2000, 4));
  EXPECT_EQ(page.get(c), bytes(100, 3));
  EXPECT_EQ(page.get(a), bytes(2000, 4));
  page.update(a, bytes(2, 5));
  EXPECT_EQ(page.get(a), bytes(2, 5));
  const auto d = page.insert(bytes(3000, 6), 4);
  EXPECT_EQ(d.slot, b.slot);
  expect_error(ErrorCode::NotFound, [&] { (void)page.get(b); });
  page.validate();
}

TEST(SlottedPageTest, FailedUpdateDoesNotChangePage) {
  SlottedPage page(Page::create(1, PageType::Heap));
  const auto a = page.insert(bytes(2000, 1), 1);
  (void)page.insert(bytes(2000, 2), 2);
  const Page before = page.page();
  expect_error(ErrorCode::PageFull, [&] { page.update(a, bytes(3000)); });
  EXPECT_EQ(page.page().bytes, before.bytes);
  expect_error(ErrorCode::InvalidArgument, [&] { page.update(a, bytes(0)); });
  EXPECT_EQ(page.page().bytes, before.bytes);
}

TEST(SlottedPageTest, AllInvalidRecordIdFormsAreRejected) {
  SlottedPage page(Page::create(1, PageType::Heap));
  const auto rid = page.insert(bytes(10), 7);
  for (const auto invalid : {RecordId{}, RecordId{2, 0, 7}, RecordId{1, 1, 7},
                             RecordId{1, 0, 8}, RecordId{1, 0, 0}}) {
    expect_error(ErrorCode::NotFound, [&] { (void)page.get(invalid); });
    expect_error(ErrorCode::NotFound, [&] { page.erase(invalid); });
    expect_error(ErrorCode::NotFound, [&] { page.update(invalid, bytes(1)); });
  }
  page.erase(rid);
  expect_error(ErrorCode::NotFound, [&] { page.erase(rid); });
  expect_error(ErrorCode::InvalidArgument, [&] { (void)page.insert(bytes(1), 0); });
}

TEST(SlottedPageTest, RejectsMalformedLayoutEvenWithValidChecksum) {
  SlottedPage page(Page::create(1, PageType::Heap));
  (void)page.insert(bytes(10), 1);
  (void)page.insert(bytes(10), 2);
  auto check_mutation = [&](auto mutate) {
    Page damaged = page.page();
    mutate(damaged);
    damaged.seal();
    expect_error(ErrorCode::Corruption, [&] { (void)SlottedPage(damaged); });
  };
  check_mutation([](Page& p) { encoding::store<std::uint16_t>(p.bytes, 24, 60000); });
  check_mutation([](Page& p) { encoding::store<std::uint16_t>(p.bytes, 26, 0); });
  check_mutation([](Page& p) { encoding::store<std::uint16_t>(p.bytes, 28, 0); });
  check_mutation([](Page& p) { encoding::store<std::uint16_t>(p.bytes, 30, 1); });
  check_mutation([](Page& p) { encoding::store<std::uint16_t>(p.bytes, 32, 4095); });
  check_mutation([](Page& p) { encoding::store<std::uint16_t>(p.bytes, 34, 0); });
  check_mutation([](Page& p) { encoding::store<std::uint64_t>(p.bytes, 36, 0); });
  check_mutation([](Page& p) { // overlapping records
    encoding::store<std::uint16_t>(p.bytes, 44, encoding::load<std::uint16_t>(p.bytes, 32));
  });
}

TEST(SlottedPageTest, RandomizedOperationsCheckInvariantsAfterEachMutation) {
  SlottedPage page(Page::create(1, PageType::Heap));
  std::vector<std::pair<RecordId, Bytes>> model;
  std::mt19937 rng(0x1234);
  std::uint64_t generation = 1;
  for (int step = 0; step < 1500; ++step) {
    const auto action = rng() % 3;
    if (model.empty() || action == 0) {
      const auto data = bytes(1 + rng() % 600, static_cast<unsigned char>(rng()));
      if (page.can_insert(data.size())) model.emplace_back(page.insert(data, generation++), data);
    } else {
      const auto index = rng() % model.size();
      auto& [rid, data] = model[index];
      if (action == 1) {
        page.erase(rid);
        model.erase(model.begin() + static_cast<std::ptrdiff_t>(index));
      } else {
        const auto replacement = bytes(1 + rng() % 700, static_cast<unsigned char>(rng()));
        const auto old = page.page();
        try { page.update(rid, replacement); data = replacement; }
        catch (const Error& e) { ASSERT_EQ(e.code(), ErrorCode::PageFull); EXPECT_EQ(page.page().bytes, old.bytes); }
      }
    }
    page.validate();
    ASSERT_EQ(page.live_count(), model.size());
    for (const auto& [rid, data] : model) ASSERT_EQ(page.get(rid), data);
  }
}

TEST_F(StorageTest, AllocateFreeAndReuseAcrossRestart) {
  {
    DiskManager disk(path_);
    EXPECT_EQ(disk.page_count(), 1U);
    EXPECT_EQ(disk.allocate_page(), 1U);
    EXPECT_EQ(disk.allocate_page(), 2U);
    EXPECT_EQ(disk.allocate_page(), 3U);
    disk.deallocate_page(2);
    disk.deallocate_page(1);
    expect_error(ErrorCode::NotFound, [&] { disk.deallocate_page(1); });
    expect_error(ErrorCode::NotFound, [&] { (void)disk.read_page(1); });
    disk.close();
  }
  DiskManager disk(path_);
  EXPECT_EQ(disk.allocate_page(), 1U);
  EXPECT_EQ(disk.allocate_page(), 2U);
  EXPECT_EQ(disk.page_count(), 4U);
  EXPECT_EQ(disk.allocated_pages(), (std::vector<PageId>{1, 2, 3}));
  disk.close();
  EXPECT_EQ(std::filesystem::file_size(path_), 4 * kPageSize);
}

TEST_F(StorageTest, PageWritesPersistAndGenerationsNeverRepeatOnReopen) {
  std::uint64_t generation;
  {
    DiskManager disk(path_);
    const auto id = disk.allocate_page();
    SlottedPage page(disk.read_page(id));
    generation = disk.reserve_generation();
    (void)page.insert(bytes(100, 77), generation);
    disk.write_page(page.page());
    disk.sync();
    disk.close();
  }
  DiskManager disk(path_);
  EXPECT_EQ(SlottedPage(disk.read_page(1)).get({1, 0, generation}), bytes(100, 77));
  EXPECT_GT(disk.reserve_generation(), generation);
}

TEST_F(StorageTest, InvalidPagesAndClosedHandlesReportErrors) {
  DiskManager disk(path_);
  const auto id = disk.allocate_page();
  for (const auto invalid : {PageId{0}, PageId{2}, kInvalidPageId}) {
    expect_error(ErrorCode::NotFound, [&] { (void)disk.read_page(invalid); });
    expect_error(ErrorCode::NotFound, [&] { disk.deallocate_page(invalid); });
    expect_error(ErrorCode::NotFound, [&] { disk.write_page(Page::create(invalid, PageType::Heap)); });
  }
  expect_error(ErrorCode::InvalidArgument, [&] { disk.write_page(Page::create(id, PageType::Free)); });
  disk.close();
  EXPECT_NO_THROW(disk.close());
  expect_error(ErrorCode::Closed, [&] { (void)disk.allocate_page(); });
  expect_error(ErrorCode::Closed, [&] { disk.sync(); });
  expect_error(ErrorCode::Closed, [&] { (void)disk.read_page(1); });
}

TEST_F(StorageTest, ExclusiveOwnerLockIsReleasedAtClose) {
  DiskManager first(path_);
  expect_error(ErrorCode::Busy, [&] { DiskManager second(path_); });
  first.close();
  EXPECT_NO_THROW(DiskManager second(path_));
}

TEST_F(StorageTest, MissingParentAndExistingEmptyFileFailExplicitly) {
  expect_error(ErrorCode::Io, [&] { DiskManager disk(directory_ / "missing" / "test.db"); });
  { std::ofstream empty(path_); }
  expect_error(ErrorCode::Corruption, [&] { DiskManager disk(path_); });
  expect_error(ErrorCode::Io, [&] { DiskManager disk(directory_); });
}

TEST_F(StorageTest, TruncationAndCorruptChecksumAreRejected) {
  { DiskManager disk(path_); (void)disk.allocate_page(); disk.close(); }
  std::filesystem::resize_file(path_, kPageSize + 1);
  expect_error(ErrorCode::Corruption, [&] { DiskManager disk(path_); });
  std::filesystem::remove(path_);
  { DiskManager disk(path_); (void)disk.allocate_page(); disk.close(); }
  {
    std::fstream file(path_, std::ios::in | std::ios::out | std::ios::binary);
    file.seekp(kPageSize + 100);
    file.put('X');
  }
  expect_error(ErrorCode::Corruption, [&] { DiskManager disk(path_); });
}

// Edit a checksummed page to test semantic metadata validation, not just CRC.
void edit_page(const std::filesystem::path& path, PageId id, const std::function<void(Page&)>& edit) {
  std::fstream file(path, std::ios::in | std::ios::out | std::ios::binary);
  ASSERT_TRUE(file.good());
  Page page;
  file.seekg(static_cast<std::streamoff>(id) * kPageSize);
  file.read(reinterpret_cast<char*>(page.bytes.data()), kPageSize);
  ASSERT_TRUE(file.good());
  edit(page);
  page.seal();
  file.seekp(static_cast<std::streamoff>(id) * kPageSize);
  file.write(reinterpret_cast<const char*>(page.bytes.data()), kPageSize);
  ASSERT_TRUE(file.good());
}

TEST_F(StorageTest, RejectsFreeListCyclesAndOrphans) {
  {
    DiskManager disk(path_);
    (void)disk.allocate_page();
    disk.deallocate_page(1);
    disk.close();
  }
  edit_page(path_, 1, [](Page& p) { encoding::store<PageId>(p.bytes, 24, 1); });
  expect_error(ErrorCode::Corruption, [&] { DiskManager disk(path_); });
  edit_page(path_, 0, [](Page& p) { encoding::store<PageId>(p.bytes, 28, kInvalidPageId); });
  expect_error(ErrorCode::Corruption, [&] { DiskManager disk(path_); });
}

TEST_F(StorageTest, RejectsBadMetadataVersionCountAndFreeHead) {
  { DiskManager disk(path_); disk.close(); }
  edit_page(path_, 0, [](Page& p) { encoding::store<PageId>(p.bytes, 28, 100); });
  expect_error(ErrorCode::Corruption, [&] { DiskManager disk(path_); });
  edit_page(path_, 0, [](Page& p) {
    encoding::store<PageId>(p.bytes, 28, kInvalidPageId);
    encoding::store<PageId>(p.bytes, 24, 2);
  });
  expect_error(ErrorCode::Corruption, [&] { DiskManager disk(path_); });
  edit_page(path_, 0, [](Page& p) {
    encoding::store<PageId>(p.bytes, 24, 1);
    encoding::store<std::uint16_t>(p.bytes, 4, 99);
  });
  expect_error(ErrorCode::Corruption, [&] { DiskManager disk(path_); });
}

TEST_F(StorageTest, GenerationExhaustionRejectsInsteadOfWrapping) {
  { DiskManager disk(path_); disk.close(); }
  edit_page(path_, 0, [](Page& p) {
    encoding::store<std::uint64_t>(p.bytes, 32, std::numeric_limits<std::uint64_t>::max());
  });
  DiskManager disk(path_);
  expect_error(ErrorCode::InvalidArgument, [&] { (void)disk.reserve_generation(); });
}

TEST_F(StorageTest, HeapCrudPersistenceAndScan) {
  RecordId alice, bob;
  {
    HeapFile heap(path_);
    alice = heap.insert(serialize({1, "Alice", 1000}));
    bob = heap.insert(serialize({2, "Bob", 500}));
    heap.update(alice, serialize({1, "Alice Smith", 800}));
    EXPECT_EQ(deserialize(heap.get(alice)), (Record{1, "Alice Smith", 800}));
    heap.erase(bob);
    EXPECT_EQ(heap.scan().size(), 1U);
    heap.close();
  }
  HeapFile heap(path_);
  EXPECT_EQ(deserialize(heap.get(alice)), (Record{1, "Alice Smith", 800}));
  expect_error(ErrorCode::NotFound, [&] { (void)heap.get(bob); });
  EXPECT_EQ(heap.scan().front().first, alice);
  heap.close();
}

TEST_F(StorageTest, StaleIdsStayInvalidAfterSlotAndWholePageReuse) {
  RecordId a, b, replacement;
  {
    HeapFile heap(path_);
    a = heap.insert(bytes(100, 1));
    b = heap.insert(bytes(100, 2));
    heap.erase(a);
    replacement = heap.insert(bytes(100, 3));
    EXPECT_EQ(a.page, replacement.page);
    EXPECT_EQ(a.slot, replacement.slot);
    EXPECT_NE(a.generation, replacement.generation);
    expect_error(ErrorCode::NotFound, [&] { (void)heap.get(a); });
    heap.erase(b);
    heap.erase(replacement);
    heap.close();
  }
  HeapFile heap(path_);
  const auto new_id = heap.insert(bytes(4052, 4));
  EXPECT_EQ(new_id.page, a.page);
  EXPECT_EQ(new_id.slot, a.slot);
  EXPECT_NE(new_id.generation, replacement.generation);
  EXPECT_EQ(heap.page_count(), 2U);
  for (const auto stale : {a, b, replacement}) {
    expect_error(ErrorCode::NotFound, [&] { (void)heap.get(stale); });
    expect_error(ErrorCode::NotFound, [&] { heap.update(stale, bytes(1)); });
    expect_error(ErrorCode::NotFound, [&] { heap.erase(stale); });
  }
}

TEST_F(StorageTest, FullHeapPagesAllocateAndRejectedUpdatePreservesOriginal) {
  HeapFile heap(path_);
  const auto a = heap.insert(bytes(2000, 1));
  const auto b = heap.insert(bytes(2000, 2));
  const auto c = heap.insert(bytes(4052, 3));
  EXPECT_EQ(a.page, b.page);
  EXPECT_NE(c.page, a.page);
  expect_error(ErrorCode::PageFull, [&] { heap.update(a, bytes(3000)); });
  EXPECT_EQ(heap.get(a), bytes(2000, 1));
  EXPECT_EQ(heap.get(b), bytes(2000, 2));
  const auto before = heap.page_count();
  expect_error(ErrorCode::InvalidArgument, [&] { (void)heap.insert(bytes(4053)); });
  expect_error(ErrorCode::InvalidArgument, [&] { (void)heap.insert(bytes(0)); });
  EXPECT_EQ(heap.page_count(), before);
}

TEST_F(StorageTest, RandomizedHeapCrudAgainstModelWithRepeatedReopens) {
  auto heap = std::make_unique<HeapFile>(path_);
  std::vector<std::pair<RecordId, Bytes>> model;
  std::mt19937 rng(20261018);
  for (int step = 0; step < 500; ++step) {
    const auto action = rng() % 4;
    if (model.empty() || action == 0) {
      auto data = bytes(1 + rng() % 900, static_cast<unsigned char>(rng()));
      const auto rid = heap->insert(data);
      model.emplace_back(rid, std::move(data));
    } else {
      const auto index = rng() % model.size();
      auto& [rid, data] = model[index];
      if (action == 1) {
        heap->erase(rid);
        expect_error(ErrorCode::NotFound, [&] { (void)heap->get(rid); });
        model.erase(model.begin() + static_cast<std::ptrdiff_t>(index));
      } else if (action == 2) {
        auto updated = bytes(1 + rng() % 1000, static_cast<unsigned char>(rng()));
        try { heap->update(rid, updated); data = updated; }
        catch (const Error& e) { ASSERT_EQ(e.code(), ErrorCode::PageFull); }
      } else EXPECT_EQ(heap->get(rid), data);
    }
    if (step % 25 == 0) {
      heap->close();
      heap = std::make_unique<HeapFile>(path_);
    }
    const auto actual = heap->scan();
    ASSERT_EQ(actual.size(), model.size());
    for (const auto& [rid, data] : model) {
      ASSERT_EQ(heap->get(rid), data);
      ASSERT_TRUE(std::find(actual.begin(), actual.end(), std::make_pair(rid, data)) != actual.end());
    }
  }
  heap->close();
}

TEST_F(StorageTest, ConcurrentHeapCallsAreSerializedWithoutLostRecords) {
  HeapFile heap(path_);
  std::atomic<bool> okay{true};
  std::vector<std::thread> clients;
  for (int client = 0; client < 4; ++client) {
    clients.emplace_back([&, client] {
      try {
        for (int i = 0; i < 50; ++i) {
          const auto data = serialize({client * 50 + i, "worker", i});
          const auto rid = heap.insert(data);
          if (heap.get(rid) != data) okay = false;
          heap.update(rid, serialize({client * 50 + i, "updated", i + 1}));
          if (i % 2 == 0) heap.erase(rid);
        }
      } catch (...) { okay = false; }
    });
  }
  for (auto& client : clients) client.join();
  EXPECT_TRUE(okay);
  EXPECT_EQ(heap.scan().size(), 100U);
  heap.close();
}

int child_status(pid_t child) {
  int status = 0;
  pid_t result;
  do { result = ::waitpid(child, &status, 0); } while (result < 0 && errno == EINTR);
  if (result != child || !WIFEXITED(status)) return -1;
  return WEXITSTATUS(status);
}

TEST_F(StorageTest, SyncedRecordsSurviveProcessExitWithoutDestructors) {
  const pid_t child = ::fork();
  ASSERT_GE(child, 0);
  if (child == 0) {
    try {
      HeapFile heap(path_);
      (void)heap.insert(serialize({99, "separate process", 123}));
      heap.sync();
      ::_exit(0);  // No destructor and no graceful close.
    } catch (...) { ::_exit(10); }
  }
  ASSERT_EQ(child_status(child), 0);
  HeapFile heap(path_);
  const auto rows = heap.scan();
  ASSERT_EQ(rows.size(), 1U);
  EXPECT_EQ(deserialize(rows.front().second), (Record{99, "separate process", 123}));
}

TEST_F(StorageTest, RealPartialWriteFailurePoisonsHandleAndIsDetectedOnReopen) {
  const pid_t child = ::fork();
  ASSERT_GE(child, 0);
  if (child == 0) {
    try {
      DiskManager disk(path_);
      ::signal(SIGXFSZ, SIG_IGN);
      struct rlimit limit {};
      if (::getrlimit(RLIMIT_FSIZE, &limit) != 0) ::_exit(11);
      limit.rlim_cur = kPageSize + 100;  // First pwrite extends only part of page 1.
      if (::setrlimit(RLIMIT_FSIZE, &limit) != 0) ::_exit(12);
      try { (void)disk.allocate_page(); ::_exit(13); }
      catch (const Error& e) { if (e.code() != ErrorCode::Io) ::_exit(14); }
      try { disk.sync(); ::_exit(15); }
      catch (const Error& e) { if (e.code() != ErrorCode::Io) ::_exit(16); }
      ::_exit(0);
    } catch (...) { ::_exit(17); }
  }
  ASSERT_EQ(child_status(child), 0);
  EXPECT_EQ(std::filesystem::file_size(path_), kPageSize + 100);
  expect_error(ErrorCode::Corruption, [&] { DiskManager disk(path_); });
}
}  // namespace
}  // namespace tinydb
