#include "buffer/buffer_pool.h"

#include <atomic>
#include <unordered_map>
#include <utility>

#include "common/encoding.h"
#include "recovery/wal.h"

namespace tinydb::detail {
struct Frame {
  Page page;
  PageId id = kInvalidPageId;
  std::size_t pins = 0;
  bool referenced = false;
  std::atomic<bool> dirty{false};
  std::shared_mutex latch;
};

struct BufferPoolState {
  BufferPoolState(DiskManager& disk_manager, std::size_t count, BufferPool::ForceWal callback)
      : disk(disk_manager), capacity(count), frames(std::make_unique<Frame[]>(count)),
        force_wal(std::move(callback)) { table.reserve(count); }
  DiskManager& disk;
  const std::size_t capacity;
  std::unique_ptr<Frame[]> frames;
  BufferPool::ForceWal force_wal;
  std::shared_ptr<Wal> wal;
  FileId file_id = 0;
  mutable std::mutex mutex;
  std::unordered_map<PageId, std::size_t> table;
  std::size_t hand = 0;
  bool closed = false, failed = false;
  BufferPoolStats counters;

  void check_open() const {
    if (closed) throw Error(ErrorCode::Closed, "buffer pool is closed");
    if (failed) throw Error(ErrorCode::Io, "buffer pool failed; stop using it and reopen");
  }
  void unpin(std::size_t index) noexcept {
    std::lock_guard lock(mutex);
    --frames[index].pins;
  }
  // All methods below run with mutex held. No page latch is acquired under it.
  // Unpinned frames cannot have a latch owner: guards unlock BEFORE unpinning.
  void flush_frame(std::size_t index) {
    auto& frame = frames[index];
    if (!frame.dirty.load(std::memory_order_relaxed)) return;
    const auto lsn = encoding::load<Lsn>(frame.page.bytes, 16);
    if (lsn != 0) {
      if (!force_wal) throw Error(ErrorCode::InvalidArgument, "nonzero page LSN requires a WAL force callback");
      force_wal(lsn);  // A throw retains the dirty image and permits retry.
    }
    try { disk.write_page(frame.page); }
    catch (...) { failed = true; throw; }
    frame.dirty.store(false, std::memory_order_relaxed);
    ++counters.page_writes;
  }
  std::size_t victim() {
    // Prefer empty frames without disturbing CLOCK reference bits.
    for (std::size_t i = 0; i < capacity; ++i)
      if (frames[i].id == kInvalidPageId) return i;
    // Two complete passes clear reference bits, then select an unpinned frame.
    for (std::size_t pass = 0; pass < 2; ++pass) {
      for (std::size_t i = 0; i < capacity; ++i) {
        const auto index = hand;
        hand = (hand + 1) % capacity;
        auto& frame = frames[index];
        if (frame.pins != 0) continue;
        if (frame.referenced) { frame.referenced = false; continue; }
        return index;
      }
    }
    throw Error(ErrorCode::Busy, "buffer pool exhausted: all frames are pinned");
  }
  void install(std::size_t index, Page page) {
    auto& frame = frames[index];
    // emplace may allocate; keep the old mapping intact until it succeeds.
    table.emplace(page.id(), index);
    if (frame.id != kInvalidPageId) {
      table.erase(frame.id);
      ++counters.evictions;
    }
    frame.page = std::move(page);
    frame.id = frame.page.id();
    frame.pins = 1;
    frame.referenced = true;
    frame.dirty.store(false, std::memory_order_relaxed);
  }
  std::size_t pin(PageId id) {
    std::lock_guard lock(mutex);
    check_open();
    if (const auto found = table.find(id); found != table.end()) {
      auto& frame = frames[found->second];
      ++frame.pins;
      frame.referenced = true;
      ++counters.hits;
      return found->second;
    }
    ++counters.misses;
    const auto index = victim();
    // Read/validate before evicting, so an invalid fetch cannot evict a valid page.
    const Page incoming = disk.read_page(id);
    ++counters.disk_reads;
    flush_frame(index);
    install(index, incoming);
    return index;
  }
  void require_unpinned() const {
    for (std::size_t i = 0; i < capacity; ++i)
      if (frames[i].pins != 0) throw Error(ErrorCode::Busy, "release page guards before flushing or closing");
  }
  void flush_all_unlocked() {
    require_unpinned();
    for (std::size_t i = 0; i < capacity; ++i) flush_frame(i);
  }
  void sync_unlocked() {
    flush_all_unlocked();
    try { disk.sync(); }
    catch (...) { failed = true; throw; }
  }
};
}  // namespace tinydb::detail

