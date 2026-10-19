#pragma once

#include <chrono>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>

namespace tinydb {
using TransactionId = std::uint64_t;
enum class LockMode { IS, IX, S, SIX, X };
struct LockResource {
  enum class Kind { Writer, Table, Key };
  Kind kind = Kind::Table;
  std::uint64_t table = 0;
  std::int64_t key = 0;
  auto operator<=>(const LockResource&) const = default;
};
struct LockStats {
  std::size_t active_transactions = 0, resources = 0, grants = 0, waiters = 0;
  std::uint64_t timeouts = 0;
};

// Strict 2PL primitive: no individual unlock, only release_all at completion.
// A timeout removes the pending request but retains existing grants; the
// transaction coordinator must undo and call release_all before continuing.
class LockManager {
 public:
  explicit LockManager(std::chrono::milliseconds timeout = std::chrono::seconds(1));
  ~LockManager();
  LockManager(const LockManager&) = delete;
  LockManager& operator=(const LockManager&) = delete;
  void register_transaction(TransactionId id);
  void acquire(TransactionId id, LockResource resource, LockMode mode);
  void release_all(TransactionId id);
  [[nodiscard]] LockStats stats() const;
  [[nodiscard]] std::optional<LockMode> held_mode(TransactionId id, LockResource resource) const;
  [[nodiscard]] static bool compatible(LockMode a, LockMode b);
 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
}  // namespace tinydb
