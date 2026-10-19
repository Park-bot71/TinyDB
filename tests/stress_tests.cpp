#include <gtest/gtest.h>

#include <atomic>
#include <barrier>
#include <exception>
#include <filesystem>
#include <mutex>
#include <random>
#include <thread>

#include "transaction/database.h"

namespace tinydb {
namespace {
class StressTest : public ::testing::Test {
 protected:
  void SetUp() override {
    auto pattern = (std::filesystem::temp_directory_path() / "tinydb-stress-XXXXXX").string();
    const auto created = ::mkdtemp(pattern.data()); ASSERT_NE(created, nullptr); dir = created;
  }
  void TearDown() override { std::filesystem::remove_all(dir); }
  std::filesystem::path dir;
};
TEST_F(StressTest, SixWritersAndTwoScannersPreserveTransferInvariantAcrossRestart) {
  constexpr int writers = 6, readers = 2, iterations = 30;
  for (std::size_t frames : {1U, 8U}) {
    const auto path = dir / std::to_string(frames);
    std::int64_t expected = 0;
    {
      TransactionalDatabase db(path, {frames, 3, std::chrono::seconds(20), {}});
      auto seed = db.begin(); seed.insert({1, std::string(4032, 'a'), 10000}); seed.insert({2, std::string(4032, 'b'), 10000}); seed.commit();
      std::barrier start(writers + readers);
      std::atomic<int> commits{0}, aborts{0};
      std::mutex error_mutex; std::exception_ptr failure;
      std::vector<std::jthread> clients;
      for (int client = 0; client < writers + readers; ++client) {
        clients.emplace_back([&, client] {
          start.arrive_and_wait();
          try {
            std::mt19937 random(static_cast<unsigned>(42 + client));
            for (int iteration = 0; iteration < iterations; ++iteration) {
              auto tx = db.begin();
              if (client < writers) {
                auto a = tx.get_for_update(1).value(), b = tx.get_for_update(2).value();
                const auto amount = static_cast<std::int64_t>(1 + random() % 5);
                a.balance -= amount; b.balance += amount; tx.update(1, a); tx.update(2, b);
                if (iteration % 5 == 0) { tx.rollback(); ++aborts; }
                else { tx.commit(); ++commits; std::lock_guard lock(error_mutex); expected += amount; }
              } else {
                const auto rows = tx.scan();
                if (rows.size() != 2 || rows[0].balance + rows[1].balance != 20000)
                  throw std::runtime_error("scan observed a partial transfer");
                tx.commit();
              }
            }
          } catch (...) { std::lock_guard lock(error_mutex); if (!failure) failure = std::current_exception(); }
        });
      }
      for (auto& client : clients) client.join();
      if (failure) { try { std::rethrow_exception(failure); } catch (const std::exception& error) { FAIL() << error.what(); } }
      EXPECT_EQ(commits, 144); EXPECT_EQ(aborts, 36);
      EXPECT_EQ(db.lock_stats().active_transactions, 0U); EXPECT_EQ(db.lock_stats().resources, 0U);
      auto tx = db.begin(); EXPECT_EQ(tx.get(1)->balance, 10000 - expected); EXPECT_EQ(tx.get(2)->balance, 10000 + expected); tx.commit();
      EXPECT_EQ(db.validate().entries, 2U); db.close();
    }
    TransactionalDatabase recovered(path, {frames, 0, std::chrono::seconds(20), {}});
    auto tx = recovered.begin(); EXPECT_EQ(tx.get(1)->balance, 10000 - expected); EXPECT_EQ(tx.get(2)->balance, 10000 + expected); tx.commit();
    EXPECT_EQ(recovered.validate().entries, 2U); recovered.close();
  }
}
TEST_F(StressTest, ConcurrentInsertDeleteRollbackAndIndexReuseHaveExactFinalKeys) {
  constexpr int clients = 4, count = 24;
  const auto path = dir / "database";
  {
    TransactionalDatabase db(path, {1, 3, std::chrono::seconds(20), {}});
    std::barrier start(clients);
    std::mutex mutex; std::exception_ptr failure;
    std::vector<std::jthread> threads;
    for (int client = 0; client < clients; ++client) {
      threads.emplace_back([&, client] {
        start.arrive_and_wait();
        try {
          for (int i = 0; i < count; ++i) {
            const auto key = client * 1000 + i;
            auto tx = db.begin(); tx.insert({key, std::string(600, 'x'), key});
            if (i % 3 == 0) tx.rollback(); else tx.commit();
          }
          for (int i = 0; i < count; ++i) if (i % 3 == 1) {
            auto tx = db.begin(); tx.erase(client * 1000 + i); tx.commit();
          }
          for (int i = 0; i < count; ++i) if (i % 3 == 0) {
            auto tx = db.begin(); tx.insert({client * 1000 + i, "reused", i}); tx.commit();
          }
        } catch (...) { std::lock_guard lock(mutex); if (!failure) failure = std::current_exception(); }
      });
    }
    for (auto& thread : threads) thread.join();
    if (failure) { try { std::rethrow_exception(failure); } catch (const std::exception& error) { FAIL() << error.what(); } }
    EXPECT_EQ(db.validate().entries, 64U); db.close();
  }
  TransactionalDatabase recovered(path, {1, 0, std::chrono::seconds(20), {}});
  auto tx = recovered.begin(); const auto rows = tx.scan(); EXPECT_EQ(rows.size(), 64U);
  for (int client = 0; client < clients; ++client) for (int i = 0; i < count; ++i) {
    const auto row = tx.get(client * 1000 + i);
    EXPECT_EQ(row.has_value(), i % 3 != 1);
    if (row) { EXPECT_EQ(row->name, i % 3 == 0 ? "reused" : std::string(600, 'x')); EXPECT_EQ(row->balance, i % 3 == 0 ? i : client * 1000 + i); }
  }
  tx.commit(); EXPECT_EQ(recovered.validate().entries, 64U); recovered.close();
}
}  // namespace
}  // namespace tinydb
