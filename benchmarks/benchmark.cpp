#include <algorithm>
#include <charconv>
#include <chrono>
#include <exception>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <latch>
#include <mutex>
#include <numeric>
#include <random>
#include <string_view>
#include <thread>

#include "storage/heap_file.h"
#include "transaction/database.h"

namespace {
using Clock = std::chrono::steady_clock;
struct Options {
  std::filesystem::path directory;
  std::size_t rows = 200, lookups = 100, transactions = 20, clients = 4, frames = 16;
  std::size_t recovery_transactions = 200;
  std::uint64_t seed = 20261019;
};
template <typename T> T integer(std::string_view text) {
  T value{};
  const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
  if (error != std::errc{} || end != text.data() + text.size()) throw std::runtime_error("invalid integer option");
  return value;
}
Options parse(int argc, char** argv) {
  Options options;
  for (int i = 1; i < argc; i += 2) {
    if (i + 1 == argc) throw std::runtime_error("every option requires a value");
    const std::string_view key(argv[i]), value(argv[i + 1]);
    if (key == "--dir") options.directory = value;
    else if (key == "--rows") options.rows = integer<std::size_t>(value);
    else if (key == "--lookups") options.lookups = integer<std::size_t>(value);
    else if (key == "--transactions") options.transactions = integer<std::size_t>(value);
    else if (key == "--recovery-transactions") options.recovery_transactions = integer<std::size_t>(value);
    else if (key == "--clients") options.clients = integer<std::size_t>(value);
    else if (key == "--frames") options.frames = integer<std::size_t>(value);
    else if (key == "--seed") options.seed = integer<std::uint64_t>(value);
    else throw std::runtime_error("unknown benchmark option");
  }
  if (options.directory.empty() || options.rows == 0 || options.rows > 100000 ||
      options.lookups == 0 || options.lookups > 1000000 || options.transactions == 0 || options.transactions > 10000 ||
      options.clients == 0 || options.clients > 64 || options.frames == 0 || options.frames > 65536 ||
      options.recovery_transactions == 0 || options.recovery_transactions > 10000)
    throw std::runtime_error("--dir required; positive sizes limited to rows=100000, lookups=1000000, transactions/recovery-transactions=10000, clients=64, frames=65536");
  return options;
}
void require(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
struct Measurement {
  double seconds;
  tinydb::BufferPoolStats heap, index;
};
tinydb::BufferPoolStats difference(const tinydb::BufferPoolStats& after, const tinydb::BufferPoolStats& before) {
  auto result = after;
  result.hits -= before.hits; result.misses -= before.misses;
  result.disk_reads -= before.disk_reads; result.page_writes -= before.page_writes;
  result.evictions -= before.evictions; return result;
}
template <typename Function> double timed(Function&& operation) {
  const auto start = Clock::now(); operation();
  return std::chrono::duration<double>(Clock::now() - start).count();
}
void row(const std::string& workload, std::size_t operations, double seconds,
         std::uintmax_t wal_bytes = 0, std::uint64_t redo_pages = 0,
         tinydb::BufferPoolStats heap = {}, tinydb::BufferPoolStats index = {}) {
  std::cout << workload << ',' << operations << ',' << std::fixed << std::setprecision(9) << seconds << ','
            << static_cast<double>(operations) / seconds << ',' << wal_bytes << ',' << redo_pages << ','
            << heap.hits << ',' << heap.misses << ',' << heap.hit_rate() << ','
            << index.hits << ',' << index.misses << ',' << index.hit_rate() << '\n';
  if (!std::cout) throw std::runtime_error("benchmark output failed");
}
void raw_workloads(const Options& options) {
  tinydb::HeapFile heap(options.directory / "raw.db", options.frames);
  tinydb::BPlusTree index(options.directory / "raw.idx", {options.frames, 0});
  std::mt19937_64 random(options.seed);
  std::vector<tinydb::IndexKey> keys(options.rows);
  std::iota(keys.begin(), keys.end(), 0); std::shuffle(keys.begin(), keys.end(), random);
  const auto insertion = timed([&] {
    for (auto key : keys) {
      const auto rid = heap.insert(tinydb::serialize({key, std::string(96, 'x'), key}));
      require(index.insert(key, rid), "raw insert duplicate");
    }
    heap.sync(); index.sync();
  });
  row("raw_heap_index_insert_sync", options.rows, insertion, 0, 0, heap.buffer_stats(), index.buffer_stats());
  require(index.validate().entries == options.rows, "raw index count mismatch");
  std::vector<tinydb::IndexKey> probes(options.lookups);
  for (auto& key : probes) key = static_cast<tinydb::IndexKey>(random() % options.rows);
  auto measure = [&](auto&& operation) {
    const auto before_heap = heap.buffer_stats(), before_index = index.buffer_stats();
    Measurement result{timed(operation), {}, {}};
    result.heap = difference(heap.buffer_stats(), before_heap);
    result.index = difference(index.buffer_stats(), before_index); return result;
  };
  // Warm each indexed probe once. The OS cache is not flushed in any workload.
  for (auto key : probes) { const auto rid = index.find(key); require(rid.has_value(), "warmup key missing"); (void)heap.get(*rid); }
  const auto indexed = measure([&] {
    for (auto key : probes) {
      const auto rid = index.find(key); require(rid.has_value(), "indexed key missing");
      require(tinydb::deserialize(heap.get(*rid)).key == key, "indexed lookup mismatch");
    }
  });
  row("raw_indexed_lookup", probes.size(), indexed.seconds, 0, 0, indexed.heap, indexed.index);
  const auto scanned = measure([&] {
    for (auto key : probes) {
      bool found = false;
      // Public heap.scan copies the entire heap, then the caller filters it.
      for (const auto& [rid, bytes] : heap.scan()) {
        (void)rid; if (tinydb::deserialize(bytes).key == key) { found = true; break; }
      }
      require(found, "heap lookup mismatch");
    }
  });
  row("raw_heap_scan_lookup", probes.size(), scanned.seconds, 0, 0, scanned.heap, scanned.index);
  const auto width = std::min<std::size_t>(20, options.rows);
  const auto ranged = measure([&] {
    for (auto key : probes) {
      const auto upper = std::min<tinydb::IndexKey>(static_cast<tinydb::IndexKey>(options.rows), key + static_cast<tinydb::IndexKey>(width));
      const auto entries = index.range(key, upper);
      require(entries.size() == static_cast<std::size_t>(upper - key), "range count mismatch");
      for (const auto& [expected, rid] : entries) require(tinydb::deserialize(heap.get(rid)).key == expected, "range RID mismatch");
    }
  });
  row("raw_index_range_max20", probes.size(), ranged.seconds, 0, 0, ranged.heap, ranged.index);
  heap.close(); index.close();
}
void transaction_workloads(const Options& options) {
  const auto path = options.directory / "transactions";
  tinydb::TransactionalDatabase db(path, {options.frames, 0, std::chrono::seconds(30), {}});
  const auto insertion = timed([&] {
    auto tx = db.begin();
    for (std::size_t i = 0; i < options.rows; ++i) tx.insert({static_cast<std::int64_t>(i), "account", 0});
    tx.commit();
  });
  row("durable_bulk_insert_one_tx", options.rows, insertion, std::filesystem::file_size(path / "database.wal"));
  const auto serial = timed([&] {
    for (std::size_t i = 0; i < options.transactions; ++i) {
      auto tx = db.begin(); auto record = tx.get_for_update(0).value(); ++record.balance;
      tx.update(0, record); tx.commit();
    }
  });
  row("durable_update_1_client", options.transactions, serial, std::filesystem::file_size(path / "database.wal"));
  std::latch start(1);
  std::mutex error_mutex; std::exception_ptr failure;
  std::vector<std::jthread> threads;
  try {
    for (std::size_t client = 0; client < options.clients; ++client) {
      threads.emplace_back([&] {
        start.wait();
        try {
          for (std::size_t i = 0; i < options.transactions; ++i) {
            auto tx = db.begin(); auto record = tx.get_for_update(0).value(); ++record.balance;
            tx.update(0, record); tx.commit();
          }
        } catch (...) { std::lock_guard lock(error_mutex); if (!failure) failure = std::current_exception(); }
      });
    }
  } catch (...) { start.count_down(); throw; } // Unblock existing jthreads before unwinding.
  const auto concurrent = timed([&] { start.count_down(); for (auto& thread : threads) thread.join(); });
  if (failure) std::rethrow_exception(failure);
  row("durable_update_" + std::to_string(options.clients) + "_clients", options.clients * options.transactions,
      concurrent, std::filesystem::file_size(path / "database.wal"));
  auto read = db.begin();
  require(read.get(0)->balance == static_cast<std::int64_t>((options.clients + 1) * options.transactions), "lost concurrent increment");
  read.commit(); (void)db.validate(); db.close();
}
void recovery_workloads(const Options& options) {
  const auto path = options.directory / "recovery";
  {
    tinydb::TransactionalDatabase db(path, {options.frames, 0, std::chrono::seconds(30), {}});
    auto tx = db.begin();
    tx.insert({0, "recovery", 0});
    tx.commit(); db.close();
  }
  // One fixed row, growing committed update history, three samples
  // per size. Timed constructor includes replay, fsync and structural validation.
  for (std::size_t stage = 0; stage < 3; ++stage) {
    if (stage != 0) {
      tinydb::TransactionalDatabase db(path, {options.frames, 0, std::chrono::seconds(30), {}});
      for (std::size_t i = 0; i < options.recovery_transactions; ++i) {
        auto tx = db.begin(); tx.update(0, {0, "recovery", static_cast<std::int64_t>(stage)}); tx.commit();
      }
      db.close();
    }
    const auto bytes = std::filesystem::file_size(path / "database.wal");
    for (std::size_t sample = 0; sample < 3; ++sample) {
      std::unique_ptr<tinydb::TransactionalDatabase> db;
      const auto elapsed = timed([&] { db = std::make_unique<tinydb::TransactionalDatabase>(path,
          tinydb::DatabaseOptions{options.frames, 0, std::chrono::seconds(30), {}}); });
      const auto stats = db->wal_stats();
      row("recovery_stage" + std::to_string(stage) + "_sample" + std::to_string(sample), 1, elapsed, bytes, stats.redo_pages);
      require(db->validate().entries == 1, "recovered row count mismatch");
      db->close();
      tinydb::HeapFile check(path / "records.db", options.frames);
      const auto rows = check.scan();
      require(!rows.empty() && tinydb::deserialize(rows.front().second).balance == static_cast<std::int64_t>(stage), "recovered value mismatch");
      check.close(); // Read-only inspection does not grow WAL between samples.
    }
  }
}
}
int main(int argc, char** argv) {
  try {
    const auto options = parse(argc, argv);
    // Never delete or reuse an existing path. Each measurement owns a fresh run.
    if (!std::filesystem::create_directory(options.directory)) throw std::runtime_error("benchmark --dir must not exist");
    std::cout << "workload,operations,seconds,ops_per_second,wal_bytes,redo_pages,heap_hits,heap_misses,heap_hit_rate,index_hits,index_misses,index_hit_rate\n";
    raw_workloads(options); transaction_workloads(options); recovery_workloads(options);
  } catch (const std::exception& error) { std::cerr << "Benchmark failed: " << error.what() << '\n'; return 1; }
}
