#include "recovery/wal.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <map>
#include <limits>
#include <mutex>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

#include "common/checksum.h"
#include "common/encoding.h"
#include "index/b_plus_tree.h"
#include "storage/heap_file.h"
#include "storage/record.h"

namespace tinydb {
namespace {
constexpr std::size_t kWalHeader = 64, kFrameHeader = 80;
constexpr std::uint32_t kWalMagic = 0x314c5754, kFrameMagic = 0x31525754;
using Bytes = std::vector<std::byte>;
[[noreturn]] void io(const std::string& operation) {
  const auto saved = errno;
  throw Error(ErrorCode::Io, operation + ": " + std::strerror(saved));
}
struct Fd {
  int fd = -1;
  explicit Fd(const std::filesystem::path& path, int flags) {
    fd = ::open(path.c_str(), flags | O_CLOEXEC, 0600);
    if (fd < 0) io("open " + path.string());
  }
  ~Fd() { if (fd >= 0) ::close(fd); }
  Fd(const Fd&) = delete;
  Fd& operator=(const Fd&) = delete;
};
void force_fd(int fd) {
  int result;
  do { result = ::fsync(fd); } while (result < 0 && errno == EINTR);
  if (result < 0) io("fsync WAL/recovery file");
}
std::uint64_t length(int fd) {
  struct stat st {};
  if (::fstat(fd, &st) < 0) io("stat WAL/recovery file");
  if (!S_ISREG(st.st_mode) || st.st_size < 0) throw Error(ErrorCode::Corruption, "expected regular recovery file");
  return static_cast<std::uint64_t>(st.st_size);
}
void resize(int fd, std::uint64_t size) {
  int result;
  do { result = ::ftruncate(fd, static_cast<off_t>(size)); } while (result < 0 && errno == EINTR);
  if (result < 0) io("truncate WAL/recovery file");
}
void read_at(int fd, std::span<std::byte> bytes, std::uint64_t offset) {
  if (offset > static_cast<std::uint64_t>(std::numeric_limits<off_t>::max()) ||
      bytes.size() > static_cast<std::uint64_t>(std::numeric_limits<off_t>::max()) - offset)
    throw Error(ErrorCode::Corruption, "recovery read exceeds file offset range");
  std::size_t done = 0;
  while (done < bytes.size()) {
    const auto n = ::pread(fd, bytes.data() + done, bytes.size() - done, static_cast<off_t>(offset + done));
    if (n < 0) { if (errno == EINTR) continue; io("read WAL/recovery file"); }
    if (n == 0) throw Error(ErrorCode::Corruption, "short WAL/recovery file");
    done += static_cast<std::size_t>(n);
  }
}
void write_at(int fd, std::span<const std::byte> bytes, std::uint64_t offset) {
  if (offset > static_cast<std::uint64_t>(std::numeric_limits<off_t>::max()) ||
      bytes.size() > static_cast<std::uint64_t>(std::numeric_limits<off_t>::max()) - offset)
    throw Error(ErrorCode::Io, "recovery write exceeds file offset range");
  std::size_t done = 0;
  while (done < bytes.size()) {
    const auto n = ::pwrite(fd, bytes.data() + done, bytes.size() - done, static_cast<off_t>(offset + done));
    if (n < 0) { if (errno == EINTR) continue; io("write WAL/recovery file"); }
    if (n == 0) throw Error(ErrorCode::Io, "zero-length WAL/recovery write");
    done += static_cast<std::size_t>(n);
  }
}
Bytes read_all(int fd) {
  const auto size = length(fd);
  if (size > std::numeric_limits<std::size_t>::max()) throw Error(ErrorCode::Corruption, "recovery file too large");
  Bytes bytes(static_cast<std::size_t>(size));
  read_at(fd, bytes, 0);
  return bytes;
}
void lock_fd(int fd) {
  if (::flock(fd, LOCK_EX | LOCK_NB) < 0) {
    if (errno == EWOULDBLOCK) throw Error(ErrorCode::Busy, "database recovery files already owned");
    io("lock WAL/recovery file");
  }
}
void sync_directory(const std::filesystem::path& dir) { Fd fd(dir, O_RDONLY | O_DIRECTORY); force_fd(fd.fd); }
void store_file(const std::filesystem::path& path, std::span<const std::byte> bytes) {
  Fd fd(path, O_RDWR | O_CREAT | O_TRUNC); write_at(fd.fd, bytes, 0); force_fd(fd.fd);
}
std::uint32_t checksum_field(Bytes bytes, std::size_t offset) {
  encoding::store<std::uint32_t>(bytes, offset, 0); return crc32(bytes);
}
struct WalRecord {
  LogType type;
  std::uint64_t lsn, tx, previous;
  FileId file = 0;
  PageId page = 0;
  std::optional<Page> before, after;
};
struct TransactionLog { std::uint64_t last = 0; LogType state = LogType::Begin; };
Bytes frame_header(const WalRecord& record) {
  Bytes header(kFrameHeader);
  const auto size = kFrameHeader + (record.before ? kPageSize : 0) + (record.after ? kPageSize : 0);
  encoding::store<std::uint32_t>(header, 0, kFrameMagic);
  encoding::store<std::uint32_t>(header, 4, static_cast<std::uint32_t>(size));
  encoding::store<std::uint16_t>(header, 8, 1);
  encoding::store<std::uint16_t>(header, 10, static_cast<std::uint16_t>(record.type));
  encoding::store<std::uint64_t>(header, 16, record.lsn);
  encoding::store<std::uint64_t>(header, 24, record.tx);
  encoding::store<std::uint64_t>(header, 32, record.previous);
  encoding::store<FileId>(header, 40, record.file);
  encoding::store<PageId>(header, 44, record.page);
  encoding::store<std::uint32_t>(header, 48, record.before ? 1U : 0U);
  encoding::store<std::uint32_t>(header, 12, checksum_field(Bytes(header.begin(), header.begin() + 76), 12));
  return header;
}
Bytes encode(const WalRecord& record) {
  auto bytes = frame_header(record);
  if (record.before) bytes.insert(bytes.end(), record.before->bytes.begin(), record.before->bytes.end());
  if (record.after) bytes.insert(bytes.end(), record.after->bytes.begin(), record.after->bytes.end());
  encoding::store<std::uint32_t>(bytes, 76, checksum_field(bytes, 76));
  return bytes;
}
}  // namespace

struct Wal::Impl {
  Impl(const std::filesystem::path& directory, std::size_t frames, std::uint16_t max_keys, FaultHook callback)
      : dir(directory), hook(std::move(callback)), log(dir / "database.wal", O_RDWR | O_CREAT) {
    lock_fd(log.fd);
    if (!std::filesystem::exists(dir / "recovery.meta")) {
      if (length(log.fd) > kWalHeader) throw Error(ErrorCode::Corruption, "missing recovery manifest for nonempty WAL");
      bootstrap(frames, max_keys);
    }
    load_header();
    const auto records = scan();
    recover(records);
  }
  const std::filesystem::path dir;
  const FaultHook hook;
  Fd log;
  mutable std::mutex mutex;
  Bytes header;
  std::map<std::uint64_t, TransactionLog> transactions;
  std::uint64_t last = 0, max_tx = 0, end = kWalHeader, writer = 0;
  bool failed = false;
  WalStats counters;
  void fault(std::string_view point) const { if (hook) hook(point); }
  void check() const {
    if (log.fd < 0) throw Error(ErrorCode::Closed, "WAL is closed");
    if (failed) throw Error(ErrorCode::Io, "WAL failed; reopen to determine transaction outcome");
  }
  void bootstrap(std::size_t frames, std::uint16_t max_keys) {
    const bool heap_exists = std::filesystem::exists(dir / "records.db");
    const bool index_exists = std::filesystem::exists(dir / "primary.idx");
    if (heap_exists && !index_exists) {
      HeapFile probe(dir / "records.db", frames);
      if (!probe.scan().empty()) throw Error(ErrorCode::Corruption, "bootstrap requires a matched nonempty heap/index pair");
      probe.close();
    } else if (index_exists && !heap_exists) {
      BPlusTree probe(dir / "primary.idx", {frames, max_keys});
      if (probe.size() != 0) throw Error(ErrorCode::Corruption, "bootstrap requires a matched nonempty heap/index pair");
      probe.close();
    }
    HeapFile heap(dir / "records.db", frames);
    BPlusTree index(dir / "primary.idx", {frames, max_keys});
    const auto rows = heap.scan();
    if (rows.size() != index.size()) throw Error(ErrorCode::Corruption, "cannot adopt mismatched heap/index");
    for (const auto& [rid, bytes] : rows)
      if (index.find(deserialize(bytes).key) != rid) throw Error(ErrorCode::Corruption, "cannot adopt invalid index binding");
    heap.sync(); index.sync();
    Fd heap_fd(dir / "records.db", O_RDONLY), index_fd(dir / "primary.idx", O_RDONLY);
    const auto heap_bytes = read_all(heap_fd.fd), index_bytes = read_all(index_fd.fd);
    for (const auto* bytes : {&heap_bytes, &index_bytes}) {
      for (std::size_t offset = 0; offset < bytes->size(); offset += kPageSize)
        if (encoding::load<std::uint64_t>(*bytes, offset + 16) != 0)
          throw Error(ErrorCode::Corruption, "WAL-stamped data cannot be adopted without its recovery files");
    }
    store_file(dir / "baseline.heap", heap_bytes); store_file(dir / "baseline.index", index_bytes);
    header.assign(kWalHeader, std::byte{0});
    encoding::store<std::uint32_t>(header, 0, kWalMagic);
    encoding::store<std::uint32_t>(header, 4, 1);
    encoding::store<std::uint32_t>(header, 8, kPageSize);
    encoding::store<std::uint64_t>(header, 16, heap_bytes.size());
    encoding::store<std::uint64_t>(header, 24, index_bytes.size());
    encoding::store<std::uint32_t>(header, 32, crc32(heap_bytes));
    encoding::store<std::uint32_t>(header, 36, crc32(index_bytes));
    encoding::store<std::uint32_t>(header, 40, checksum_field(header, 40));
    resize(log.fd, 0); write_at(log.fd, header, 0); force_fd(log.fd);
    store_file(dir / "recovery.meta.tmp", header);
    std::filesystem::rename(dir / "recovery.meta.tmp", dir / "recovery.meta");
    sync_directory(dir);
    heap.close(); index.close();
  }
  void load_header() {
    Fd manifest(dir / "recovery.meta", O_RDONLY);
    const auto expected = read_all(manifest.fd);
    if (expected.size() != kWalHeader || length(log.fd) < kWalHeader)
      throw Error(ErrorCode::Corruption, "missing/truncated recovery manifest or WAL header");
    header.resize(kWalHeader); read_at(log.fd, header, 0);
    if (header != expected || encoding::load<std::uint32_t>(header, 0) != kWalMagic ||
        encoding::load<std::uint32_t>(header, 4) != 1 || encoding::load<std::uint32_t>(header, 8) != kPageSize ||
        encoding::load<std::uint32_t>(header, 40) != checksum_field(header, 40))
      throw Error(ErrorCode::Corruption, "invalid recovery manifest/WAL header");
  }
  std::vector<WalRecord> scan() {
    std::vector<WalRecord> result;
    const auto bytes = read_all(log.fd);
    std::size_t offset = kWalHeader;
    while (offset < bytes.size()) {
      if (bytes.size() - offset < kFrameHeader) break; // Provably incomplete final header.
      Bytes h(bytes.begin() + static_cast<std::ptrdiff_t>(offset), bytes.begin() + static_cast<std::ptrdiff_t>(offset + kFrameHeader));
      if (encoding::load<std::uint32_t>(h, 0) != kFrameMagic || encoding::load<std::uint16_t>(h, 8) != 1 ||
          encoding::load<std::uint32_t>(h, 12) != checksum_field(Bytes(h.begin(), h.begin() + 76), 12))
        throw Error(ErrorCode::Corruption, "corrupt WAL frame header");
      const auto size = encoding::load<std::uint32_t>(h, 4);
      const auto type_value = encoding::load<std::uint16_t>(h, 10);
      const auto flags = encoding::load<std::uint32_t>(h, 48);
      if (type_value < 1 || type_value > 4 || flags > 1 ||
          (size != kFrameHeader && size != kFrameHeader + kPageSize && size != kFrameHeader + 2 * kPageSize))
        throw Error(ErrorCode::Corruption, "invalid WAL frame size/type/flags");
      if (size > bytes.size() - offset) break; // Length is protected by validated header CRC.
      Bytes frame(bytes.begin() + static_cast<std::ptrdiff_t>(offset), bytes.begin() + static_cast<std::ptrdiff_t>(offset + size));
      if (encoding::load<std::uint32_t>(frame, 76) != checksum_field(frame, 76))
        throw Error(ErrorCode::Corruption, "corrupt complete WAL frame (including final frame)");
      WalRecord record{static_cast<LogType>(type_value), encoding::load<std::uint64_t>(h, 16),
                    encoding::load<std::uint64_t>(h, 24), encoding::load<std::uint64_t>(h, 32), 0, 0, {}, {}};
      record.file = encoding::load<FileId>(h, 40); record.page = encoding::load<PageId>(h, 44);
      if (record.lsn != last + 1 || record.tx == 0) throw Error(ErrorCode::Corruption, "invalid WAL LSN/transaction ID");
      if (record.type == LogType::Update) {
        if ((record.file != kHeapFileId && record.file != kIndexFileId) ||
            size != kFrameHeader + kPageSize * (1U + flags))
          throw Error(ErrorCode::Corruption, "invalid WAL UPDATE payload");
        std::size_t cursor = kFrameHeader;
        if (flags != 0) {
          record.before.emplace(); std::copy_n(frame.begin() + static_cast<std::ptrdiff_t>(cursor), kPageSize, record.before->bytes.begin());
          record.before->validate(record.page); cursor += kPageSize;
        }
        record.after.emplace(); std::copy_n(frame.begin() + static_cast<std::ptrdiff_t>(cursor), kPageSize, record.after->bytes.begin());
        record.after->validate(record.page);
        if (encoding::load<std::uint64_t>(record.after->bytes, 16) != record.lsn)
          throw Error(ErrorCode::Corruption, "WAL after-image LSN mismatch");
      } else if (size != kFrameHeader || flags != 0 || record.file != 0 || record.page != 0) {
        throw Error(ErrorCode::Corruption, "invalid transaction WAL frame");
      }
      analyze(record);
      result.push_back(std::move(record)); offset += size;
    }
    counters.trimmed_bytes = bytes.size() - offset;
    if (offset != bytes.size()) resize(log.fd, offset);
    // A process crash can leave complete but previously unforced frames visible.
    // Stabilize the accepted prefix before using it as a recovery decision.
    force_fd(log.fd);
    end = offset; counters.durable_lsn = last;
    return result;
  }
  void analyze(const WalRecord& record) {
    if (record.type == LogType::Begin) {
      if (transactions.contains(record.tx) || record.previous != 0)
        throw Error(ErrorCode::Corruption, "duplicate/chained WAL BEGIN");
      transactions.emplace(record.tx, TransactionLog{record.lsn, LogType::Begin});
    } else {
      const auto found = transactions.find(record.tx);
      if (found == transactions.end() || found->second.state != LogType::Begin || found->second.last != record.previous)
        throw Error(ErrorCode::Corruption, "invalid WAL transaction chain/state");
      found->second.last = record.lsn;
      if (record.type != LogType::Update) found->second.state = record.type;
    }
    last = record.lsn; max_tx = std::max(max_tx, record.tx);
    ++counters.records;
    if (record.type == LogType::Update) ++counters.updates;
  }
  void recover(const std::vector<WalRecord>& records) {
    // Lock BOTH materialized data files before repair; their headers may be torn.
    Fd heap(dir / "records.db", O_RDWR | O_CREAT), index(dir / "primary.idx", O_RDWR | O_CREAT);
    lock_fd(heap.fd); lock_fd(index.fd);
    Fd base_heap(dir / "baseline.heap", O_RDONLY), base_index(dir / "baseline.index", O_RDONLY);
    const auto heap_bytes = read_all(base_heap.fd), index_bytes = read_all(base_index.fd);
    if (heap_bytes.size() != encoding::load<std::uint64_t>(header, 16) || index_bytes.size() != encoding::load<std::uint64_t>(header, 24) ||
        crc32(heap_bytes) != encoding::load<std::uint32_t>(header, 32) || crc32(index_bytes) != encoding::load<std::uint32_t>(header, 36))
      throw Error(ErrorCode::Corruption, "immutable recovery baseline is missing or corrupt");
    Fd scratch_heap(dir / "recovery.heap.tmp", O_RDWR | O_CREAT | O_TRUNC);
    Fd scratch_index(dir / "recovery.index.tmp", O_RDWR | O_CREAT | O_TRUNC);
    write_at(scratch_heap.fd, heap_bytes, 0); write_at(scratch_index.fd, index_bytes, 0);
    fault("recovery.after_baseline");
    std::uint64_t generation = 1;
    for (const auto& record : records) {
      if (record.type != LogType::Update) continue;
      if (record.file == kHeapFileId && record.page == 0)
        generation = std::max(generation, encoding::load<std::uint64_t>(record.after->bytes, 32));
      if (transactions.at(record.tx).state != LogType::Commit) continue;
      write_at(record.file == kHeapFileId ? scratch_heap.fd : scratch_index.fd,
               record.after->bytes, static_cast<std::uint64_t>(record.page) * kPageSize);
      ++counters.redo_pages; fault("recovery.redo_page");
    }
    for (const auto& [tx, state] : transactions) {
      (void)tx;
      if (state.state == LogType::Commit) ++counters.recovered_commits;
      else if (state.state == LogType::Begin) ++counters.recovered_losers;
    }
    auto finish_file = [&](int source, int target, bool is_heap) {
      Page meta; read_at(source, meta.bytes, 0); meta.validate(0);
      const auto count = encoding::load<PageId>(meta.bytes, 24);
      if (count == 0 || count == kInvalidPageId) throw Error(ErrorCode::Corruption, "invalid recovered file length");
      if (is_heap && encoding::load<std::uint64_t>(meta.bytes, 32) < generation) {
        encoding::store<std::uint64_t>(meta.bytes, 32, generation); meta.seal(); write_at(source, meta.bytes, 0);
      }
      const auto size = static_cast<std::uint64_t>(count) * kPageSize;
      if (length(source) < size) throw Error(ErrorCode::Corruption, "WAL omits allocated page images");
      resize(source, size); force_fd(source);
      for (PageId id = 0; id < count; ++id) {
        Page page; read_at(source, page.bytes, static_cast<std::uint64_t>(id) * kPageSize); page.validate(id);
        write_at(target, std::span(page.bytes).first(kPageSize / 2), static_cast<std::uint64_t>(id) * kPageSize);
        fault("recovery.after_half_page");
        write_at(target, std::span(page.bytes).subspan(kPageSize / 2), static_cast<std::uint64_t>(id) * kPageSize + kPageSize / 2);
      }
      resize(target, size); force_fd(target);
    };
    fault("recovery.after_redo");
    finish_file(scratch_heap.fd, heap.fd, true); fault("recovery.after_publish_heap");
    finish_file(scratch_index.fd, index.fd, false);
    sync_directory(dir); fault("recovery.complete");
  }
  std::uint64_t append(LogType type, std::uint64_t tx, FileId file = 0, std::optional<Page> before = {}, std::optional<Page> after = {}) {
    check();
    const auto found = transactions.find(tx);
    if (tx == 0 || (type == LogType::Begin ? found != transactions.end() : found == transactions.end() || found->second.state != LogType::Begin))
      throw Error(ErrorCode::InvalidTransactionState, "invalid transaction state for WAL append");
    if (last == std::numeric_limits<std::uint64_t>::max()) throw Error(ErrorCode::Io, "WAL LSN exhausted");
    WalRecord record{type, last + 1, tx, type == LogType::Begin ? 0 : found->second.last, file, after ? after->id() : 0, before, after};
    if (record.after) { encoding::store<std::uint64_t>(record.after->bytes, 16, record.lsn); record.after->seal(); }
    const auto bytes = encode(record);
    try {
      const auto half = bytes.size() / 2;
      write_at(log.fd, std::span(bytes).first(half), end);
      switch (type) {
        case LogType::Begin: fault("wal.begin.after_half_frame"); break;
        case LogType::Update: fault("wal.update.after_half_frame"); break;
        case LogType::Commit: fault("wal.commit.after_half_frame"); break;
        case LogType::Abort: fault("wal.abort.after_half_frame"); break;
      }
      write_at(log.fd, std::span(bytes).subspan(half), end + half);
      analyze(record); end += bytes.size();
    }
    catch (...) { failed = true; throw; }
    return record.lsn;
  }
  void force(std::uint64_t lsn) {
    check();
    if (lsn > last) throw Error(ErrorCode::InvalidArgument, "cannot force a future WAL LSN");
    if (lsn <= counters.durable_lsn) return;
    try { fault("wal.before_force"); force_fd(log.fd); counters.durable_lsn = last; fault("wal.after_force"); }
    catch (...) { failed = true; throw; }
  }
};

Wal::Wal(const std::filesystem::path& dir, std::size_t frames, std::uint16_t max_keys, FaultHook hook)
    : impl_(std::make_unique<Impl>(dir, frames, max_keys, std::move(hook))) {}
Wal::~Wal() = default;
void Wal::begin(std::uint64_t tx) { std::lock_guard lock(impl_->mutex); (void)impl_->append(LogType::Begin, tx); }
void Wal::start_writer(std::uint64_t tx) {
  std::lock_guard lock(impl_->mutex); impl_->check();
  if (impl_->writer != 0 || !impl_->transactions.contains(tx) || impl_->transactions.at(tx).state != LogType::Begin)
    throw Error(ErrorCode::InvalidTransactionState, "invalid WAL writer ownership");
  impl_->writer = tx;
}
void Wal::finish_writer(std::uint64_t tx) { std::lock_guard lock(impl_->mutex); if (impl_->writer == tx) impl_->writer = 0; }
Page Wal::log_page(FileId file, std::optional<Page> before, Page after) {
  std::lock_guard lock(impl_->mutex);
  if (file != kHeapFileId && file != kIndexFileId) throw Error(ErrorCode::InvalidArgument, "invalid WAL file ID");
  after.validate(after.id());
  if (before) before->validate(after.id());
  if (impl_->writer == 0) throw Error(ErrorCode::InvalidTransactionState, "page mutation requires a WAL writer");
  const auto lsn = impl_->append(LogType::Update, impl_->writer, file, before, after);
  encoding::store<std::uint64_t>(after.bytes, 16, lsn); after.seal(); return after;
}
void Wal::force(std::uint64_t lsn) { std::lock_guard lock(impl_->mutex); impl_->force(lsn); }
void Wal::commit(std::uint64_t tx) {
  { std::lock_guard lock(impl_->mutex); impl_->force(impl_->append(LogType::Commit, tx)); }
  impl_->fault("commit.after_force");
}
void Wal::abort(std::uint64_t tx) {
  { std::lock_guard lock(impl_->mutex); impl_->force(impl_->append(LogType::Abort, tx)); }
  impl_->fault("abort.after_force");
}
void Wal::close() {
  std::lock_guard lock(impl_->mutex);
  if (impl_->log.fd < 0) return;
  impl_->force(impl_->last);
  const auto fd = impl_->log.fd; impl_->log.fd = -1;
  if (::close(fd) < 0) io("close WAL");
}
bool Wal::rollback_allowed() const {
  std::lock_guard lock(impl_->mutex);
  return !impl_->failed && impl_->writer != 0 && impl_->transactions.at(impl_->writer).state == LogType::Abort;
}
bool Wal::committed(std::uint64_t tx) const {
  std::lock_guard lock(impl_->mutex);
  return impl_->transactions.contains(tx) && impl_->transactions.at(tx).state == LogType::Commit;
}
std::uint64_t Wal::max_transaction_id() const { std::lock_guard lock(impl_->mutex); return impl_->max_tx; }
WalStats Wal::stats() const { std::lock_guard lock(impl_->mutex); return impl_->counters; }
void Wal::fault(std::string_view point) const { impl_->fault(point); }
}  // namespace tinydb