namespace tinydb {
ReadPageGuard::ReadPageGuard(std::shared_ptr<detail::BufferPoolState> state, std::size_t frame)
    : state_(std::move(state)), frame_(frame), latch_(state_->frames[frame_].latch) {}
ReadPageGuard::~ReadPageGuard() { drop(); }
ReadPageGuard::ReadPageGuard(ReadPageGuard&& other) noexcept
    : state_(std::move(other.state_)), frame_(other.frame_), latch_(std::move(other.latch_)) {}
ReadPageGuard& ReadPageGuard::operator=(ReadPageGuard&& other) noexcept {
  if (this != &other) {
    drop();
    state_ = std::move(other.state_);
    frame_ = other.frame_;
    latch_ = std::move(other.latch_);
  }
  return *this;
}
const Page& ReadPageGuard::page() const {
  if (!state_) throw Error(ErrorCode::Closed, "read guard is empty");
  return state_->frames[frame_].page;
}
void ReadPageGuard::drop() noexcept {
  if (!state_) return;
  latch_.unlock();
  state_->unpin(frame_);
  state_.reset();
}

WritePageGuard::WritePageGuard(std::shared_ptr<detail::BufferPoolState> state, std::size_t frame)
    : state_(std::move(state)), frame_(frame), latch_(state_->frames[frame_].latch) {}
WritePageGuard::~WritePageGuard() { drop(); }
WritePageGuard::WritePageGuard(WritePageGuard&& other) noexcept
    : state_(std::move(other.state_)), frame_(other.frame_), latch_(std::move(other.latch_)) {}
WritePageGuard& WritePageGuard::operator=(WritePageGuard&& other) noexcept {
  if (this != &other) {
    drop();
    state_ = std::move(other.state_);
    frame_ = other.frame_;
    latch_ = std::move(other.latch_);
  }
  return *this;
}
const Page& WritePageGuard::page() const {
  if (!state_) throw Error(ErrorCode::Closed, "write guard is empty");
  return state_->frames[frame_].page;
}
void WritePageGuard::replace(Page image, Lsn lsn) {
  (void)page();  // Check moved/dropped guard before dereferencing state_.
  auto& frame = state_->frames[frame_];
  if (image.id() != frame.id || !is_data_page_type(image.type()) || image.type() != frame.page.type() ||
      (!state_->wal && lsn < encoding::load<Lsn>(frame.page.bytes, 16)))
    throw Error(ErrorCode::InvalidArgument, "replacement must preserve page identity and not decrease its LSN");
  encoding::store<Lsn>(image.bytes, 16, lsn);
  image.seal();
  image.validate(frame.id);
  if (state_->wal) {
    if (lsn != 0) throw Error(ErrorCode::InvalidArgument, "WAL owns page LSN assignment");
    image = state_->wal->log_page(state_->file_id, frame.page, image);
  }
  frame.page = std::move(image);
  frame.dirty.store(true, std::memory_order_relaxed);
}
void WritePageGuard::drop() noexcept {
  if (!state_) return;
  latch_.unlock();
  state_->unpin(frame_);
  state_.reset();
}

BufferPool::BufferPool(DiskManager& disk, std::size_t frames, ForceWal force_wal) {
  if (frames == 0) throw Error(ErrorCode::InvalidArgument, "buffer pool needs at least one frame");
  state_ = std::make_shared<detail::BufferPoolState>(disk, frames, std::move(force_wal));
}
BufferPool::~BufferPool() = default;
ReadPageGuard BufferPool::fetch_read(PageId id) {
  const auto index = state_->pin(id);
  try { return ReadPageGuard(state_, index); }
  catch (...) { state_->unpin(index); throw; }
}
WritePageGuard BufferPool::fetch_write(PageId id) {
  const auto index = state_->pin(id);
  try { return WritePageGuard(state_, index); }
  catch (...) { state_->unpin(index); throw; }
}
WritePageGuard BufferPool::new_page(PageType type) {
  std::size_t index;
  {
    std::lock_guard lock(state_->mutex);
    state_->check_open();
    if (!is_data_page_type(type))
      throw Error(ErrorCode::InvalidArgument, "allocation requires a supported data-page type");
    index = state_->victim();  // Fail before allocating anything if exhausted.
    state_->flush_frame(index);
    PageId id;
    try { id = state_->disk.allocate_page(type); }
    catch (const Error& error) { if (error.code() == ErrorCode::Io) state_->failed = true; throw; }
    state_->install(index, state_->disk.read_page(id)); // Retain allocator-assigned page LSN.
  }
  try { return WritePageGuard(state_, index); }
  catch (...) { state_->unpin(index); throw; }
}
void BufferPool::delete_page(PageId id) {
  std::lock_guard lock(state_->mutex);
  state_->check_open();
  const auto found = state_->table.find(id);
  if (found != state_->table.end() && state_->frames[found->second].pins != 0)
    throw Error(ErrorCode::Busy, "cannot free a pinned page");
  try { state_->disk.deallocate_page(id); }
  catch (const Error& error) { if (error.code() == ErrorCode::Io) state_->failed = true; throw; }
  if (found != state_->table.end()) {
    auto& frame = state_->frames[found->second];
    frame.id = kInvalidPageId;
    frame.dirty.store(false, std::memory_order_relaxed);
    frame.referenced = false;
    state_->table.erase(found);
  }
}
void BufferPool::flush_page(PageId id) {
  std::lock_guard lock(state_->mutex);
  state_->check_open();
  const auto found = state_->table.find(id);
  if (found == state_->table.end()) { (void)state_->disk.read_page(id); return; }
  if (state_->frames[found->second].pins != 0)
    throw Error(ErrorCode::Busy, "cannot flush a pinned page");
  state_->flush_frame(found->second);
}
void BufferPool::flush_all() {
  std::lock_guard lock(state_->mutex);
  state_->check_open();
  state_->flush_all_unlocked();
}
void BufferPool::sync() {
  std::lock_guard lock(state_->mutex);
  state_->check_open();
  state_->sync_unlocked();
}
void BufferPool::close() {
  std::lock_guard lock(state_->mutex);
  if (state_->closed) return;
  state_->check_open();
  state_->sync_unlocked();
  state_->closed = true;
}
BufferPoolStats BufferPool::stats() const {
  std::lock_guard lock(state_->mutex);
  auto result = state_->counters;
  result.capacity = state_->capacity;
  result.resident_pages = state_->table.size();
  for (std::size_t i = 0; i < state_->capacity; ++i) {
    const auto& frame = state_->frames[i];
    result.pins += frame.pins;
    result.pinned_pages += frame.pins != 0 ? 1U : 0U;
    result.dirty_pages += frame.dirty.load(std::memory_order_relaxed) ? 1U : 0U;
  }
  return result;
}
void BufferPool::discard_all() {
  std::lock_guard lock(state_->mutex);
  if (state_->closed) throw Error(ErrorCode::Closed, "buffer pool is closed");
  state_->require_unpinned();
  for (std::size_t i = 0; i < state_->capacity; ++i) {
    auto& frame = state_->frames[i];
    frame.id = kInvalidPageId;
    frame.referenced = false;
    frame.dirty.store(false, std::memory_order_relaxed);
  }
  state_->table.clear();
  state_->hand = 0;
  state_->failed = false;
}
void BufferPool::attach_wal(std::shared_ptr<Wal> wal, std::uint32_t file_id) {
  std::lock_guard lock(state_->mutex); state_->check_open(); state_->require_unpinned();
  if (state_->wal || !wal) throw Error(ErrorCode::InvalidArgument, "invalid buffer WAL attachment");
  for (std::size_t i = 0; i < state_->capacity; ++i)
    if (state_->frames[i].dirty.load()) throw Error(ErrorCode::Busy, "flush cache before WAL attachment");
  state_->wal = wal; state_->file_id = file_id;
  state_->force_wal = [wal = std::move(wal)](Lsn lsn) { wal->force(lsn); };
}
std::vector<PageId> BufferPool::allocated_pages() const {
  std::lock_guard lock(state_->mutex);
  state_->check_open();
  return state_->disk.allocated_pages();
}
PageId BufferPool::page_count() const {
  std::lock_guard lock(state_->mutex);
  state_->check_open();
  return state_->disk.page_count();
}
std::uint64_t BufferPool::reserve_generation() {
  std::lock_guard lock(state_->mutex);
  state_->check_open();
  try { return state_->disk.reserve_generation(); }
  catch (const Error& error) { if (error.code() == ErrorCode::Io) state_->failed = true; throw; }
}
}  // namespace tinydb
