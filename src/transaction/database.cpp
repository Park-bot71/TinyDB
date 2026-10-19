#include "transaction/database.h"

#include <atomic>
#include <exception>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <utility>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <unistd.h>

#include "common/error.h"
#include "storage/heap_file.h"

namespace tinydb {
namespace {
constexpr LockResource kWriter{LockResource::Kind::Writer};
constexpr LockResource kTable{LockResource::Kind::Table};
LockResource key_resource(IndexKey key) { return {LockResource::Kind::Key, 0, key}; }
std::filesystem::path checked_directory(const std::filesystem::path& directory, DatabaseOptions options) {
  if (options.buffer_frames == 0 || options.lock_timeout.count() <= 0 ||
      (options.index_max_keys != 0 && (options.index_max_keys < 3 || options.index_max_keys > kIndexMaxKeys)))
    throw Error(ErrorCode::InvalidArgument, "invalid database buffer, index capacity, or lock timeout");
  std::error_code error;
  const bool created = std::filesystem::create_directories(directory, error);
  if (error) throw Error(ErrorCode::Io, "create database directory: " + error.message());
  if (created) {
    // Persist every new directory entry, including nested parents, before commit can succeed.
    auto current = std::filesystem::absolute(directory);
    for (;;) {
      const int fd = ::open(current.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
      if (fd < 0) throw Error(ErrorCode::Io, "open database parent directory: " + std::string(std::strerror(errno)));
      int result;
      do { result = ::fsync(fd); } while (result < 0 && errno == EINTR);
      const auto saved = errno;
      const int closed = ::close(fd);
      if (result < 0) throw Error(ErrorCode::Io, "sync database parent directory: " + std::string(std::strerror(saved)));
      if (closed < 0) throw Error(ErrorCode::Io, "close database parent directory");
      const auto parent = current.parent_path();
      if (parent == current || parent.empty()) break;
      current = parent;
    }
  }
  const bool heap_exists = std::filesystem::exists(directory / "records.db", error);
  if (error) throw Error(ErrorCode::Io, "inspect heap file: " + error.message());
  const bool index_exists = std::filesystem::exists(directory / "primary.idx", error);
  if (error) throw Error(ErrorCode::Io, "inspect index file: " + error.message());
  (void)heap_exists; (void)index_exists; // Bootstrap/recovery decides whether a missing file is repairable.
  return directory;
}
TransactionId next_transaction_id(TransactionId durable_max) {
  static std::atomic<TransactionId> next{1};
  auto current = next.load();
  for (;;) {
    if (current <= durable_max) {
      if (durable_max == std::numeric_limits<TransactionId>::max())
        throw Error(ErrorCode::InvalidTransactionState, "transaction ID space exhausted");
      if (!next.compare_exchange_weak(current, durable_max + 1)) continue;
      current = durable_max + 1;
    }
    if (current == std::numeric_limits<TransactionId>::max())
      throw Error(ErrorCode::InvalidTransactionState, "transaction ID space exhausted");
    if (next.compare_exchange_weak(current, current + 1)) return current;
  }
}
}  // namespace

namespace detail {
struct DatabaseState {
  DatabaseState(const std::filesystem::path& path, DatabaseOptions options)
      : directory(checked_directory(path, options)), wal(std::make_shared<Wal>(directory, options.buffer_frames, options.index_max_keys, options.fault_hook)),
        heap(directory / "records.db", options.buffer_frames),
        index(directory / "primary.idx", {options.buffer_frames, options.index_max_keys}), locks(options.lock_timeout) {
    (void)validate_storage();
    heap.attach_wal(wal); index.attach_wal(wal);
  }
  const std::filesystem::path directory;
  std::shared_ptr<Wal> wal;
  HeapFile heap;
  BPlusTree index;
  LockManager locks;
  mutable std::mutex lifecycle;
  mutable std::shared_mutex storage;
  std::size_t active = 0;
  std::atomic<bool> failed{false}, closed{false};

  void check_open() const {
    if (closed.load()) throw Error(ErrorCode::Closed, "transactional database is closed");
    if (failed.load()) throw Error(ErrorCode::Io, "database failed; resolve handles, stop using it and reopen (repair may be required)");
  }
  TreeValidation validate_storage() {
    const auto validation = index.validate();
    const auto records = heap.scan();
    if (records.size() != index.size()) throw Error(ErrorCode::Corruption, "heap/index entry counts disagree");
    for (const auto& [rid, bytes] : records) {
      const auto record = deserialize(bytes);
      if (index.find(record.key) != rid) throw Error(ErrorCode::Corruption, "heap key/RID disagrees with primary index");
    }
    return validation;
  }
  void begin_heap_undo() { heap.begin_undo(); }
  void begin_index_undo() { index.begin_undo(); }
  void commit_heap_undo() { heap.commit_undo(); }
  void commit_index_undo() { index.commit_undo(); }
  void rollback_heap_undo() { heap.rollback_undo(); }
  void rollback_index_undo() { index.rollback_undo(); }
};
}  // namespace detail

struct Transaction::Impl {
  Impl(std::shared_ptr<detail::DatabaseState> owner, TransactionId transaction)
      : db(std::move(owner)), id(transaction) {}
  std::shared_ptr<detail::DatabaseState> db;
  const TransactionId id;
  std::atomic<TransactionState> state{TransactionState::Active};
  std::mutex operation;
  bool writer = false, heap_undo = false, index_undo = false, registered = true;

