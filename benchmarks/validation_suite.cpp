#include <algorithm>
#include <atomic>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <latch>
#include <map>
#include <mutex>
#include <numeric>
#include <random>
#include <sstream>
#include <string>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

#include "common/error.h"
#include "storage/heap_file.h"
#include "transaction/database.h"

namespace {
using namespace tinydb;
using Clock = std::chrono::steady_clock;
using Fields = std::map<std::string, std::string>;
struct Options {
  std::string phase, method = "indexed", scenario;
  std::filesystem::path directory;
  std::size_t rows = 1000, lookups = 10000, frames = 64, workers = 1, transactions = 100;
  std::uint64_t seed = 20261019;
};
template <typename T> T integer(const std::string& text) {
  T value{};
  const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
  if (error != std::errc{} || end != text.data() + text.size()) throw std::runtime_error("invalid numeric option: " + text);
  return value;
}
Options parse(int argc, char** argv) {
  Options result;
  for (int i = 1; i < argc; i += 2) {
    if (i + 1 == argc) throw std::runtime_error("options require values");
    const std::string key = argv[i], value = argv[i + 1];
    if (key == "--phase") result.phase = value;
    else if (key == "--dir") result.directory = value;
    else if (key == "--method") result.method = value;
    else if (key == "--scenario") result.scenario = value;
    else if (key == "--rows") result.rows = integer<std::size_t>(value);
    else if (key == "--lookups") result.lookups = integer<std::size_t>(value);
    else if (key == "--frames") result.frames = integer<std::size_t>(value);
    else if (key == "--workers") result.workers = integer<std::size_t>(value);
    else if (key == "--transactions") result.transactions = integer<std::size_t>(value);
    else if (key == "--seed") result.seed = integer<std::uint64_t>(value);
    else throw std::runtime_error("unknown option: " + key);
  }
  if (result.directory.empty() || result.rows == 0 || result.rows > 1000000 || result.lookups == 0 || result.lookups > 1000000 ||
      result.frames == 0 || result.frames > 65536 || result.workers == 0 || result.workers > 1000 ||
      result.transactions == 0 || result.transactions > 100000)
    throw std::runtime_error("invalid workload sizes/directory");
  return result;
}
void require(bool condition, const std::string& message) { if (!condition) throw std::runtime_error(message); }
std::string decimal(double value) { std::ostringstream out; out << std::setprecision(17) << value; return out.str(); }
void quoted(std::ostream& out, const std::string& text) {
  out << '"';
  for (char character : text) {
    const auto c = static_cast<unsigned char>(character);
    if (c == '"' || c == '\\') out << '\\' << static_cast<char>(c);
    else if (c < 32) out << "\\u00" << std::hex << std::setw(2) << std::setfill('0') << static_cast<unsigned>(c) << std::dec;
    else out << static_cast<char>(c);
  }
  out << '"';
}
void emit(const Fields& fields) {
  std::cout << '{'; bool first = true;
  for (const auto& [key, value] : fields) {
    if (!first) std::cout << ',';
    first = false; quoted(std::cout, key); std::cout << ':'; quoted(std::cout, value);
  }
  std::cout << "}\n" << std::flush;
  require(static_cast<bool>(std::cout), "result output failed");
}
Fields base(const Options& options, const std::string& category, const std::string& name) {
  return {{"category", category}, {"name", name}, {"dataset_size", std::to_string(options.rows)},
          {"workers", std::to_string(options.workers)}, {"capacity_per_file", std::to_string(options.frames)},
          {"seed", std::to_string(options.seed)}, {"status", "pass"}, {"correctness", "pass"}};
}
BufferPoolStats delta(BufferPoolStats after, const BufferPoolStats& before) {
  after.hits -= before.hits; after.misses -= before.misses; after.evictions -= before.evictions;
  return after;
}
void cache(Fields& fields, const std::string& name, const BufferPoolStats& stats) {
  fields[name + "_hits"] = std::to_string(stats.hits);
  fields[name + "_misses"] = std::to_string(stats.misses);
  fields[name + "_evictions"] = std::to_string(stats.evictions);
  fields[name + "_hit_rate"] = decimal(stats.hit_rate());
}
Record expected(IndexKey key) { return {key, "benchmark", key}; }
void validate_pair(HeapFile& heap, BPlusTree& index, std::size_t count) {
  require(index.validate().entries == count, "tree count/invariants mismatch");
  const auto rows = heap.scan(); require(rows.size() == count, "heap count mismatch");
  std::vector<bool> found(count);
  for (const auto& [rid, bytes] : rows) {
    const auto row = deserialize(bytes);
    require(row.key >= 0 && static_cast<std::uint64_t>(row.key) < count, "unexpected key");
    require(!found[static_cast<std::size_t>(row.key)], "duplicate heap key");
    found[static_cast<std::size_t>(row.key)] = true;
    require(row == expected(row.key) && index.find(row.key) == rid, "heap/index binding or row mismatch");
  }
}
void prepare(const Options& options) {
  require(std::filesystem::create_directory(options.directory), "prepare directory must not exist");
  HeapFile heap(options.directory / "records.db", options.frames);
  BPlusTree index(options.directory / "primary.idx", {options.frames, 0});
  std::vector<IndexKey> keys(options.rows); std::iota(keys.begin(), keys.end(), 0);
  std::mt19937_64 random(options.seed); std::shuffle(keys.begin(), keys.end(), random);
  const auto start = Clock::now();
  for (std::size_t i = 0; i < keys.size(); ++i) {
    const auto rid = heap.insert(serialize(expected(keys[i])));
    require(index.insert(keys[i], rid), "duplicate during setup");
    if ((i + 1) % 1000 == 0) std::cerr << "inserted=" << i + 1 << '\n' << std::flush;
  }
  heap.sync(); index.sync(); validate_pair(heap, index, options.rows); heap.close(); index.close();
  // A retained, identical probe stream is shared by independently opened methods.
  std::ofstream probes(options.directory / "keys.txt");
  for (std::size_t i = 0; i < options.lookups; ++i) probes << random() % options.rows << '\n';
  probes.close(); require(static_cast<bool>(probes), "writing lookup keys failed");
  auto fields = base(options, "setup", "heap_index_prepare");
  fields["operations"] = std::to_string(options.rows);
  fields["seconds"] = decimal(std::chrono::duration<double>(Clock::now() - start).count()); emit(fields);
}
double median(std::vector<double> values) {
  require(!values.empty(), "no samples"); std::sort(values.begin(), values.end());
  const auto middle = values.size() / 2;
  return values.size() % 2 ? values[middle] : (values[middle - 1] + values[middle]) / 2;
}
void lookup(const Options& options) {
  require(options.method == "indexed" || options.method == "sequential", "unknown lookup method");
  HeapFile heap(options.directory / "records.db", options.frames);
  BPlusTree index(options.directory / "primary.idx", {options.frames, 0});
  std::ifstream input(options.directory / "keys.txt");
  std::vector<IndexKey> keys; IndexKey key;
  while (input >> key) { require(key >= 0 && static_cast<std::uint64_t>(key) < options.rows, "probe key outside dataset"); keys.push_back(key); }
  require(input.eof() && keys.size() == options.lookups, "invalid/truncated probe stream");
  // Both independent methods do the same full validation and first 1000 indexed
  // probes before timing. Warm OS cache; bounded CLOCK state is not reset.
  validate_pair(heap, index, options.rows);
  for (std::size_t i = 0; i < std::min<std::size_t>(1000, keys.size()); ++i) {
    const auto rid = index.find(keys[i]); require(rid.has_value(), "warmup lookup missing");
    require(deserialize(heap.get(*rid)) == expected(keys[i]), "warmup lookup mismatch");
  }
  const auto before_heap = heap.buffer_stats(), before_index = index.buffer_stats();
  std::vector<double> latencies; latencies.reserve(keys.size());
  const auto started = Clock::now();
  for (std::size_t i = 0; i < keys.size(); ++i) {
    const auto begin = Clock::now();
    if (options.method == "indexed") {
      const auto rid = index.find(keys[i]); require(rid.has_value(), "indexed key missing");
      require(deserialize(heap.get(*rid)) == expected(keys[i]), "indexed row mismatch");
    } else {
      bool found = false;
      for (const auto& [rid, bytes] : heap.scan()) {
        (void)rid; const auto record = deserialize(bytes);
        if (record.key == keys[i]) { require(record == expected(keys[i]), "sequential row mismatch"); found = true; break; }
      }
      require(found, "sequential key missing");
    }
    latencies.push_back(std::chrono::duration<double, std::micro>(Clock::now() - begin).count());
    if ((i + 1) % 1000 == 0) std::cerr << "lookups=" << i + 1 << '\n' << std::flush;
  }
  const auto seconds = std::chrono::duration<double>(Clock::now() - started).count();
  auto fields = base(options, "lookup", options.method);
  fields["operations"] = std::to_string(keys.size()); fields["successes"] = std::to_string(keys.size()); fields["failures"] = "0";
  fields["seconds"] = decimal(seconds); fields["median_latency_us"] = decimal(median(latencies));
  fields["throughput_ops_s"] = decimal(static_cast<double>(keys.size()) / seconds);
  cache(fields, "heap", delta(heap.buffer_stats(), before_heap)); cache(fields, "index", delta(index.buffer_stats(), before_index));
  // Preserve per-operation evidence after timing; it cannot inflate throughput.
  std::ofstream samples(options.directory / (options.method + "-latencies.csv")); samples << "key,latency_us\n" << std::setprecision(17);
  for (std::size_t i = 0; i < keys.size(); ++i) samples << keys[i] << ',' << latencies[i] << '\n';
  samples.close(); require(static_cast<bool>(samples), "writing latency samples failed");
  validate_pair(heap, index, options.rows); heap.close(); index.close(); emit(fields);
}
void concurrency(const Options& options) {
  require(std::filesystem::create_directory(options.directory), "transaction directory must not exist");
  std::uint64_t successes = 0, failures = 0, timeouts = 0;
  std::map<std::string, std::uint64_t> errors;
  std::string launch_error;
  auto fields = base(options, "concurrency", "two_account_transfer"); fields["dataset_size"] = "2";
  {
    TransactionalDatabase db(options.directory, {options.frames, 0, std::chrono::seconds(30), {}});
    auto seed = db.begin(); seed.insert({1, "Alice", 0}); seed.insert({2, "Bob", 0}); seed.commit();
    std::latch start(1); std::mutex counts;
    std::vector<std::jthread> workers;
    const auto body = [&] {
      start.wait();
      for (std::size_t i = 0; i < options.transactions; ++i) {
        try {
          auto tx = db.begin(); auto a = tx.get_for_update(1).value(), b = tx.get_for_update(2).value();
          --a.balance; ++b.balance; tx.update(1, a); tx.update(2, b); tx.commit();
          std::lock_guard lock(counts); ++successes;
        } catch (const Error& error) {
          std::lock_guard lock(counts); ++failures; if (error.code() == ErrorCode::LockTimeout) ++timeouts;
          ++errors[std::to_string(static_cast<int>(error.code())) + ":" + error.what()];
        } catch (const std::exception& error) { std::lock_guard lock(counts); ++failures; ++errors[error.what()]; }
      }
    };
    try { for (std::size_t i = 0; i < options.workers; ++i) workers.emplace_back(body); }
    catch (const std::system_error& error) {
      launch_error = std::string("worker creation failed: ") + error.what();
    } catch (...) { start.count_down(); throw; }
    const auto begin = Clock::now(); start.count_down(); for (auto& worker : workers) worker.join();
    const auto seconds = std::chrono::duration<double>(Clock::now() - begin).count();
    fields["seconds"] = decimal(seconds); fields["operations"] = std::to_string(workers.size() * options.transactions);
    fields["successes"] = std::to_string(successes); fields["failures"] = std::to_string(failures);
    fields["timeouts"] = std::to_string(timeouts); fields["throughput_ops_s"] = decimal(static_cast<double>(successes) / seconds);
    require(successes + failures == workers.size() * options.transactions, "attempt counts disagree");
    require(db.validate().entries == 2, "concurrent tree invariant/count mismatch");
    const auto locks = db.lock_stats(); require(locks.active_transactions == 0 && locks.grants == 0 && locks.waiters == 0, "leaked transaction locks");
    auto read = db.begin(); const auto rows = read.scan();
    require(rows == std::vector<Record>{{1, "Alice", -static_cast<std::int64_t>(successes)}, {2, "Bob", static_cast<std::int64_t>(successes)}}, "lost or partial concurrent transfer");
    read.commit(); db.close();
  }
  // Recovery and full validation after every worker-count test, outside timing.
  TransactionalDatabase db(options.directory, {options.frames, 0, std::chrono::seconds(30), {}});
  require(db.validate().entries == 2, "recovered concurrency tree mismatch");
  auto tx = db.begin(); require(tx.get(1)->balance == -static_cast<std::int64_t>(successes) && tx.get(2)->balance == static_cast<std::int64_t>(successes), "recovered transfer mismatch");
  tx.commit(); db.close();
  std::string details;
  for (const auto& [error, count] : errors) details += std::to_string(count) + "x " + error + "; ";
  fields["details"] = details;
  if (failures) fields["status"] = "completed_with_failures";
  if (!launch_error.empty()) { fields["status"] = "unsupported"; fields["details"] += launch_error; fields.erase("throughput_ops_s"); }
  emit(fields);
}
void buffer(const Options& options) {
  require(std::filesystem::create_directory(options.directory), "buffer directory must not exist");
  // Actual BufferPool fetches, not heap-layout guesses; 128 real allocated pages.
  DiskManager disk(options.directory / "pages.db");
  for (std::size_t i = 0; i < 128; ++i) (void)disk.allocate_page(PageType::Heap);
  disk.sync(); BufferPool pool(disk, options.frames);
  for (std::size_t i = 0; i < options.lookups; ++i) {
    // Eight passes over a hot set of 16, then one pass over all 128 pages.
    const auto cycle = i % 256;
    const auto id = static_cast<PageId>(1 + (cycle < 128 ? cycle % 16 : cycle - 128));
    const auto guard = pool.fetch_read(id); require(guard.page().id() == id, "buffer fetched wrong page"); guard.page().validate(id);
  }
  auto fields = base(options, "buffer", "hot16_scan128"); fields["dataset_size"] = "128";
  fields["operations"] = std::to_string(options.lookups); cache(fields, "heap", pool.stats());
  pool.close(); disk.close(); emit(fields);
}
void seed_crash(const std::filesystem::path& path) {
  TransactionalDatabase db(path, {1, 3, std::chrono::seconds(5), {}});
  auto tx = db.begin(); tx.insert({1, std::string(4032, 'a'), 1000}); tx.insert({2, std::string(4032, 'b'), 500}); tx.commit(); db.close();
}
void recovered(const std::filesystem::path& path, bool committed) {
  for (int restart = 0; restart < 2; ++restart) {
    TransactionalDatabase db(path, {1, 0, std::chrono::seconds(5), {}});
    require(db.validate().entries == 2, "recovered tree invariant/count mismatch");
    auto tx = db.begin(); const auto rows = tx.scan();
    require(rows == std::vector<Record>{{1, std::string(4032, 'a'), committed ? 800 : 1000},
                                       {2, std::string(4032, 'b'), committed ? 700 : 500}}, "committed/loser crash outcome mismatch");
    tx.commit(); db.close();
  }
}
int crash_child(const Options& options) {
  bool commit = false, abort = false;
  auto hook = [&](std::string_view event) {
    if (event == "commit.after_force") commit = true;
    if (event == "abort.after_force") abort = true;
    if (event == options.scenario || (options.scenario == "torn_after_commit" && commit && event == "data.heap.after_half_write") ||
        (options.scenario == "torn_during_abort" && abort && event == "data.heap.after_half_write")) ::_exit(86);
  };
  TransactionalDatabase db(options.directory, {1, 0, std::chrono::seconds(5), hook});
  if (options.scenario.starts_with("recovery.")) return 10; // The selected hook must have terminated construction.
  auto tx = db.begin(); auto a = tx.get_for_update(1).value(), b = tx.get_for_update(2).value();
  a.balance = 800; tx.update(1, a); hook("between_updates"); b.balance = 700; tx.update(2, b); hook("before_commit");
  if (options.scenario == "abort.after_force" || options.scenario == "torn_during_abort") tx.rollback(); else tx.commit();
  if (options.scenario == "acknowledged_no_close") ::_exit(86);
  db.close(); return 11;
}
void exec_crash(const std::filesystem::path& executable, const std::filesystem::path& path, const std::string& point) {
  const auto pid = ::fork(); require(pid >= 0, "fork failed");
  if (pid == 0) {
    ::execl(executable.c_str(), executable.c_str(), "--phase", "crash-child", "--dir", path.c_str(), "--scenario", point.c_str(), nullptr);
    ::_exit(127);
  }
  int status = 0; pid_t result;
  do { result = ::waitpid(pid, &status, 0); } while (result < 0 && errno == EINTR);
  require(result == pid && WIFEXITED(status) && WEXITSTATUS(status) == 86, "child did not reach the requested actual crash point");
}
int crashes(const Options& options, const std::filesystem::path& executable) {
  require(std::filesystem::create_directory(options.directory), "crash directory must not exist");
  std::size_t failures = 0;
  const std::vector<std::string> scenarios{
      "before_commit", "between_updates", "commit.after_force", "acknowledged_no_close",
      "data.heap.after_half_write", "torn_after_commit", "abort.after_force", "torn_during_abort",
      "wal.begin.after_half_frame", "wal.update.after_half_frame", "wal.commit.after_half_frame",
      "recovery.after_baseline", "recovery.redo_page", "recovery.after_redo", "recovery.after_half_page", "recovery.after_publish_heap"};
  for (const auto& scenario : scenarios) {
    auto fields = base(options, "crash", scenario); fields["dataset_size"] = "2"; fields["capacity_per_file"] = "1";
    const auto path = options.directory / scenario;
    const auto start = Clock::now();
    try {
      seed_crash(path);
      const bool recovery = scenario.starts_with("recovery.");
      const bool committed = recovery || scenario == "commit.after_force" || scenario == "acknowledged_no_close" || scenario == "torn_after_commit";
      if (recovery) exec_crash(executable, path, "commit.after_force");
      exec_crash(executable, path, scenario);
      if (scenario == "commit.after_force") {
        HeapFile raw(path / "records.db", 1);
        for (const auto& [rid, bytes] : raw.scan()) { (void)rid; const auto row = deserialize(bytes); require(row.balance == (row.key == 1 ? 800 : 500), "expected a stale data page after durable commit"); }
      }
      recovered(path, committed);
      fields["successes"] = "1"; fields["failures"] = "0";
    } catch (const std::exception& error) {
      ++failures; fields["status"] = "fail"; fields["correctness"] = "fail"; fields["details"] = error.what(); fields["successes"] = "0"; fields["failures"] = "1";
    }
    fields["operations"] = "1"; fields["seconds"] = decimal(std::chrono::duration<double>(Clock::now() - start).count()); emit(fields);
  }
  return failures ? 1 : 0;
}
}  // namespace
int main(int argc, char** argv) {
  try {
    const auto options = parse(argc, argv);
    if (options.phase == "prepare") prepare(options);
    else if (options.phase == "lookup") lookup(options);
    else if (options.phase == "concurrency") concurrency(options);
    else if (options.phase == "buffer") buffer(options);
    else if (options.phase == "crash-child") return crash_child(options);
    else if (options.phase == "crashes") return crashes(options, std::filesystem::canonical("/proc/self/exe"));
    else throw std::runtime_error("unknown phase");
  } catch (const std::exception& error) {
    emit({{"status", "fail"}, {"correctness", "fail"}, {"details", error.what()}}); return 1;
  }
}
