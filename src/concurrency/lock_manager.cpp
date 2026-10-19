#include "concurrency/lock_manager.h"

#include <algorithm>
#include <condition_variable>
#include <list>
#include <map>
#include <mutex>

#include "common/error.h"

namespace tinydb {
namespace {
LockMode combine(LockMode held, LockMode requested) {
  if (held == requested) return held;
  if (held == LockMode::X || requested == LockMode::X) return LockMode::X;
  if (held == LockMode::IS) return requested;
  if (requested == LockMode::IS) return held;
  return LockMode::SIX;  // S + IX, or a mode already containing both.
}
bool valid_mode(LockMode mode) { return mode >= LockMode::IS && mode <= LockMode::X; }
}  // namespace
bool LockManager::compatible(LockMode a, LockMode b) {
  if (!valid_mode(a) || !valid_mode(b)) throw Error(ErrorCode::InvalidArgument, "invalid lock mode");
  constexpr bool matrix[5][5] = {
    {true, true, true, true, false},
    {true, true, false, false, false},
    {true, false, true, false, false},
    {true, false, false, false, false},
    {false, false, false, false, false}
  };
  return matrix[static_cast<int>(a)][static_cast<int>(b)];
}
struct LockManager::Impl {
  struct Registration { bool active = true; };
  struct Request { TransactionId id; LockMode mode; };
  struct Entry {
    std::map<TransactionId, LockMode> holders;
    std::list<const Request*> queue;  // Stack requests live until acquire returns, under mutex.
  };
  explicit Impl(std::chrono::milliseconds wait) : timeout(wait) {}
  const std::chrono::milliseconds timeout;
  mutable std::mutex mutex;
  std::condition_variable changed;
  std::map<TransactionId, std::shared_ptr<Registration>> transactions;
  std::map<LockResource, std::shared_ptr<Entry>> entries;
  std::uint64_t timeouts = 0;

  bool can_grant(const Entry& entry, TransactionId id, LockMode mode) const {
    for (const auto& [owner, held] : entry.holders)
      if (owner != id && !LockManager::compatible(held, mode)) return false;
    return true;
  }
  void prune(LockResource resource, const std::shared_ptr<Entry>& entry) {
    if (entry->holders.empty() && entry->queue.empty()) {
      const auto found = entries.find(resource);
      if (found != entries.end() && found->second == entry) entries.erase(found);
    }
  }
};
LockManager::LockManager(std::chrono::milliseconds timeout) {
  if (timeout.count() <= 0) throw Error(ErrorCode::InvalidArgument, "lock timeout must be positive");
  impl_ = std::make_unique<Impl>(timeout);
}
LockManager::~LockManager() = default;
void LockManager::register_transaction(TransactionId id) {
  std::lock_guard lock(impl_->mutex);
  if (id == 0 || impl_->transactions.contains(id))
    throw Error(ErrorCode::InvalidArgument, "invalid or already active transaction ID");
  impl_->transactions.emplace(id, std::make_shared<Impl::Registration>());
}
void LockManager::acquire(TransactionId id, LockResource resource, LockMode mode) {
  if (!valid_mode(mode)) throw Error(ErrorCode::InvalidArgument, "invalid lock mode");
  const auto started = std::chrono::steady_clock::now();
  const auto available = std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::time_point::max() - started);
  const auto deadline = impl_->timeout >= available
      ? std::chrono::steady_clock::time_point::max() : started + impl_->timeout;
  std::unique_lock lock(impl_->mutex);
  const auto transaction = impl_->transactions.find(id);
  if (transaction == impl_->transactions.end()) throw Error(ErrorCode::TransactionAborted, "transaction is not registered");
  const auto registration = transaction->second;
  auto [found, inserted] = impl_->entries.try_emplace(resource, std::make_shared<Impl::Entry>());
  (void)inserted;
  const auto entry = found->second;
  auto target = mode;
  if (const auto held = entry->holders.find(id); held != entry->holders.end()) {
    target = combine(held->second, mode);
    if (target == held->second) return;
  }
  if (entry->queue.empty() && impl_->can_grant(*entry, id, target)) {
    try { entry->holders.insert_or_assign(id, target); }
    catch (...) { impl_->prune(resource, entry); throw; }
    return;
  }
  const Impl::Request request{id, target};
  entry->queue.push_back(&request);
  try {
    const auto ready = [&] {
      return !registration->active ||
             (!entry->queue.empty() && entry->queue.front() == &request && impl_->can_grant(*entry, id, target));
    };
    if (!impl_->changed.wait_until(lock, deadline, ready)) {
      ++impl_->timeouts;
      throw Error(ErrorCode::LockTimeout, "lock acquisition timed out; transaction must roll back");
    }
    if (!registration->active) throw Error(ErrorCode::TransactionAborted, "transaction canceled while waiting for a lock");
    entry->holders.insert_or_assign(id, target);
    entry->queue.remove(&request);
    impl_->changed.notify_all();
  } catch (...) {
    entry->queue.remove(&request);
    impl_->prune(resource, entry);
    impl_->changed.notify_all();
    throw;
  }
}
void LockManager::release_all(TransactionId id) {
  std::lock_guard lock(impl_->mutex);
  const auto transaction = impl_->transactions.find(id);
  if (transaction == impl_->transactions.end()) return;
  transaction->second->active = false;
  impl_->transactions.erase(transaction);
  for (auto it = impl_->entries.begin(); it != impl_->entries.end();) {
    auto& entry = *it->second;
    entry.holders.erase(id);
    entry.queue.remove_if([&](const Impl::Request* request) { return request->id == id; });
    if (entry.holders.empty() && entry.queue.empty()) it = impl_->entries.erase(it);
    else ++it;
  }
  impl_->changed.notify_all();
}
LockStats LockManager::stats() const {
  std::lock_guard lock(impl_->mutex);
  LockStats result;
  result.active_transactions = impl_->transactions.size();
  result.resources = impl_->entries.size();
  result.timeouts = impl_->timeouts;
  for (const auto& [resource, entry] : impl_->entries) {
    (void)resource;
    result.grants += entry->holders.size();
    result.waiters += entry->queue.size();
  }
  return result;
}
std::optional<LockMode> LockManager::held_mode(TransactionId id, LockResource resource) const {
  std::lock_guard lock(impl_->mutex);
  const auto found = impl_->entries.find(resource);
  if (found == impl_->entries.end()) return {};
  const auto held = found->second->holders.find(id);
  return held == found->second->holders.end() ? std::optional<LockMode>{} : std::optional<LockMode>{held->second};
}
}  // namespace tinydb
