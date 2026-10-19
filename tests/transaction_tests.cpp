#include <gtest/gtest.h>

#include <atomic>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <filesystem>
#include <fstream>
#include <functional>
#include <future>
#include <latch>
#include <memory>
#include <sys/resource.h>
#include <sys/wait.h>
#include <thread>

#include "common/encoding.h"
#include "storage/heap_file.h"
#include "transaction/database.h"

namespace tinydb {
namespace {
using namespace std::chrono_literals;
constexpr LockResource kTable{LockResource::Kind::Table};
constexpr LockResource kKey{LockResource::Kind::Key, 0, 1};
void expect_error(ErrorCode code, const std::function<void()>& operation) {
  try { operation(); ADD_FAILURE() << "expected database error"; }
  catch (const Error& error) { EXPECT_EQ(error.code(), code) << error.what(); }
}
template <typename Predicate> bool eventually(Predicate predicate) {
  const auto deadline = std::chrono::steady_clock::now() + 2s;
  while (std::chrono::steady_clock::now() < deadline) {
    if (predicate()) return true;
    std::this_thread::yield();
  }
  return predicate();
}
class TransactionTest : public ::testing::Test {
 protected:
  void SetUp() override {
    auto pattern = (std::filesystem::temp_directory_path() / "tinydb-transactions-XXXXXX").string();
    const auto created = ::mkdtemp(pattern.data());
    ASSERT_NE(created, nullptr);
    directory_ = created;
    path_ = directory_ / "database";
  }
  void TearDown() override { std::filesystem::remove_all(directory_); }
  std::filesystem::path directory_, path_;
};
DatabaseOptions options(std::chrono::milliseconds timeout = 2s) { return {1, 3, timeout, {}}; }
void seed(TransactionalDatabase& db) {
  auto tx = db.begin();
  tx.insert({1, "Alice", 1000}); tx.insert({2, "Bob", 500});
  tx.commit();
}
std::vector<Record> committed_rows(TransactionalDatabase& db) {
  auto tx = db.begin(); auto rows = tx.scan(); tx.commit(); return rows;
}
std::vector<char> file_bytes(const std::filesystem::path& path) {
  std::ifstream file(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}

TEST(LockTest, FullCompatibilityMatrixAndInvalidInputs) {
  const LockMode modes[] = {LockMode::IS, LockMode::IX, LockMode::S, LockMode::SIX, LockMode::X};
  const bool expected[5][5] = {{1,1,1,1,0}, {1,1,0,0,0}, {1,0,1,0,0}, {1,0,0,0,0}, {0,0,0,0,0}};
  for (int a = 0; a < 5; ++a) for (int b = 0; b < 5; ++b)
    EXPECT_EQ(LockManager::compatible(modes[a], modes[b]), expected[a][b]);
  expect_error(ErrorCode::InvalidArgument, [] { LockManager manager(0ms); });
  LockManager manager;
  expect_error(ErrorCode::TransactionAborted, [&] { manager.acquire(1, kKey, LockMode::S); });
  expect_error(ErrorCode::InvalidArgument, [&] { manager.register_transaction(0); });
  manager.register_transaction(1);
  expect_error(ErrorCode::InvalidArgument, [&] { manager.register_transaction(1); });
  manager.release_all(1);
  EXPECT_EQ(manager.stats().resources, 0U);
  LockManager long_wait(std::chrono::milliseconds::max());
  long_wait.register_transaction(1);
  long_wait.acquire(1, kKey, LockMode::S); // Deadline arithmetic must not overflow.
  long_wait.release_all(1);
}

TEST(LockTest, SharedCompatibilityAndStrictUpgradeModes) {
  LockManager manager;
  manager.register_transaction(1); manager.register_transaction(2);
  manager.acquire(1, kKey, LockMode::S); manager.acquire(2, kKey, LockMode::S);
  EXPECT_EQ(manager.stats().grants, 2U);
  manager.release_all(2);
  manager.acquire(1, kKey, LockMode::X);
  manager.acquire(1, kKey, LockMode::S); // Never downgrade X.
  EXPECT_EQ(manager.held_mode(1, kKey), LockMode::X);
  manager.acquire(1, kTable, LockMode::IS);
  manager.acquire(1, kTable, LockMode::S);
  manager.acquire(1, kTable, LockMode::IX);
  EXPECT_EQ(manager.held_mode(1, kTable), LockMode::SIX);
  manager.release_all(1);
  EXPECT_EQ(manager.stats().resources, 0U);
}

TEST(LockTest, TimeoutRetainsGrantedLocksUntilCoordinatorReleasesThem) {
  LockManager manager(30ms);
  manager.register_transaction(1); manager.register_transaction(2);
  manager.acquire(1, kKey, LockMode::S); manager.acquire(2, kKey, LockMode::S);
  expect_error(ErrorCode::LockTimeout, [&] { manager.acquire(1, kKey, LockMode::X); });
  EXPECT_EQ(manager.held_mode(1, kKey), LockMode::S);
  EXPECT_EQ(manager.stats().waiters, 0U);
  EXPECT_EQ(manager.stats().timeouts, 1U);
  manager.release_all(1); manager.release_all(2);
  EXPECT_EQ(manager.stats().active_transactions, 0U);
}

TEST(LockTest, QueuedWriterIsNotBypassedByNewReaders) {
  LockManager manager(2s);
  for (TransactionId id = 1; id <= 3; ++id) manager.register_transaction(id);
  manager.acquire(1, kKey, LockMode::S);
  std::latch writer_granted(1), release_writer(1);
  auto writer = std::async(std::launch::async, [&] {
    manager.acquire(2, kKey, LockMode::X);
    writer_granted.count_down(); release_writer.wait(); manager.release_all(2);
  });
  EXPECT_TRUE(eventually([&] { return manager.stats().waiters == 1; }));
  auto reader = std::async(std::launch::async, [&] { manager.acquire(3, kKey, LockMode::S); manager.release_all(3); });
  EXPECT_TRUE(eventually([&] { return manager.stats().waiters == 2; }));
  manager.release_all(1);
  writer_granted.wait();
  EXPECT_EQ(reader.wait_for(0ms), std::future_status::timeout);
  release_writer.count_down(); writer.get(); reader.get();
  EXPECT_EQ(manager.stats().resources, 0U);
}

TEST(LockTest, CancelingWaiterDoesNotGrantAnAbortedRegistration) {
  LockManager manager(2s);
  manager.register_transaction(1); manager.register_transaction(2);
  manager.acquire(1, kKey, LockMode::X);
  auto waiter = std::async(std::launch::async, [&] {
    try { manager.acquire(2, kKey, LockMode::S); return false; }
    catch (const Error& e) { return e.code() == ErrorCode::TransactionAborted; }
  });
  EXPECT_TRUE(eventually([&] { return manager.stats().waiters == 1; }));
  manager.release_all(2);
  manager.register_transaction(2); // Old waiter retains its canceled registration token.
  EXPECT_TRUE(waiter.get());
  EXPECT_FALSE(manager.held_mode(2, kKey));
  manager.release_all(1); manager.release_all(2);
  EXPECT_EQ(manager.stats().resources, 0U);
}

TEST_F(TransactionTest, DiskUndoRestoresAllocationTypesPayloadAndGenerationHighWater) {
  const auto file = directory_ / "undo.db";
  DiskManager disk(file);
  const auto first = disk.allocate_page(), free = disk.allocate_page();
  SlottedPage page(disk.read_page(first));
  const auto rid = page.insert(Bytes(50, std::byte{7}), disk.reserve_generation());
  disk.write_page(page.page()); disk.deallocate_page(free); disk.sync();
  const auto baseline = disk.read_page(first);
  const auto count = disk.page_count();
  disk.begin_undo();
  expect_error(ErrorCode::Busy, [&] { disk.begin_undo(); });
  expect_error(ErrorCode::Busy, [&] { disk.close(); });
  const auto issued = disk.reserve_generation();
  disk.deallocate_page(first);
  EXPECT_EQ(disk.allocate_page(PageType::IndexLeaf), first);
  (void)disk.allocate_page(PageType::IndexInternal);
  (void)disk.allocate_page(); // Extends beyond baseline.
  disk.rollback_undo();
  EXPECT_EQ(disk.page_count(), count);
  EXPECT_EQ(disk.read_page(first).bytes, baseline.bytes);
  EXPECT_EQ(SlottedPage(disk.read_page(first)).get(rid), Bytes(50, std::byte{7}));
  EXPECT_EQ(disk.allocate_page(), free);
  EXPECT_GT(disk.reserve_generation(), issued);
  expect_error(ErrorCode::InvalidTransactionState, [&] { disk.rollback_undo(); });
  disk.begin_undo(); disk.commit_undo();
  disk.close();
}

TEST_F(TransactionTest, CacheDiscardAfterPhysicalUndoAndPinnedRejection) {
  DiskManager disk(directory_ / "cache.db");
  const auto id = disk.allocate_page();
  BufferPool pool(disk, 1);
  disk.begin_undo();
  auto guard = pool.fetch_write(id);
  SlottedPage page(guard.page());
  (void)page.insert(Bytes(30, std::byte{9}), disk.reserve_generation());
  guard.replace(page.page());
  expect_error(ErrorCode::Busy, [&] { pool.discard_all(); });
  guard.drop(); pool.flush_all();
  disk.rollback_undo(); pool.discard_all();
  EXPECT_EQ(SlottedPage(pool.fetch_read(id).page()).live_count(), 0U);
  EXPECT_EQ(pool.stats().dirty_pages, 0U);
  pool.close(); disk.close();
}

TEST_F(TransactionTest, LifecycleCommitAndRestartPersistenceWithoutDatabaseClose) {
  TransactionId previous;
  {
    TransactionalDatabase db(path_, options());
    auto tx = db.begin(); previous = tx.id();
    EXPECT_EQ(tx.state(), TransactionState::Active);
    tx.insert({1, "Alice", 1000}); tx.insert({2, "Bob", 500});
    EXPECT_EQ(tx.get(1), (Record{1, "Alice", 1000}));
    tx.update(1, {1, "Alice", 800}); tx.update(2, {2, "Bob", 700});
    tx.commit(); EXPECT_EQ(tx.state(), TransactionState::Committed);
    expect_error(ErrorCode::InvalidTransactionState, [&] { tx.commit(); });
    expect_error(ErrorCode::InvalidTransactionState, [&] { (void)tx.get(1); });
    expect_error(ErrorCode::InvalidTransactionState, [&] { tx.rollback(); });
    EXPECT_EQ(db.validate().entries, 2U);
    EXPECT_EQ(db.lock_stats().active_transactions, 0U);
  }
  TransactionalDatabase db(path_, options());
  auto tx = db.begin(); EXPECT_GT(tx.id(), previous);
  EXPECT_EQ(tx.get(1), (Record{1, "Alice", 800}));
  EXPECT_EQ(tx.get(2), (Record{2, "Bob", 700}));
  tx.commit(); db.close();
}

TEST_F(TransactionTest, MultiOperationRollbackAndDestructorRollback) {
  TransactionalDatabase db(path_, options()); seed(db);
  const auto original = committed_rows(db);
  {
    auto tx = db.begin();
    tx.update(1, {1, "changed", 1}); tx.erase(2); tx.insert({3, "new", 3});
    EXPECT_FALSE(tx.get(2)); EXPECT_TRUE(tx.get(3));
    tx.rollback(); tx.rollback();
    EXPECT_EQ(tx.state(), TransactionState::Aborted);
    expect_error(ErrorCode::InvalidTransactionState, [&] { tx.commit(); });
  }
  EXPECT_EQ(committed_rows(db), original);
  { auto tx = db.begin(); tx.update(1, {1, "discarded", 0}); tx.erase(2); }
  EXPECT_EQ(committed_rows(db), original);
  EXPECT_EQ(db.lock_stats().resources, 0U);
  EXPECT_EQ(db.validate().entries, 2U);
  db.close();
}

TEST_F(TransactionTest, ErrorAbortsAllPriorChangesAndDatabaseRemainsUsable) {
  TransactionalDatabase db(path_, options()); seed(db);
  const auto original = committed_rows(db);
  for (int scenario = 0; scenario < 5; ++scenario) {
    auto tx = db.begin();
    tx.update(1, {1, "changed", 0});
    switch (scenario) {
      case 0: expect_error(ErrorCode::DuplicateKey, [&] { tx.insert({2, "duplicate", 1}); }); break;
      case 1: expect_error(ErrorCode::NotFound, [&] { tx.erase(99); }); break;
      case 2: expect_error(ErrorCode::NotFound, [&] { tx.update(99, {99, "missing", 0}); }); break;
      case 3: expect_error(ErrorCode::InvalidArgument, [&] { tx.update(2, {99, "different key", 0}); }); break;
      case 4: expect_error(ErrorCode::InvalidArgument, [&] { tx.insert({3, std::string(5000, 'x'), 0}); }); break;
    }
    EXPECT_EQ(tx.state(), TransactionState::Aborted);
    EXPECT_EQ(committed_rows(db), original);
    EXPECT_EQ(db.lock_stats().resources, 0U);
  }
  db.close();
}

TEST_F(TransactionTest, FailedGrowingUpdateRestoresEarlierOperations) {
  TransactionalDatabase db(path_, options());
  { auto tx = db.begin(); tx.insert({1, std::string(1980, 'a'), 1}); tx.insert({2, std::string(1980, 'b'), 2}); tx.commit(); }
  const auto original = committed_rows(db);
  auto tx = db.begin(); tx.update(2, {2, std::string(1980, 'c'), 0});
  expect_error(ErrorCode::PageFull, [&] { tx.update(1, {1, std::string(3000, 'a'), 1}); });
  EXPECT_EQ(tx.state(), TransactionState::Aborted);
  EXPECT_EQ(committed_rows(db), original);
  db.close();
}

TEST_F(TransactionTest, PhysicalUndoRestoresRootsRecordsRidsAndFileSizesAfterReuse) {
  TransactionalDatabase db(path_, options());
  {
    auto tx = db.begin();
    for (IndexKey key = 0; key < 50; ++key) tx.insert({key, std::string(4032, 'x'), key});
    tx.commit();
  }
  const auto baseline_rows = committed_rows(db);
  db.sync(); // A WAL commit need not flush the materialized data pages.
  const auto baseline_heap = file_bytes(path_ / "records.db");
  const auto baseline_index = file_bytes(path_ / "primary.idx");
  auto tx = db.begin();
  for (IndexKey key = 0; key < 50; ++key) tx.erase(key); // Index root collapse + freed heap pages.
  for (IndexKey key = 100; key < 180; ++key) tx.insert({key, std::string(4032, 'n'), key}); // Reuse + extension + splits.
  tx.rollback();
  EXPECT_EQ(committed_rows(db), baseline_rows);
  EXPECT_EQ(db.validate().entries, 50U);
  db.close();
  const auto restored_heap = file_bytes(path_ / "records.db");
  EXPECT_EQ(restored_heap.size(), baseline_heap.size());
  ASSERT_GE(restored_heap.size(), kPageSize);
  EXPECT_TRUE(std::equal(baseline_heap.begin() + kPageSize, baseline_heap.end(), restored_heap.begin() + kPageSize));
  EXPECT_EQ(file_bytes(path_ / "primary.idx"), baseline_index); // Includes exact persisted RIDs, root and allocator.
  const auto baseline_generation = encoding::load<std::uint64_t>(std::as_bytes(std::span(baseline_heap)), 32);
  const auto restored_generation = encoding::load<std::uint64_t>(std::as_bytes(std::span(restored_heap)), 32);
  EXPECT_GT(restored_generation, baseline_generation);
  TransactionalDatabase reopened(path_, options());
  EXPECT_EQ(committed_rows(reopened), baseline_rows);
}

TEST_F(TransactionTest, DirtyPointReadWaitsUntilRollbackAndUnrelatedReadsProceed) {
  TransactionalDatabase db(path_, options()); seed(db);
  auto writer = db.begin(); writer.update(1, {1, "uncommitted", 0});
  { auto unrelated = db.begin(); EXPECT_EQ(unrelated.get(2)->balance, 500); unrelated.commit(); }
  auto reader = std::async(std::launch::async, [&] {
    auto tx = db.begin(); auto value = tx.get(1); tx.commit(); return value;
  });
  EXPECT_TRUE(eventually([&] { return db.lock_stats().waiters == 1; }));
  EXPECT_EQ(reader.wait_for(0ms), std::future_status::timeout);
  writer.rollback();
  EXPECT_EQ(reader.get(), (Record{1, "Alice", 1000}));
}

TEST_F(TransactionTest, ReadsAreRepeatableAndWritesWaitForReaderCommit) {
  TransactionalDatabase db(path_, options()); seed(db);
  auto reader = db.begin(); EXPECT_EQ(reader.get(1)->balance, 1000);
  auto writer = std::async(std::launch::async, [&] {
    auto tx = db.begin(); auto row = *tx.get_for_update(1); row.balance = 900; tx.update(1, row); tx.commit();
  });
  EXPECT_TRUE(eventually([&] { return db.lock_stats().waiters == 1; }));
  EXPECT_EQ(reader.get(1)->balance, 1000);
  reader.commit(); writer.get();
  EXPECT_EQ(committed_rows(db).front().balance, 900);
}

TEST_F(TransactionTest, MissingKeyReadPreventsInsertUntilTransactionEnds) {
  TransactionalDatabase db(path_, options());
  auto reader = db.begin(); EXPECT_FALSE(reader.get(99));
  auto writer = std::async(std::launch::async, [&] { auto tx = db.begin(); tx.insert({99, "new", 1}); tx.commit(); });
  EXPECT_TRUE(eventually([&] { return db.lock_stats().waiters == 1; }));
  EXPECT_FALSE(reader.get(99)); reader.commit(); writer.get();
  EXPECT_EQ(committed_rows(db).size(), 1U);
}

TEST_F(TransactionTest, RangeLocksPreventPhantomsEvenForEmptyRanges) {
  TransactionalDatabase db(path_, options()); seed(db);
  auto reader = db.begin(); EXPECT_TRUE(reader.scan(10, 20).empty());
  auto writer = std::async(std::launch::async, [&] { auto tx = db.begin(); tx.insert({15, "phantom", 1}); tx.commit(); });
  EXPECT_TRUE(eventually([&] { return db.lock_stats().waiters == 1; }));
  EXPECT_TRUE(reader.scan(10, 20).empty());
  reader.commit(); writer.get();
  auto check = db.begin(); EXPECT_EQ(check.scan(10, 20).size(), 1U); check.commit();
}

TEST_F(TransactionTest, ScansWaitForWholeMultiRecordCommit) {
  TransactionalDatabase db(path_, options()); seed(db);
  auto writer = db.begin(); writer.update(1, {1, "Alice", 800});
  auto reader = std::async(std::launch::async, [&] { auto tx = db.begin(); auto rows = tx.scan(); tx.commit(); return rows; });
  EXPECT_TRUE(eventually([&] { return db.lock_stats().waiters == 1; }));
  writer.update(2, {2, "Bob", 700}); writer.commit();
  const auto rows = reader.get(); ASSERT_EQ(rows.size(), 2U);
  EXPECT_EQ(rows[0].balance, 800); EXPECT_EQ(rows[1].balance, 700);
}

TEST_F(TransactionTest, ScanThenWriteUpgradeRetainsRangeProtection) {
  TransactionalDatabase db(path_, options()); seed(db);
  auto tx = db.begin(); EXPECT_EQ(tx.scan().size(), 2U);
  tx.update(1, {1, "Alice", 123});
  EXPECT_EQ(tx.scan().front().balance, 123);
  auto writer = std::async(std::launch::async, [&] { auto other = db.begin(); other.insert({3, "Carol", 0}); other.commit(); });
  EXPECT_TRUE(eventually([&] { return db.lock_stats().waiters == 1; }));
  tx.rollback(); writer.get();
  EXPECT_EQ(committed_rows(db).front().balance, 1000);
}

TEST_F(TransactionTest, TimeoutAutomaticallyUndoesEarlierUpdatesAndReleasesLocks) {
  TransactionalDatabase db(path_, options(50ms)); seed(db);
  auto reader = db.begin(); (void)reader.get(2);
  auto writer = db.begin(); writer.update(1, {1, "temporary", 0});
  expect_error(ErrorCode::LockTimeout, [&] { writer.update(2, {2, "blocked", 0}); });
  EXPECT_EQ(writer.state(), TransactionState::Aborted);
  reader.commit();
  EXPECT_EQ(committed_rows(db), (std::vector<Record>{{1, "Alice", 1000}, {2, "Bob", 500}}));
  EXPECT_EQ(db.lock_stats().resources, 0U);
}

TEST_F(TransactionTest, WriterGateDeadlockIsResolvedByTimeoutAndPhysicalUndo) {
  TransactionalDatabase db(path_, options(150ms)); seed(db);
  auto first = db.begin(); first.update(1, {1, "temporary", 1});
  auto second = db.begin(); (void)second.get(2);
  auto waiting = std::async(std::launch::async, [tx = std::move(first)]() mutable {
    try { tx.update(2, {2, "first", 200}); tx.commit(); return true; }
    catch (const Error& e) { EXPECT_EQ(e.code(), ErrorCode::LockTimeout); EXPECT_EQ(tx.state(), TransactionState::Aborted); return false; }
  });
  EXPECT_TRUE(eventually([&] { return db.lock_stats().waiters == 1; }));
  bool second_committed = false;
  try { second.update(1, {1, "second", 300}); second.commit(); second_committed = true; }
  catch (const Error& e) { EXPECT_EQ(e.code(), ErrorCode::LockTimeout); }
  const bool first_committed = waiting.get();
  EXPECT_FALSE(first_committed && second_committed);
  const auto rows = committed_rows(db);
  EXPECT_EQ(rows[0].balance, second_committed ? 300 : (first_committed ? 1 : 1000));
  EXPECT_EQ(rows[1].balance, first_committed ? 200 : 500);
  EXPECT_EQ(db.lock_stats().resources, 0U);
}

TEST_F(TransactionTest, ConcurrentReadModifyWriteHasNoLostUpdates) {
  TransactionalDatabase db(path_, options());
  { auto tx = db.begin(); tx.insert({1, "counter", 0}); tx.commit(); }
  std::atomic<bool> okay{true};
  std::vector<std::thread> clients;
  for (int client = 0; client < 4; ++client) clients.emplace_back([&] {
    try {
      for (int i = 0; i < 10; ++i) {
        auto tx = db.begin(); auto row = *tx.get_for_update(1); ++row.balance; tx.update(1, row); tx.commit();
      }
    } catch (...) { okay = false; }
  });
  for (auto& client : clients) client.join();
  EXPECT_TRUE(okay); EXPECT_EQ(committed_rows(db).front().balance, 40);
  EXPECT_EQ(db.validate().entries, 1U);
}

TEST_F(TransactionTest, MovesRollbackReplacedTransactionAndHandlesOutliveWrapper) {
  auto db = std::make_unique<TransactionalDatabase>(path_, options()); seed(*db);
  auto writer = db->begin(); writer.update(2, {2, "temporary", 0});
  auto reader = db->begin(); (void)reader.get(1);
  const auto reader_id = reader.id();
  writer = std::move(reader); // Rolls back the destination writer before taking reader ownership.
  EXPECT_EQ(writer.id(), reader_id);
  expect_error(ErrorCode::InvalidTransactionState, [&] { (void)reader.get(1); });
  EXPECT_EQ(writer.get(2), (Record{2, "Bob", 500}));
  db.reset(); // Live transaction retains storage and lock-manager ownership.
  writer.commit();
}

TEST_F(TransactionTest, CloseAndValidationRejectActiveTransactionsAndClosedBegin) {
  TransactionalDatabase db(path_, options());
  auto tx = db.begin();
  expect_error(ErrorCode::Busy, [&] { db.close(); });
  expect_error(ErrorCode::Busy, [&] { (void)db.validate(); });
  tx.rollback(); db.close(); EXPECT_NO_THROW(db.close());
  expect_error(ErrorCode::Closed, [&] { (void)db.begin(); });
}

TEST_F(TransactionTest, InvalidRangeAbortsEarlierWorkAndBoundsWork) {
  TransactionalDatabase db(path_, options()); seed(db);
  auto tx = db.begin(); tx.update(1, {1, "temporary", 0});
  expect_error(ErrorCode::InvalidArgument, [&] { (void)tx.scan(10, 0); });
  EXPECT_EQ(tx.state(), TransactionState::Aborted);
  auto check = db.begin();
  EXPECT_EQ(check.scan(1, 2), (std::vector<Record>{{1, "Alice", 1000}}));
  EXPECT_EQ(check.scan({}, {}, 1).size(), 1U);
  EXPECT_TRUE(check.scan({}, {}, 0).empty()); check.commit();
}

int wait_child(pid_t child) {
  int status = 0; pid_t result;
  do { result = ::waitpid(child, &status, 0); } while (result < 0 && errno == EINTR);
  return result == child && WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}
TEST_F(TransactionTest, IndexAllocationFailureUndoesHeapAndFailsDatabaseClosed) {
  const auto child = ::fork(); ASSERT_GE(child, 0);
  if (child == 0) {
    try {
      TransactionalDatabase db(path_, options());
      auto tx = db.begin();
      ::signal(SIGXFSZ, SIG_IGN);
      struct rlimit limit {};
      if (::getrlimit(RLIMIT_FSIZE, &limit) != 0) ::_exit(11);
      limit.rlim_cur = 2 * kPageSize;
      if (::setrlimit(RLIMIT_FSIZE, &limit) != 0) ::_exit(12);
      try { tx.insert({1, "new", 1}); ::_exit(13); }
      catch (const Error& e) { if (e.code() != ErrorCode::TransactionFailed && e.code() != ErrorCode::Io) ::_exit(14); }
      if (tx.state() != TransactionState::Aborted && tx.state() != TransactionState::Failed) ::_exit(15);
      try { (void)db.begin(); ::_exit(16); }
      catch (const Error& e) { if (e.code() != ErrorCode::Io) ::_exit(17); }
      ::_exit(0);
    } catch (...) { ::_exit(18); }
  }
  ASSERT_EQ(wait_child(child), 0);
  TransactionalDatabase db(path_, options());
  EXPECT_EQ(db.validate().entries, 0U); EXPECT_TRUE(committed_rows(db).empty());
}

TEST_F(TransactionTest, FailedCommitAndFailedUndoNeverReportCommittedOrAllowClients) {
  { TransactionalDatabase db(path_, options()); seed(db); db.close(); }
  const auto child = ::fork(); ASSERT_GE(child, 0);
  if (child == 0) {
    try {
      TransactionalDatabase db(path_, options());
      auto tx = db.begin(); tx.update(1, {1, "changed", 0});
      ::signal(SIGXFSZ, SIG_IGN);
      struct rlimit limit {};
      if (::getrlimit(RLIMIT_FSIZE, &limit) != 0) ::_exit(11);
      limit.rlim_cur = kPageSize + 100;
      if (::setrlimit(RLIMIT_FSIZE, &limit) != 0) ::_exit(12);
      try { tx.commit(); ::_exit(13); }
      catch (const Error& e) { if (e.code() != ErrorCode::TransactionFailed) ::_exit(14); }
      if (tx.state() != TransactionState::Failed) ::_exit(15);
      try { (void)db.begin(); ::_exit(16); }
      catch (const Error& e) { if (e.code() != ErrorCode::Io) ::_exit(17); }
      if (db.lock_stats().active_transactions != 0) ::_exit(18);
      ::_exit(0);
    } catch (...) { ::_exit(19); }
  }
  ASSERT_EQ(wait_child(child), 0); // Repair after this failed partial write belongs to Phase 5.
}
}  // namespace
}  // namespace tinydb
