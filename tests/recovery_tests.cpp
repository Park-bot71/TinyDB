#include <gtest/gtest.h>

#include <cerrno>
#include <filesystem>
#include <fstream>
#include <functional>
#include <sys/wait.h>
#include <unistd.h>

#include "common/encoding.h"
#include "storage/heap_file.h"
#include "transaction/database.h"

namespace tinydb {
namespace {
void expect_error(ErrorCode code, const std::function<void()>& operation) {
  try { operation(); ADD_FAILURE() << "expected database error"; }
  catch (const Error& e) { EXPECT_EQ(e.code(), code) << e.what(); }
}
class RecoveryTest : public ::testing::Test {
 protected:
  void SetUp() override {
    auto pattern = (std::filesystem::temp_directory_path() / "tinydb-recovery-XXXXXX").string();
    const auto created = ::mkdtemp(pattern.data()); ASSERT_NE(created, nullptr);
    dir_ = created; path_ = dir_ / "database";
  }
  void TearDown() override { std::filesystem::remove_all(dir_); }
  void seed() {
    TransactionalDatabase db(path_, {1, 3, std::chrono::seconds(2), {}});
    auto tx = db.begin();
    tx.insert({1, std::string(4032, 'a'), 1000}); tx.insert({2, std::string(4032, 'b'), 500});
    tx.commit(); db.close();
  }
  int child(const std::string& scenario, const std::string& point) {
    const auto pid = ::fork();
    if (pid < 0) return -1;
    if (pid == 0) {
      ::execl(RECOVERY_WORKER_PATH, RECOVERY_WORKER_PATH, path_.c_str(), scenario.c_str(), point.c_str(), nullptr);
      ::_exit(127);
    }
    int status = 0; pid_t result;
    do { result = ::waitpid(pid, &status, 0); } while (result < 0 && errno == EINTR);
    return result == pid && WIFEXITED(status) ? WEXITSTATUS(status) : -1;
  }
  void balances(std::int64_t alice, std::int64_t bob) {
    TransactionalDatabase db(path_, {1, 0, std::chrono::seconds(2), {}});
    EXPECT_EQ(db.validate().entries, 2U);
    auto tx = db.begin(); const auto rows = tx.scan(); ASSERT_EQ(rows.size(), 2U);
    EXPECT_EQ(rows[0], (Record{1, std::string(4032, 'a'), alice}));
    EXPECT_EQ(rows[1], (Record{2, std::string(4032, 'b'), bob}));
    tx.commit(); db.close();
  }
  std::filesystem::path dir_, path_;
};
std::vector<std::byte> read_bytes(const std::filesystem::path& path) {
  std::ifstream in(path, std::ios::binary);
  std::vector<char> chars{std::istreambuf_iterator<char>(in), {}};
  std::vector<std::byte> bytes(chars.size());
  for (std::size_t i = 0; i < chars.size(); ++i) bytes[i] = static_cast<std::byte>(chars[i]);
  return bytes;
}
void flip(const std::filesystem::path& path, std::uint64_t offset) {
  std::fstream file(path, std::ios::binary | std::ios::in | std::ios::out);
  file.seekg(static_cast<std::streamoff>(offset)); char c = 0; file.get(c);
  ASSERT_TRUE(file.good()); c ^= 1;
  file.seekp(static_cast<std::streamoff>(offset)); file.put(c); ASSERT_TRUE(file.good());
}

TEST_F(RecoveryTest, CrashBetweenUpdatesAndBeforeCommitUndoesLoser) {
  for (const std::string point : {"between_updates", "before_commit"}) {
    path_ = dir_ / point; seed();
    ASSERT_EQ(child("transfer", point), 86);
    balances(1000, 500);
    balances(1000, 500); // Repeated restart must not resurrect the loser.
  }
}
TEST_F(RecoveryTest, DurableCommitBeforeDataFlushIsRedoneAndIdsAdvance) {
  seed(); ASSERT_EQ(child("transfer", "commit.after_force"), 86);
  {
    // Offline inspection before recovery proves that COMMIT preceded one data write.
    HeapFile raw(path_ / "records.db", 1);
    for (const auto& [rid, bytes] : raw.scan()) {
      (void)rid; const auto row = deserialize(bytes);
      EXPECT_EQ(row.balance, row.key == 1 ? 800 : 500);
    }
  }
  std::uint64_t old_id = 0; { std::ifstream in(path_ / "worker.txid"); in >> old_id; ASSERT_TRUE(in.good() || in.eof()); }
  TransactionalDatabase db(path_);
  EXPECT_GT(db.wal_stats().redo_pages, 0U);
  auto tx = db.begin(); EXPECT_GT(tx.id(), old_id);
  EXPECT_EQ(tx.get(1)->balance, 800); EXPECT_EQ(tx.get(2)->balance, 700);
  tx.commit(); db.close(); balances(800, 700);
}
TEST_F(RecoveryTest, AcknowledgedCommitWithoutCloseSurvivesProcessExit) {
  seed(); ASSERT_EQ(child("no_close", "never"), 86); balances(800, 700);
}
TEST_F(RecoveryTest, TornUncommittedAndCommittedDataPagesAreRepaired) {
  seed(); ASSERT_EQ(child("transfer", "data.heap.after_half_write"), 86); balances(1000, 500);
  ASSERT_EQ(child("transfer", "torn_after_commit"), 86); balances(800, 700);
}
TEST_F(RecoveryTest, AbortDecisionAndInterruptedPhysicalUndoRemainAborted) {
  for (const std::string point : {"abort.after_force", "torn_during_abort"}) {
    path_ = dir_ / point; seed();
    ASSERT_EQ(child("abort", point), 86); balances(1000, 500);
  }
}
TEST_F(RecoveryTest, IncompleteWalHeadersPayloadsAndCommitFramesAreTrimmed) {
  for (const std::string point : {"wal.begin.after_half_frame", "wal.update.after_half_frame", "wal.commit.after_half_frame"}) {
    path_ = dir_ / point; seed();
    ASSERT_EQ(child("transfer", point), 86);
    {
      TransactionalDatabase db(path_);
      EXPECT_GT(db.wal_stats().trimmed_bytes, 0U);
      auto tx = db.begin(); EXPECT_EQ(tx.get(1)->balance, 1000); EXPECT_EQ(tx.get(2)->balance, 500); tx.commit();
    }
    balances(1000, 500);
  }
}
TEST_F(RecoveryTest, RecoveryCanCrashRepeatedlyDuringRedoAndPublishing) {
  seed(); ASSERT_EQ(child("transfer", "commit.after_force"), 86);
  for (const std::string point : {"recovery.after_baseline", "recovery.redo_page", "recovery.after_redo",
                                 "recovery.after_half_page", "recovery.after_publish_heap"})
    ASSERT_EQ(child("recover", point), 86) << point;
  balances(800, 700); balances(800, 700);
}
TEST_F(RecoveryTest, UncommittedStructuralChangesRestoreOriginalAndCommittedSplitsPersist) {
  seed(); ASSERT_EQ(child("structural", "before_commit"), 86); balances(1000, 500);
  ASSERT_EQ(child("structural", "commit.after_force"), 86);
  TransactionalDatabase db(path_, {1, 0, std::chrono::seconds(2), {}});
  const auto valid = db.validate(); EXPECT_EQ(valid.entries, 20U); EXPECT_GE(valid.height, 3U);
  auto tx = db.begin(); const auto rows = tx.scan(); ASSERT_EQ(rows.size(), 20U);
  for (std::size_t i = 0; i < rows.size(); ++i) EXPECT_EQ(rows[i], (Record{static_cast<IndexKey>(100 + i), std::string(4032, 'n'), static_cast<IndexKey>(100 + i)}));
  EXPECT_FALSE(tx.get(1)); EXPECT_FALSE(tx.get(2)); tx.commit(); db.close();
  EXPECT_EQ(child("recover", "never"), 0);
}
TEST_F(RecoveryTest, TornSuperblocksMissingDataFilesAndBadIndexHeadersAreRepairable) {
  seed(); ASSERT_EQ(child("transfer", "commit.after_force"), 86);
  std::filesystem::resize_file(path_ / "records.db", kPageSize + 99);
  flip(path_ / "records.db", 0);
  std::filesystem::resize_file(path_ / "primary.idx", 0);
  balances(800, 700);
  std::filesystem::remove(path_ / "records.db"); balances(800, 700);
  std::filesystem::remove(path_ / "records.db"); std::filesystem::remove(path_ / "primary.idx"); balances(800, 700);
}
TEST_F(RecoveryTest, CompleteCorruptWalFramesAreNotTreatedAsTornTails) {
  for (const bool last : {false, true}) {
    path_ = dir_ / (last ? "last" : "interior"); seed();
    const auto bytes = read_bytes(path_ / "database.wal");
    std::vector<std::size_t> offsets;
    for (std::size_t offset = 64; offset < bytes.size();) { offsets.push_back(offset); offset += encoding::load<std::uint32_t>(bytes, offset + 4); }
    ASSERT_GT(offsets.size(), 3U);
    flip(path_ / "database.wal", (last ? offsets.back() : offsets[1]) + 24);
    const auto data = read_bytes(path_ / "records.db");
    expect_error(ErrorCode::Corruption, [&] { TransactionalDatabase db(path_); });
    EXPECT_EQ(read_bytes(path_ / "records.db"), data); // Validation precedes publishing.
  }
}
TEST_F(RecoveryTest, CorruptPayloadAndLostWalOrManifestFailClosed) {
  for (int scenario = 0; scenario < 3; ++scenario) {
    path_ = dir_ / std::to_string(scenario); seed();
    if (scenario == 0) {
      const auto bytes = read_bytes(path_ / "database.wal");
      std::size_t offset = 64;
      while (encoding::load<std::uint16_t>(bytes, offset + 10) != static_cast<std::uint16_t>(LogType::Update))
        offset += encoding::load<std::uint32_t>(bytes, offset + 4);
      flip(path_ / "database.wal", offset + 80 + 100);
    } else if (scenario == 1) std::filesystem::remove(path_ / "database.wal");
    else std::filesystem::remove(path_ / "recovery.meta");
    expect_error(ErrorCode::Corruption, [&] { TransactionalDatabase db(path_); });
  }
}
TEST_F(RecoveryTest, ImmutableBaselineCorruptionDoesNotOverwriteData) {
  seed(); flip(path_ / "baseline.heap", 10);
  const auto before = read_bytes(path_ / "records.db");
  expect_error(ErrorCode::Corruption, [&] { TransactionalDatabase db(path_); });
  EXPECT_EQ(read_bytes(path_ / "records.db"), before);
}
TEST_F(RecoveryTest, WalOwnershipRejectsSecondCoordinatorBeforeRepair) {
  seed(); TransactionalDatabase first(path_);
  expect_error(ErrorCode::Busy, [&] { TransactionalDatabase second(path_); });
  EXPECT_EQ(first.validate().entries, 2U);
}
TEST_F(RecoveryTest, FailedCommitForceNeverReportsSuccessAndOutcomeIsAtomic) {
  seed(); bool fail = false;
  {
    DatabaseOptions opts; opts.fault_hook = [&](std::string_view point) {
      if (fail && point == "wal.before_force") throw Error(ErrorCode::Io, "injected force failure");
    };
    TransactionalDatabase db(path_, opts);
    auto tx = db.begin(); auto a = *tx.get_for_update(1), b = *tx.get_for_update(2);
    a.balance = 800; b.balance = 700; tx.update(1, a); tx.update(2, b);
    fail = true;
    expect_error(ErrorCode::TransactionFailed, [&] { tx.commit(); });
    EXPECT_EQ(tx.state(), TransactionState::Failed);
    expect_error(ErrorCode::Io, [&] { (void)db.begin(); });
  }
  fail = false;
  TransactionalDatabase db(path_); auto tx = db.begin(); const auto rows = tx.scan();
  ASSERT_EQ(rows.size(), 2U);
  EXPECT_TRUE((rows[0].balance == 800 && rows[1].balance == 700) || (rows[0].balance == 1000 && rows[1].balance == 500));
  tx.commit();
}
TEST_F(RecoveryTest, AFailureAfterForcedCommitDoesNotUndoTheCommittedDecision) {
  seed(); bool fail = false;
  {
    DatabaseOptions opts; opts.fault_hook = [&](std::string_view point) {
      if (fail && point == "commit.after_force") throw Error(ErrorCode::Io, "fault after force");
    };
    TransactionalDatabase db(path_, opts);
    auto tx = db.begin(); auto a = *tx.get_for_update(1); a.balance = 800; tx.update(1, a); fail = true;
    expect_error(ErrorCode::TransactionFailed, [&] { tx.commit(); });
  }
  balances(800, 500);
}
TEST_F(RecoveryTest, AdoptsExistingUnloggedHeapAndIndexAsNonemptyBaseline) {
  std::filesystem::create_directories(path_);
  {
    HeapFile heap(path_ / "records.db", 1); BPlusTree index(path_ / "primary.idx", {1, 3});
    for (const auto& row : {Record{1, std::string(4032, 'a'), 1000}, Record{2, std::string(4032, 'b'), 500}}) {
      const auto rid = heap.insert(serialize(row)); ASSERT_TRUE(index.insert(row.key, rid));
    }
    heap.close(); index.close();
  }
  balances(1000, 500);
  ASSERT_EQ(child("transfer", "commit.after_force"), 86); balances(800, 700);
}
TEST_F(RecoveryTest, DataOnlyCopyWithWalStampsIsNotSilentlyAdopted) {
  seed();
  for (const auto* file : {"database.wal", "recovery.meta", "baseline.heap", "baseline.index"})
    std::filesystem::remove(path_ / file);
  expect_error(ErrorCode::Corruption, [&] { TransactionalDatabase db(path_); });
}
TEST_F(RecoveryTest, RawStorageOwnerPreventsRecoveryFromOverwritingItsFiles) {
  seed();
  HeapFile raw(path_ / "records.db", 1);
  const auto before = read_bytes(path_ / "records.db");
  expect_error(ErrorCode::Busy, [&] { TransactionalDatabase db(path_); });
  EXPECT_EQ(read_bytes(path_ / "records.db"), before);
  raw.close(); balances(1000, 500);
}
}  // namespace
}  // namespace tinydb
