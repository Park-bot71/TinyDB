#include <gtest/gtest.h>

#include <atomic>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <filesystem>
#include <functional>
#include <future>
#include <latch>
#include <memory>
#include <random>
#include <sys/resource.h>
#include <sys/wait.h>
#include <thread>
#include <utility>

#include "buffer/buffer_pool.h"
#include "common/encoding.h"
#include "storage/heap_file.h"
#include "storage/slotted_page.h"

namespace tinydb {
namespace {
using namespace std::chrono_literals;

void expect_error(ErrorCode expected, const std::function<void()>& operation) {
  try { operation(); ADD_FAILURE() << "expected database error"; }
  catch (const Error& error) { EXPECT_EQ(error.code(), expected) << error.what(); }
}
// Low-level page-cache tests use an integer in otherwise-unused heap payload space.
std::uint64_t counter(const Page& page) { return encoding::load<std::uint64_t>(page.bytes, 128); }
Page with_counter(Page page, std::uint64_t value) {
  encoding::store<std::uint64_t>(page.bytes, 128, value);
  page.seal();
  return page;
}

class BufferTest : public ::testing::Test {
 protected:
  void SetUp() override {
    std::string pattern = (std::filesystem::temp_directory_path() / "tinydb-buffer-XXXXXX").string();
    const auto created = ::mkdtemp(pattern.data());
    ASSERT_NE(created, nullptr);
    directory_ = created;
    path_ = directory_ / "test.db";
  }
  void TearDown() override { std::filesystem::remove_all(directory_); }
  std::filesystem::path directory_, path_;
};

TEST_F(BufferTest, RejectsZeroFrames) {
  DiskManager disk(path_);
  expect_error(ErrorCode::InvalidArgument, [&] { BufferPool pool(disk, 0); });
}

TEST_F(BufferTest, RepeatedFetchHitsCacheAndPinsAreReleased) {
  DiskManager disk(path_);
  const auto id = disk.allocate_page();
  BufferPool pool(disk, 2);
  EXPECT_DOUBLE_EQ(pool.stats().hit_rate(), 0.0);
  {
    const auto guard = pool.fetch_read(id);
    EXPECT_EQ(guard.page().id(), id);
    EXPECT_EQ(pool.stats().pins, 1U);
    EXPECT_EQ(pool.stats().pinned_pages, 1U);
  }
  EXPECT_EQ(pool.stats().pins, 0U);
  for (int i = 0; i < 9; ++i) { const auto guard = pool.fetch_read(id); }
  const auto stats = pool.stats();
  EXPECT_EQ(stats.capacity, 2U);
  EXPECT_EQ(stats.resident_pages, 1U);
  EXPECT_EQ(stats.disk_reads, 1U);
  EXPECT_EQ(stats.misses, 1U);
  EXPECT_EQ(stats.hits, 9U);
  EXPECT_DOUBLE_EQ(stats.hit_rate(), 0.9);
}

TEST_F(BufferTest, ExhaustionAndFlushWithPinnedFramesFailWithoutAllocating) {
  DiskManager disk(path_);
  const auto a = disk.allocate_page(), b = disk.allocate_page();
  BufferPool pool(disk, 1);
  auto guard = pool.fetch_write(a);
  guard.replace(with_counter(guard.page(), 7));
  const auto count = pool.page_count();
  expect_error(ErrorCode::Busy, [&] { (void)pool.fetch_read(b); });
  expect_error(ErrorCode::Busy, [&] { (void)pool.new_page(); });
  expect_error(ErrorCode::Busy, [&] { pool.delete_page(a); });
  expect_error(ErrorCode::Busy, [&] { pool.flush_page(a); });
  expect_error(ErrorCode::Busy, [&] { pool.flush_all(); });
  expect_error(ErrorCode::Busy, [&] { pool.close(); });
  EXPECT_EQ(pool.page_count(), count);
  EXPECT_EQ(counter(disk.read_page(a)), 0U);
  EXPECT_EQ(pool.stats().page_writes, 0U);
  guard.drop();
  EXPECT_EQ(pool.fetch_read(b).page().id(), b);
  EXPECT_EQ(counter(disk.read_page(a)), 7U);
  pool.close();
}

TEST_F(BufferTest, GuardMovesAndExceptionUnwindingUnpinExactlyOnce) {
  DiskManager disk(path_);
  const auto a = disk.allocate_page(), b = disk.allocate_page();
  BufferPool pool(disk, 2);
  {
    auto first = pool.fetch_read(a);
    ReadPageGuard moved(std::move(first));
    EXPECT_FALSE(first);
    expect_error(ErrorCode::Closed, [&] { (void)first.page(); });
    auto second = pool.fetch_read(b);
    second = std::move(moved);  // Releases b and takes ownership of a's one pin.
    EXPECT_FALSE(moved);
    EXPECT_EQ(pool.stats().pins, 1U);
    second.drop();
    second.drop();
    EXPECT_EQ(pool.stats().pins, 0U);
  }
  {
    auto first = pool.fetch_write(a);
    WritePageGuard moved(std::move(first));
    EXPECT_FALSE(first);
    auto second = pool.fetch_write(b);
    second = std::move(moved);
    EXPECT_EQ(pool.stats().pins, 1U);
    second.replace(with_counter(second.page(), 11));
  }
  try { const auto guard = pool.fetch_write(a); throw std::runtime_error("test"); }
  catch (const std::runtime_error&) {}
  EXPECT_EQ(pool.stats().pins, 0U);
  EXPECT_EQ(pool.stats().dirty_pages, 1U);
  pool.sync();
  EXPECT_EQ(counter(disk.read_page(a)), 11U);
}

TEST_F(BufferTest, DirtyTrackingFlushAndCleanEviction) {
  DiskManager disk(path_);
  const auto a = disk.allocate_page(), b = disk.allocate_page();
  BufferPool pool(disk, 1);
  {
    auto guard = pool.fetch_write(a);
    guard.replace(with_counter(guard.page(), 99));
  }
  EXPECT_EQ(counter(disk.read_page(a)), 0U);
  EXPECT_EQ(pool.stats().dirty_pages, 1U);
  pool.flush_page(a);
  EXPECT_EQ(counter(disk.read_page(a)), 99U);
  EXPECT_EQ(pool.stats().dirty_pages, 0U);
  EXPECT_EQ(pool.stats().page_writes, 1U);
  pool.flush_page(a);
  { const auto guard = pool.fetch_read(b); }
  { const auto guard = pool.fetch_read(a); EXPECT_EQ(counter(guard.page()), 99U); }
  EXPECT_EQ(pool.stats().page_writes, 1U);  // Clean victims never get written.
  EXPECT_EQ(pool.stats().evictions, 2U);
  pool.flush_page(b);  // Valid nonresident page: no-op.
  expect_error(ErrorCode::NotFound, [&] { pool.flush_page(12345); });
}

TEST_F(BufferTest, ClockSecondChanceAndPinnedVictims) {
  DiskManager disk(path_);
  const auto a = disk.allocate_page(), b = disk.allocate_page();
  const auto c = disk.allocate_page(), d = disk.allocate_page();
  BufferPool pool(disk, 3);
  auto pinned = pool.fetch_read(a);
  { const auto guard = pool.fetch_read(b); }
  { const auto guard = pool.fetch_read(c); }
  { const auto guard = pool.fetch_read(d); }  // Skips a, clears b/c, evicts b.
  auto reads = pool.stats().disk_reads;
  { const auto guard = pool.fetch_read(c); }  // c survives and gets another chance.
  EXPECT_EQ(pool.stats().disk_reads, reads);
  { const auto guard = pool.fetch_read(b); }  // Clears c/d, then evicts c.
  EXPECT_EQ(pool.stats().disk_reads, reads + 1);
  EXPECT_EQ(pinned.page().id(), a);
  EXPECT_EQ(pool.stats().resident_pages, 3U);
  pinned.drop();
}

TEST_F(BufferTest, InvalidFetchDoesNotEvictOrLoseDirtyVictim) {
  DiskManager disk(path_);
  const auto id = disk.allocate_page();
  BufferPool pool(disk, 1);
  { auto guard = pool.fetch_write(id); guard.replace(with_counter(guard.page(), 77)); }
  expect_error(ErrorCode::NotFound, [&] { (void)pool.fetch_read(123); });
  EXPECT_EQ(pool.stats().evictions, 0U);
  EXPECT_EQ(pool.stats().page_writes, 0U);
  EXPECT_EQ(counter(pool.fetch_read(id).page()), 77U);
  pool.close();
}

TEST_F(BufferTest, NewAndDeletedPagesRemainCoherentOnReuse) {
  DiskManager disk(path_);
  BufferPool pool(disk, 1);
  auto guard = pool.new_page();
  const auto id = guard.page().id();
  EXPECT_EQ(id, 1U);
  guard.replace(with_counter(guard.page(), 42));
  guard.drop();
  pool.delete_page(id);  // Discard dirty image, do not write a freed page later.
  EXPECT_EQ(pool.stats().resident_pages, 0U);
  EXPECT_EQ(pool.stats().dirty_pages, 0U);
  EXPECT_EQ(pool.stats().page_writes, 0U);
  expect_error(ErrorCode::NotFound, [&] { (void)pool.fetch_read(id); });
  expect_error(ErrorCode::NotFound, [&] { pool.delete_page(id); });
  auto reused = pool.new_page();
  EXPECT_EQ(reused.page().id(), id);
  EXPECT_EQ(counter(reused.page()), 0U);
  reused.drop();
  EXPECT_EQ(pool.page_count(), 2U);
  pool.close();
}

TEST_F(BufferTest, FlushAllChecksAllPinsBeforeWritingAnyPage) {
  DiskManager disk(path_);
  const auto a = disk.allocate_page(), b = disk.allocate_page();
  BufferPool pool(disk, 2);
  { auto guard = pool.fetch_write(a); guard.replace(with_counter(guard.page(), 1)); }
  auto pinned = pool.fetch_read(b);
  expect_error(ErrorCode::Busy, [&] { pool.flush_all(); });
  EXPECT_EQ(pool.stats().page_writes, 0U);
  pinned.drop();
  pool.flush_all();
  EXPECT_EQ(pool.stats().page_writes, 1U);
}

TEST_F(BufferTest, WalIsForcedBeforeExplicitFlushAndDirtyEviction) {
  DiskManager disk(path_);
  const auto a = disk.allocate_page(), b = disk.allocate_page();
  std::vector<Lsn> forces;
  BufferPool pool(disk, 1, [&](Lsn lsn) {
    // Inspect the real data file during the callback to prove write ordering.
    EXPECT_EQ(encoding::load<Lsn>(disk.read_page(a).bytes, 16), forces.empty() ? 0U : 10U);
    forces.push_back(lsn);
  });
  { auto guard = pool.fetch_write(a); guard.replace(with_counter(guard.page(), 1), 10); }
  pool.flush_page(a);
  EXPECT_EQ(forces, (std::vector<Lsn>{10}));
  EXPECT_EQ(encoding::load<Lsn>(disk.read_page(a).bytes, 16), 10U);
  { auto guard = pool.fetch_write(a); guard.replace(with_counter(guard.page(), 2), 20); }
  { const auto guard = pool.fetch_read(b); }
  EXPECT_EQ(forces, (std::vector<Lsn>{10, 20}));
  EXPECT_EQ(counter(disk.read_page(a)), 2U);
  EXPECT_EQ(encoding::load<Lsn>(disk.read_page(a).bytes, 16), 20U);
  EXPECT_EQ(pool.stats().page_writes, 2U);
}

TEST_F(BufferTest, NonzeroLsnWithoutWalCallbackCannotReachDisk) {
  DiskManager disk(path_);
  const auto a = disk.allocate_page(), b = disk.allocate_page();
  BufferPool pool(disk, 1);
  { auto guard = pool.fetch_write(a); guard.replace(with_counter(guard.page(), 5), 5); }
  expect_error(ErrorCode::InvalidArgument, [&] { pool.flush_page(a); });
  expect_error(ErrorCode::InvalidArgument, [&] { pool.flush_all(); });
  expect_error(ErrorCode::InvalidArgument, [&] { (void)pool.fetch_read(b); });
  expect_error(ErrorCode::InvalidArgument, [&] { pool.close(); });
  EXPECT_EQ(pool.stats().dirty_pages, 1U);
  EXPECT_EQ(pool.stats().evictions, 0U);
  EXPECT_EQ(pool.stats().page_writes, 0U);
  EXPECT_EQ(counter(pool.fetch_read(a).page()), 5U);
  EXPECT_EQ(counter(disk.read_page(a)), 0U);
}

TEST_F(BufferTest, FailedWalForceRetainsDirtyPageAndCanBeRetried) {
  DiskManager disk(path_);
  const auto a = disk.allocate_page(), b = disk.allocate_page();
  bool fail = true;
  std::vector<Lsn> forces;
  BufferPool pool(disk, 1, [&](Lsn lsn) {
    forces.push_back(lsn);
    if (fail) throw Error(ErrorCode::Io, "injected WAL force failure");
  });
  { auto guard = pool.fetch_write(a); guard.replace(with_counter(guard.page(), 7), 70); }
  expect_error(ErrorCode::Io, [&] { (void)pool.fetch_read(b); });
  EXPECT_EQ(pool.stats().pins, 0U);
  EXPECT_EQ(pool.stats().dirty_pages, 1U);
  EXPECT_EQ(counter(disk.read_page(a)), 0U);
  fail = false;
  { const auto guard = pool.fetch_read(b); }
  EXPECT_EQ(forces, (std::vector<Lsn>{70, 70}));
  EXPECT_EQ(counter(disk.read_page(a)), 7U);
  EXPECT_EQ(pool.stats().evictions, 1U);
}

TEST_F(BufferTest, ZeroLsnNeedsNoWalForceAndReplacementCannotChangeIdentityOrRegressLsn) {
  DiskManager disk(path_);
  const auto id = disk.allocate_page();
  int calls = 0;
  BufferPool pool(disk, 1, [&](Lsn) { ++calls; });
  auto guard = pool.fetch_write(id);
  guard.replace(with_counter(guard.page(), 1));
  guard.drop();
  pool.flush_all();
  EXPECT_EQ(calls, 0);
  guard = pool.fetch_write(id);
  guard.replace(with_counter(guard.page(), 2), 20);
  const auto before = guard.page();
  expect_error(ErrorCode::InvalidArgument, [&] { guard.replace(with_counter(before, 3), 19); });
  expect_error(ErrorCode::InvalidArgument, [&] { guard.replace(with_counter(before, 3)); });
  expect_error(ErrorCode::InvalidArgument, [&] { guard.replace(Page::create(99, PageType::Heap), 20); });
  expect_error(ErrorCode::InvalidArgument, [&] { guard.replace(Page::create(id, PageType::Free), 20); });
  auto corrupt = before;
  corrupt.bytes[0] = std::byte{0};
  expect_error(ErrorCode::Corruption, [&] { guard.replace(corrupt, 20); });
  EXPECT_EQ(guard.page().bytes, before.bytes);
  guard.drop();
  pool.sync();
  EXPECT_EQ(calls, 1);
}

TEST_F(BufferTest, CloseFlushesAndRejectsFurtherOperations) {
  DiskManager disk(path_);
  const auto id = disk.allocate_page();
  BufferPool pool(disk, 1);
  { auto guard = pool.fetch_write(id); guard.replace(with_counter(guard.page(), 123)); }
  pool.close();
  EXPECT_EQ(counter(disk.read_page(id)), 123U);
  EXPECT_NO_THROW(pool.close());
  expect_error(ErrorCode::Closed, [&] { (void)pool.fetch_read(id); });
  expect_error(ErrorCode::Closed, [&] { (void)pool.fetch_write(id); });
  expect_error(ErrorCode::Closed, [&] { (void)pool.new_page(); });
  expect_error(ErrorCode::Closed, [&] { pool.delete_page(id); });
  expect_error(ErrorCode::Closed, [&] { pool.flush_all(); });
  expect_error(ErrorCode::Closed, [&] { pool.sync(); });
  expect_error(ErrorCode::Closed, [&] { (void)pool.reserve_generation(); });
  EXPECT_EQ(pool.stats().pins, 0U);
}

TEST_F(BufferTest, LiveGuardKeepsFramesAliveAfterPoolDestructionWithoutImplicitFlush) {
  DiskManager disk(path_);
  const auto id = disk.allocate_page();
  auto pool = std::make_unique<BufferPool>(disk, 1);
  auto guard = pool->fetch_write(id);
  guard.replace(with_counter(guard.page(), 9));
  pool.reset();
  EXPECT_EQ(counter(guard.page()), 9U);
  guard.drop();  // Safe even though the pool object is gone.
  EXPECT_EQ(counter(disk.read_page(id)), 0U);  // Destructor did not claim durability.
}

TEST_F(BufferTest, SharedReadersCoexistAndWriterWaitsWithoutHoldingPoolMutex) {
  DiskManager disk(path_);
  const auto id = disk.allocate_page();
  BufferPool pool(disk, 1);
  std::latch readers_ready(2), release_readers(1);
  std::atomic<bool> okay{true};
  auto reader = [&] {
    try {
      const auto guard = pool.fetch_read(id);
      readers_ready.count_down();
      release_readers.wait();
      if (counter(guard.page()) != 0) okay = false;
    } catch (...) { okay = false; readers_ready.count_down(); }
  };
  std::thread a(reader), b(reader);
  readers_ready.wait();
  EXPECT_EQ(pool.stats().pins, 2U);
  std::latch writer_started(1);
  auto writer = std::async(std::launch::async, [&] {
    writer_started.count_down();
    auto guard = pool.fetch_write(id);
    guard.replace(with_counter(guard.page(), 1));
  });
  writer_started.wait();
  const auto deadline = std::chrono::steady_clock::now() + 2s;
  while (pool.stats().pins != 3 && std::chrono::steady_clock::now() < deadline)
    std::this_thread::yield();
  EXPECT_EQ(pool.stats().pins, 3U);  // Writer is pinned and waiting on the readers.
  EXPECT_EQ(writer.wait_for(0ms), std::future_status::timeout);
  // If a waiting writer held the map mutex, this stats call would deadlock.
  EXPECT_GE(pool.stats().pins, 2U);
  release_readers.count_down();
  a.join(); b.join(); writer.get();
  EXPECT_TRUE(okay);
  EXPECT_EQ(counter(pool.fetch_read(id).page()), 1U);
  EXPECT_EQ(pool.stats().pins, 0U);
}

TEST_F(BufferTest, ConcurrentWritersDoNotLoseUpdates) {
  DiskManager disk(path_);
  const auto id = disk.allocate_page();
  BufferPool pool(disk, 1);
  std::atomic<bool> okay{true};
  std::vector<std::thread> clients;
  for (int i = 0; i < 4; ++i) clients.emplace_back([&] {
    try {
      for (int j = 0; j < 100; ++j) {
        auto guard = pool.fetch_write(id);
        guard.replace(with_counter(guard.page(), counter(guard.page()) + 1));
      }
    } catch (...) { okay = false; }
  });
  for (auto& client : clients) client.join();
  EXPECT_TRUE(okay);
  EXPECT_EQ(counter(pool.fetch_read(id).page()), 400U);
  EXPECT_EQ(pool.stats().pins, 0U);
  EXPECT_EQ(pool.stats().disk_reads, 1U);
  pool.sync();
  EXPECT_EQ(counter(disk.read_page(id)), 400U);
}

TEST_F(BufferTest, ConcurrentEvictionAndFlushingKeepAllIncrements) {
  DiskManager disk(path_);
  std::vector<PageId> ids;
  for (int i = 0; i < 6; ++i) ids.push_back(disk.allocate_page());
  BufferPool pool(disk, 2);
  std::atomic<bool> okay{true}, done{false};
  std::thread flusher([&] {
    while (!done.load()) {
      try { pool.flush_all(); }
      catch (const Error& e) { if (e.code() != ErrorCode::Busy) okay = false; }
      std::this_thread::yield();
    }
  });
  std::vector<std::thread> clients;
  for (unsigned client = 0; client < 4; ++client) clients.emplace_back([&, client] {
    try {
      std::mt19937 rng(100 + client);
      for (int j = 0; j < 100; ++j) {
        const auto id = ids[rng() % ids.size()];
        const auto deadline = std::chrono::steady_clock::now() + 5s;
        for (;;) {
          try {
            auto guard = pool.fetch_write(id);
            guard.replace(with_counter(guard.page(), counter(guard.page()) + 1));
            break;
          } catch (const Error& e) {
            if (e.code() != ErrorCode::Busy || std::chrono::steady_clock::now() > deadline) throw;
            std::this_thread::yield();
          }
        }
      }
    } catch (...) { okay = false; }
  });
  for (auto& client : clients) client.join();
  done = true;
  flusher.join();
  ASSERT_TRUE(okay);
  pool.sync();
  std::uint64_t total = 0;
  for (const auto id : ids) total += counter(disk.read_page(id));
  EXPECT_EQ(total, 400U);
  EXPECT_GT(pool.stats().evictions, 0U);
  EXPECT_LE(pool.stats().resident_pages, 2U);
}

TEST_F(BufferTest, RandomizedCacheWritesSurviveRepeatedEvictionAndReopen) {
  std::vector<std::uint64_t> model(9);
  {
    DiskManager disk(path_);
    std::vector<PageId> ids;
    for (int i = 0; i < 9; ++i) ids.push_back(disk.allocate_page());
    BufferPool pool(disk, 2);
    std::mt19937 rng(20261019);
    for (int i = 0; i < 500; ++i) {
      const auto index = rng() % ids.size();
      if (rng() % 2 == 0) {
        auto guard = pool.fetch_write(ids[index]);
        EXPECT_EQ(counter(guard.page()), model[index]);
        const auto value = rng();
        guard.replace(with_counter(guard.page(), value));
        model[index] = value;
      } else {
        const auto guard = pool.fetch_read(ids[index]);
        EXPECT_EQ(counter(guard.page()), model[index]);
      }
      EXPECT_LE(pool.stats().resident_pages, 2U);
      EXPECT_EQ(pool.stats().pins, 0U);
    }
    pool.close();
    disk.close();
  }
  DiskManager reopened(path_);
  for (std::size_t i = 0; i < model.size(); ++i)
    EXPECT_EQ(counter(reopened.read_page(static_cast<PageId>(i + 1))), model[i]);
}

TEST_F(BufferTest, RealPageWriteErrorPreventsServingCachedPages) {
  const auto child = ::fork();
  ASSERT_GE(child, 0);
  if (child == 0) {
    try {
      DiskManager disk(path_);
      const auto id = disk.allocate_page();
      BufferPool pool(disk, 1);
      { auto guard = pool.fetch_write(id); guard.replace(with_counter(guard.page(), 99)); }
      ::signal(SIGXFSZ, SIG_IGN);
      struct rlimit limit {};
      if (::getrlimit(RLIMIT_FSIZE, &limit) != 0) ::_exit(11);
      limit.rlim_cur = kPageSize;  // Existing page 1 cannot be overwritten.
      if (::setrlimit(RLIMIT_FSIZE, &limit) != 0) ::_exit(12);
      try { pool.flush_all(); ::_exit(13); }
      catch (const Error& e) { if (e.code() != ErrorCode::Io) ::_exit(14); }
      try { (void)pool.fetch_read(id); ::_exit(15); }
      catch (const Error& e) { if (e.code() != ErrorCode::Io) ::_exit(16); }
      if (pool.stats().dirty_pages != 1 || pool.stats().page_writes != 0) ::_exit(17);
      ::_exit(0);
    } catch (...) { ::_exit(18); }
  }
  int status = 0;
  pid_t result;
  do { result = ::waitpid(child, &status, 0); } while (result < 0 && errno == EINTR);
  ASSERT_EQ(result, child);
  ASSERT_TRUE(WIFEXITED(status));
  ASSERT_EQ(WEXITSTATUS(status), 0);
  DiskManager disk(path_);
  EXPECT_EQ(counter(disk.read_page(1)), 0U);
}

TEST_F(BufferTest, HeapUsesCacheAndPersistsWithOnlyOneFrame) {
  std::vector<RecordId> ids;
  {
    HeapFile heap(path_, 1);
    for (int i = 0; i < 6; ++i)
      ids.push_back(heap.insert(Bytes(SlottedPage::kMaxRecordSize, static_cast<std::byte>(i + 1))));
    EXPECT_EQ(heap.buffer_stats().capacity, 1U);
    EXPECT_GT(heap.buffer_stats().evictions, 0U);
    const auto reads = heap.buffer_stats().disk_reads;
    for (int i = 0; i < 10; ++i) EXPECT_EQ(heap.get(ids.back()).front(), std::byte{6});
    EXPECT_EQ(heap.buffer_stats().disk_reads, reads);
    heap.update(ids[0], Bytes(1000, std::byte{42}));
    heap.erase(ids[1]);
    const auto reused = heap.insert(Bytes(4052, std::byte{99}));
    EXPECT_EQ(reused.page, ids[1].page);
    expect_error(ErrorCode::NotFound, [&] { (void)heap.get(ids[1]); });
    heap.sync();
    EXPECT_EQ(heap.buffer_stats().dirty_pages, 0U);
    heap.close();
  }
  HeapFile reopened(path_, 1);
  EXPECT_EQ(reopened.scan().size(), 6U);
  EXPECT_EQ(reopened.get(ids[0]), Bytes(1000, std::byte{42}));
  expect_error(ErrorCode::NotFound, [&] { (void)reopened.get(ids[1]); });
  for (std::size_t i = 2; i < ids.size(); ++i)
    EXPECT_EQ(reopened.get(ids[i]), Bytes(4052, static_cast<std::byte>(i + 1)));
  reopened.close();
}
}  // namespace
}  // namespace tinydb
