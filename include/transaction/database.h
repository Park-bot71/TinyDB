#pragma once

#include <chrono>
#include <filesystem>
#include <limits>
#include <memory>
#include <optional>
#include <vector>

#include "concurrency/lock_manager.h"
#include "index/b_plus_tree.h"
#include "storage/record.h"
#include "recovery/wal.h"

namespace tinydb {
namespace detail { struct DatabaseState; }
enum class TransactionState { Active, Committing, Committed, Aborting, Aborted, Failed };
struct DatabaseOptions {
  std::size_t buffer_frames = 64;
  std::uint16_t index_max_keys = 0;
  std::chrono::milliseconds lock_timeout{1000};
  FaultHook fault_hook; // Optional deterministic fault injection; empty in normal operation.
};

class Transaction {
 public:
  ~Transaction();  // Rolls back unfinished work; an undo failure fails the database.
  Transaction(Transaction&&) noexcept;
  Transaction& operator=(Transaction&&) noexcept;
  Transaction(const Transaction&) = delete;
  Transaction& operator=(const Transaction&) = delete;
  [[nodiscard]] TransactionId id() const;
  [[nodiscard]] TransactionState state() const;
  [[nodiscard]] std::optional<Record> get(IndexKey key);
  [[nodiscard]] std::optional<Record> get_for_update(IndexKey key);
  void insert(const Record& record);
  void update(IndexKey key, const Record& record);  // Primary key cannot change.
  void erase(IndexKey key);
  [[nodiscard]] std::vector<Record> scan(std::optional<IndexKey> lower = {},
                                       std::optional<IndexKey> upper = {},
                                       std::size_t limit = std::numeric_limits<std::size_t>::max());
  void commit();
  void rollback();
 private:
  friend class TransactionalDatabase;
  struct Impl;
  explicit Transaction(std::unique_ptr<Impl> impl);
  Impl& implementation() const;
  void cleanup() noexcept;
  std::unique_ptr<Impl> impl_;
};

// One fixed-schema table, records.db + primary.idx. All clients of this database
// use these transactions; low-level HeapFile/BPlusTree APIs remain independent.
class TransactionalDatabase {
 public:
  explicit TransactionalDatabase(const std::filesystem::path& directory, DatabaseOptions options = {});
  TransactionalDatabase(const TransactionalDatabase&) = delete;
  TransactionalDatabase& operator=(const TransactionalDatabase&) = delete;
  [[nodiscard]] Transaction begin();
  [[nodiscard]] TreeValidation validate() const;  // Busy while transactions are active.
  [[nodiscard]] LockStats lock_stats() const;
  [[nodiscard]] WalStats wal_stats() const;
  void sync(); // Flush materialized data; does not truncate/checkpoint the WAL.
  void close();  // Busy while transactions are active. Checked synchronization.
 private:
  std::shared_ptr<detail::DatabaseState> state_;
};
}  // namespace tinydb