  void require_active() const {
    if (state.load() != TransactionState::Active)
      throw Error(ErrorCode::InvalidTransactionState, "transaction is no longer active");
  }
  void finish() {
    if (!registered) return;
    db->wal->finish_writer(id);
    db->locks.release_all(id);
    std::lock_guard lock(db->lifecycle);
    --db->active;
    registered = false;
  }
  void abort() {
    state = TransactionState::Aborting;
    std::exception_ptr failure;
    try { db->wal->abort(id); } // Force the abort decision BEFORE any unlogged restoration.
    catch (...) { failure = std::current_exception(); db->failed = true; }
    if (!failure && (heap_undo || index_undo)) {
      std::unique_lock storage(db->storage);
      // Attempt both restorations even if the first file cannot be restored.
      if (index_undo) {
        try { db->rollback_index_undo(); index_undo = false; }
        catch (...) { failure = std::current_exception(); }
      }
      if (heap_undo) {
        try { db->rollback_heap_undo(); heap_undo = false; }
        catch (...) { if (!failure) failure = std::current_exception(); }
      }
      if (failure) db->failed = true;  // Publish before letting any other storage call proceed.
    }
    state = failure ? TransactionState::Failed : TransactionState::Aborted;
    finish();  // Strict locks release only after undo, or a fail-stop decision.
    if (failure) throw Error(ErrorCode::TransactionFailed, "transaction outcome requires recovery; database is unavailable");
  }
  void on_error() {
    const auto original = std::current_exception();
    try { std::rethrow_exception(original); }
    catch (const Error& error) {
      if (error.code() == ErrorCode::Io || error.code() == ErrorCode::Corruption) db->failed = true;
    } catch (...) {}
    abort();
    std::rethrow_exception(original);
  }
  template <typename Function> auto run(Function&& function) {
    std::lock_guard lock(operation);
    require_active();
    try { db->check_open(); return function(); }
    catch (...) { on_error(); throw; }
  }
  void read_locks(IndexKey key) {
    db->locks.acquire(id, kTable, LockMode::IS);
    db->locks.acquire(id, key_resource(key), LockMode::S);
  }
  void write_locks(IndexKey key) {
    if (!writer) { db->locks.acquire(id, kWriter, LockMode::X); writer = true; }
    db->locks.acquire(id, kTable, LockMode::IX);
    db->locks.acquire(id, key_resource(key), LockMode::X);
  }
  void ensure_undo() {  // Caller holds the database storage latch exclusively.
    if (heap_undo && index_undo) return;
    try {
      db->wal->start_writer(id);
      db->begin_heap_undo(); heap_undo = true;
      db->begin_index_undo(); index_undo = true;
    } catch (...) {
      // A baseline flush can fail before a journal exists for that file.
      db->failed = true;
      throw;
    }
  }
  std::optional<Record> read(IndexKey key) {
    const auto rid = db->index.find(key);
    if (!rid) return {};
    auto record = deserialize(db->heap.get(*rid));
    if (record.key != key) throw Error(ErrorCode::Corruption, "indexed heap record has a different key");
    return record;
  }
};

Transaction::Transaction(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
Transaction::~Transaction() { cleanup(); }
Transaction::Transaction(Transaction&&) noexcept = default;
Transaction& Transaction::operator=(Transaction&& other) noexcept {
  if (this != &other) { cleanup(); impl_ = std::move(other.impl_); }
  return *this;
}
Transaction::Impl& Transaction::implementation() const {
  if (!impl_) throw Error(ErrorCode::InvalidTransactionState, "transaction handle has been moved");
  return *impl_;
}
void Transaction::cleanup() noexcept {
  if (!impl_ || !impl_->registered) return;
  try { std::lock_guard lock(impl_->operation); impl_->abort(); }
  catch (...) { impl_->db->failed = true; }
}
TransactionId Transaction::id() const { return implementation().id; }
TransactionState Transaction::state() const { return implementation().state.load(); }
std::optional<Record> Transaction::get(IndexKey key) {
  auto& tx = implementation();
  return tx.run([&] {
    tx.read_locks(key);
    std::shared_lock storage(tx.db->storage);
    tx.db->check_open();
    return tx.read(key);
  });
}
std::optional<Record> Transaction::get_for_update(IndexKey key) {
  auto& tx = implementation();
  return tx.run([&] {
    tx.write_locks(key);
    std::unique_lock storage(tx.db->storage);
    tx.db->check_open(); tx.ensure_undo();
    return tx.read(key);
  });
}
void Transaction::insert(const Record& record) {
  auto& tx = implementation();
  tx.run([&] {
    const auto bytes = serialize(record);
    tx.write_locks(record.key);
    std::unique_lock storage(tx.db->storage);
    tx.db->check_open(); tx.ensure_undo();
    if (tx.db->index.find(record.key)) throw Error(ErrorCode::DuplicateKey, "primary key already exists");
    const auto rid = tx.db->heap.insert(bytes);
    if (!tx.db->index.insert(record.key, rid)) throw Error(ErrorCode::Corruption, "unexpected duplicate during indexed insert");
  });
}
void Transaction::update(IndexKey key, const Record& record) {
  auto& tx = implementation();
  tx.run([&] {
    if (record.key != key) throw Error(ErrorCode::InvalidArgument, "update cannot change the primary key");
    const auto bytes = serialize(record);
    tx.write_locks(key);
    std::unique_lock storage(tx.db->storage);
    tx.db->check_open(); tx.ensure_undo();
    const auto rid = tx.db->index.find(key);
    if (!rid) throw Error(ErrorCode::NotFound, "update key does not exist");
    tx.db->heap.update(*rid, bytes);
  });
}
void Transaction::erase(IndexKey key) {
  auto& tx = implementation();
  tx.run([&] {
    tx.write_locks(key);
    std::unique_lock storage(tx.db->storage);
    tx.db->check_open(); tx.ensure_undo();
    const auto rid = tx.db->index.find(key);
    if (!rid) throw Error(ErrorCode::NotFound, "delete key does not exist");
    tx.db->heap.erase(*rid);
    if (!tx.db->index.erase(key)) throw Error(ErrorCode::Corruption, "index entry disappeared during delete");
  });
}
std::vector<Record> Transaction::scan(std::optional<IndexKey> lower, std::optional<IndexKey> upper, std::size_t limit) {
  auto& tx = implementation();
  return tx.run([&] {
    if (lower && upper && *lower > *upper) throw Error(ErrorCode::InvalidArgument, "range lower bound exceeds upper bound");
    tx.db->locks.acquire(tx.id, kTable, LockMode::S);
    std::shared_lock storage(tx.db->storage);
    tx.db->check_open();
    std::vector<Record> result;
    for (const auto& [key, rid] : tx.db->index.scan(lower, upper, limit)) {
      auto record = deserialize(tx.db->heap.get(rid));
      if (record.key != key) throw Error(ErrorCode::Corruption, "range index/heap key mismatch");
      result.push_back(std::move(record));
    }
    return result;
  });
}
void Transaction::commit() {
  auto& tx = implementation();
  tx.run([&] {
    tx.state = TransactionState::Committing;
    if (tx.writer && tx.heap_undo && tx.index_undo) {
      std::unique_lock storage(tx.db->storage);
      tx.db->check_open();
      // All cache and allocator mutations already have WAL images. Force COMMIT
      // before acknowledging; dirty committed frames need not be written here.
      tx.db->wal->commit(tx.id);
      tx.db->commit_heap_undo(); tx.heap_undo = false;
      tx.db->commit_index_undo(); tx.index_undo = false;
    } else tx.db->wal->commit(tx.id);
    tx.state = TransactionState::Committed;
    tx.finish();
  });
}
void Transaction::rollback() {
  auto& tx = implementation();
  std::lock_guard lock(tx.operation);
  if (tx.state.load() == TransactionState::Aborted) return;
  tx.require_active();
  tx.abort();
}
TransactionalDatabase::TransactionalDatabase(const std::filesystem::path& directory, DatabaseOptions options)
    : state_(std::make_shared<detail::DatabaseState>(directory, options)) {}
Transaction TransactionalDatabase::begin() {
  std::lock_guard lock(state_->lifecycle);
  state_->check_open();
  const auto id = next_transaction_id(state_->wal->max_transaction_id());
  auto impl = std::make_unique<Transaction::Impl>(state_, id);
  try { state_->wal->begin(id); } catch (...) { state_->failed = true; throw; }
  state_->locks.register_transaction(id);
  ++state_->active;
  return Transaction(std::move(impl));
}
TreeValidation TransactionalDatabase::validate() const {
  std::lock_guard lock(state_->lifecycle);
  state_->check_open();
  if (state_->active != 0) throw Error(ErrorCode::Busy, "complete transactions before validating the database");
  std::shared_lock storage(state_->storage);
  return state_->validate_storage();
}
LockStats TransactionalDatabase::lock_stats() const { return state_->locks.stats(); }
WalStats TransactionalDatabase::wal_stats() const { return state_->wal->stats(); }
void TransactionalDatabase::sync() {
  std::lock_guard lock(state_->lifecycle); state_->check_open();
  if (state_->active != 0) throw Error(ErrorCode::Busy, "complete transactions before syncing the database");
  std::unique_lock storage(state_->storage);
  try { state_->heap.sync(); state_->index.sync(); } catch (...) { state_->failed = true; throw; }
}
void TransactionalDatabase::close() {
  std::lock_guard lock(state_->lifecycle);
  if (state_->closed.load()) return;
  state_->check_open();
  if (state_->active != 0) throw Error(ErrorCode::Busy, "complete transactions before closing the database");
  std::unique_lock storage(state_->storage);
  try { state_->heap.close(); state_->index.close(); state_->wal->close(); state_->closed = true; }
  catch (...) { state_->failed = true; throw; }
}
}  // namespace tinydb
