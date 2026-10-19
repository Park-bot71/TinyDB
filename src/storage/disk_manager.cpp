#include "storage/disk_manager.h"

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <limits>
#include <unordered_map>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

#include "common/encoding.h"
#include "recovery/wal.h"

namespace tinydb {
struct DiskManager::UndoState {
  PageId baseline_pages;
  std::unordered_map<PageId, Page> before;
};
static_assert(sizeof(off_t) >= 8, "tinydb requires 64-bit file offsets");
namespace {
[[noreturn]] void io_error(const std::string& operation) {
  const int saved = errno;
  throw Error(ErrorCode::Io, operation + ": " + std::strerror(saved));
}
void sync_fd(int fd) {
  int result;
  do { result = ::fsync(fd); } while (result < 0 && errno == EINTR);
  if (result < 0) io_error("fsync");
}
}  // namespace

DiskManager::DiskManager(const std::filesystem::path& path) {
  bool created = true;
  fd_ = ::open(path.c_str(), O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
  if (fd_ < 0 && errno == EEXIST) {
    created = false;
    fd_ = ::open(path.c_str(), O_RDWR | O_CLOEXEC);
  }
  if (fd_ < 0) io_error("open database " + path.string());
  try {
    if (::flock(fd_, LOCK_EX | LOCK_NB) < 0) {
      if (errno == EWOULDBLOCK) throw Error(ErrorCode::Busy, "database already open by another owner");
      io_error("lock database");
    }
    struct stat info {};
    if (::fstat(fd_, &info) < 0) io_error("stat database");
    if (!S_ISREG(info.st_mode)) throw Error(ErrorCode::InvalidArgument, "database must be a regular file");
    if (created) {
      write_metadata();
      sync_unlocked();
      const auto parent = path.has_parent_path() ? path.parent_path() : std::filesystem::path(".");
      const int dir = ::open(parent.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
      if (dir < 0) io_error("open database directory");
      try { sync_fd(dir); } catch (...) { ::close(dir); throw; }
      if (::close(dir) < 0) io_error("close database directory");
    } else {
      load_existing();
    }
  } catch (...) {
    ::close(fd_);
    fd_ = -1;
    throw;
  }
}
void DiskManager::load_existing() {
  struct stat info {};
  if (::fstat(fd_, &info) < 0) io_error("stat database");
  if (info.st_size < static_cast<off_t>(kPageSize) || info.st_size % static_cast<off_t>(kPageSize) != 0)
    throw Error(ErrorCode::Corruption, "database size is not a whole number of pages");
  const Page meta = read_raw(0);
  free_pages_.clear();
  page_count_ = encoding::load<PageId>(meta.bytes, 24);
  free_head_ = encoding::load<PageId>(meta.bytes, 28);
  next_generation_ = encoding::load<std::uint64_t>(meta.bytes, 32);
  if (page_count_ == 0 || page_count_ == kInvalidPageId || next_generation_ == 0 ||
      static_cast<std::uint64_t>(info.st_size) != static_cast<std::uint64_t>(page_count_) * kPageSize)
    throw Error(ErrorCode::Corruption, "invalid allocation metadata or file length");
  PageId cursor = free_head_;
  while (cursor != kInvalidPageId) {
    if (cursor == 0 || cursor >= page_count_ || !free_pages_.insert(cursor).second)
      throw Error(ErrorCode::Corruption, "invalid or cyclic free-page chain");
    const Page page = read_raw(cursor);
    if (page.type() != PageType::Free || encoding::load<std::uint32_t>(page.bytes, 28) != 0)
      throw Error(ErrorCode::Corruption, "invalid free-page header");
    cursor = encoding::load<PageId>(page.bytes, 24);
  }
  // Detect orphan free pages as well as corrupt allocated page headers.
  page_types_.resize(page_count_);
  for (PageId id = 1; id < page_count_; ++id) {
    const Page page = read_raw(id);
    page_types_[id] = page.type();
    if ((page.type() == PageType::Free) != free_pages_.contains(id))
      throw Error(ErrorCode::Corruption, "free-page chain disagrees with page state");
  }
}

DiskManager::~DiskManager() { if (fd_ >= 0) ::close(fd_); }
void DiskManager::check_open() const {
  if (fd_ < 0) throw Error(ErrorCode::Closed, "database is closed");
  if (failed_) throw Error(ErrorCode::Io, "database handle failed; stop using it and reopen");
}
void DiskManager::check_allocated(PageId id) const {
  check_open();
  if (id == 0 || id >= page_count_ || free_pages_.contains(id))
    throw Error(ErrorCode::NotFound, "page is not allocated");
}
Page DiskManager::read_raw(PageId id) const {
  Page page;
  std::size_t done = 0;
  while (done < kPageSize) {
    const auto n = ::pread(fd_, page.bytes.data() + done, kPageSize - done,
                           static_cast<off_t>(id) * static_cast<off_t>(kPageSize) + static_cast<off_t>(done));
    if (n < 0) { if (errno == EINTR) continue; io_error("read page"); }
    if (n == 0) throw Error(ErrorCode::Corruption, "unexpected end of database page");
    done += static_cast<std::size_t>(n);
  }
  page.validate(id);
  return page;
}
void DiskManager::write_raw(Page page, bool logged) {
  std::size_t done = 0;
  try {
    if (undo_ && page.id() < undo_->baseline_pages && !undo_->before.contains(page.id()))
      undo_->before.emplace(page.id(), read_raw(page.id()));
    if (wal_ && !restoring_) {
      if (!logged) {
        struct stat info {};
        if (::fstat(fd_, &info) < 0) io_error("stat before WAL page write");
        std::optional<Page> before;
        if (static_cast<std::uint64_t>(info.st_size) >= (static_cast<std::uint64_t>(page.id()) + 1) * kPageSize)
          before = read_raw(page.id());
        page = wal_->log_page(file_id_, before, page);
      }
      const auto lsn = encoding::load<std::uint64_t>(page.bytes, 16);
      if (lsn == 0) throw Error(ErrorCode::InvalidArgument, "unlogged page cannot reach WAL-backed storage");
      wal_->force(lsn);
    }
    while (done < kPageSize) {
      const auto amount = wal_ && done == 0 ? kPageSize / 2 : kPageSize - done;
      const auto n = ::pwrite(fd_, page.bytes.data() + done, amount,
                              static_cast<off_t>(page.id()) * static_cast<off_t>(kPageSize) + static_cast<off_t>(done));
      if (n < 0) { if (errno == EINTR) continue; io_error("write page"); }
      if (n == 0) throw Error(ErrorCode::Io, "zero-length page write");
      done += static_cast<std::size_t>(n);
      if (wal_ && done == kPageSize / 2)
        wal_->fault(file_id_ == kHeapFileId ? "data.heap.after_half_write" : "data.index.after_half_write");
    }
  } catch (...) { failed_ = true; throw; }
}
void DiskManager::write_metadata() {
  Page meta = Page::create(0, PageType::Metadata);
  encoding::store<PageId>(meta.bytes, 24, page_count_);
  encoding::store<PageId>(meta.bytes, 28, free_head_);
  encoding::store<std::uint64_t>(meta.bytes, 32, next_generation_);
  meta.seal();
  write_raw(meta);
}
Page DiskManager::read_page(PageId id) const {
  std::lock_guard lock(mutex_);
  check_allocated(id);
  return read_raw(id);
}
void DiskManager::write_page(Page page) {
  std::lock_guard lock(mutex_);
  check_allocated(page.id());
  // Callers may modify bytes, but cannot bypass allocator ownership of page type.
  if (!is_data_page_type(page.type()) || page.type() != page_types_[page.id()])
    throw Error(ErrorCode::InvalidArgument, "page writes must preserve the allocated data-page type");
  page.seal();
  page.validate(page.id());
  write_raw(page, true);
}
PageId DiskManager::allocate_page(PageType type) {
  std::lock_guard lock(mutex_);
  check_open();
  if (!is_data_page_type(type))
    throw Error(ErrorCode::InvalidArgument, "allocation requires a supported data-page type");
  PageId id = free_head_;
  PageId next = kInvalidPageId;
  if (id == kInvalidPageId) {
    if (page_count_ == kInvalidPageId - 1)
      throw Error(ErrorCode::InvalidArgument, "page ID space exhausted");
    id = page_count_;
  } else {
    next = encoding::load<PageId>(read_raw(id).bytes, 24);
  }
  if (id == page_count_) page_types_.push_back(type);  // Allocate memory before I/O.
  write_raw(Page::create(id, type));
  page_types_[id] = type;
  if (id == page_count_) ++page_count_;
  else { free_pages_.erase(id); free_head_ = next; }
  write_metadata();
  return id;
}
void DiskManager::deallocate_page(PageId id) {
  std::lock_guard lock(mutex_);
  check_allocated(id);
  // Allocate the set entry before the first disk mutation (bad_alloc is harmless).
  free_pages_.insert(id);
  Page page = Page::create(id, PageType::Free);
  encoding::store<PageId>(page.bytes, 24, free_head_);
  page.seal();
  write_raw(page);
  page_types_[id] = PageType::Free;
  free_head_ = id;
  write_metadata();
}
std::uint64_t DiskManager::reserve_generation() {
  std::lock_guard lock(mutex_);
  check_open();
  if (next_generation_ == std::numeric_limits<std::uint64_t>::max())
    throw Error(ErrorCode::InvalidArgument, "record generation space exhausted");
  const auto result = next_generation_++;
  write_metadata();
  return result;
}
std::vector<PageId> DiskManager::allocated_pages() const {
  std::lock_guard lock(mutex_);
  check_open();
  std::vector<PageId> result;
  for (PageId id = 1; id < page_count_; ++id)
    if (!free_pages_.contains(id)) result.push_back(id);
  return result;
}
PageId DiskManager::page_count() const {
  std::lock_guard lock(mutex_);
  check_open();
  return page_count_;
}
void DiskManager::sync_unlocked() {
  try { sync_fd(fd_); } catch (...) { failed_ = true; throw; }
}
void DiskManager::sync() {
  std::lock_guard lock(mutex_);
  check_open();
  sync_unlocked();
}
void DiskManager::close() {
  std::lock_guard lock(mutex_);
  if (fd_ < 0) return;
  if (undo_) throw Error(ErrorCode::Busy, "resolve active storage undo before closing");
  check_open();
  sync_unlocked();
  const int old_fd = fd_;
  fd_ = -1;
  // On Linux close releases the descriptor even on error: never retry close.
  if (::close(old_fd) < 0) io_error("close database");
}
void DiskManager::begin_undo() {
  std::lock_guard lock(mutex_);
  check_open();
  if (undo_) throw Error(ErrorCode::Busy, "storage undo already active");
  undo_ = std::make_unique<UndoState>(UndoState{page_count_, {}});
}
void DiskManager::commit_undo() {
  std::lock_guard lock(mutex_);
  check_open();
  if (!undo_) throw Error(ErrorCode::InvalidTransactionState, "no active storage undo");
  undo_.reset();
}
void DiskManager::rollback_undo() {
  std::lock_guard lock(mutex_);
  if (fd_ < 0) throw Error(ErrorCode::Closed, "database is closed");
  if (!undo_) throw Error(ErrorCode::InvalidTransactionState, "no active storage undo");
  if (wal_ && !wal_->rollback_allowed()) throw Error(ErrorCode::Io, "durable ABORT required before physical undo");
  auto saved = std::move(undo_);  // Disable capture during restoration.
  restoring_ = true;
  const auto high_water = next_generation_;
  try {
    // No cache/client may access the file until all images and metadata are restored.
    for (const auto& [id, page] : saved->before) { (void)id; write_raw(page); }
    int result;
    do { result = ::ftruncate(fd_, static_cast<off_t>(saved->baseline_pages) * static_cast<off_t>(kPageSize)); }
    while (result < 0 && errno == EINTR);
    if (result < 0) io_error("truncate during storage undo");
    load_existing();
    // Never recycle a generation issued to an aborted insert.
    if (next_generation_ < high_water) { next_generation_ = high_water; write_metadata(); }
    failed_ = false;
    restoring_ = false;
  } catch (...) { failed_ = true; restoring_ = false; undo_ = std::move(saved); throw; }
}
void DiskManager::attach_wal(std::shared_ptr<Wal> wal, std::uint32_t file_id) {
  std::lock_guard lock(mutex_); check_open();
  if (undo_ || wal_ || !wal || (file_id != kHeapFileId && file_id != kIndexFileId))
    throw Error(ErrorCode::InvalidArgument, "invalid WAL attachment");
  wal_ = std::move(wal); file_id_ = file_id;
}
}  // namespace tinydb
