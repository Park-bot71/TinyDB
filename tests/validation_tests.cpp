#include <gtest/gtest.h>

#include <cerrno>
#include <filesystem>
#include <fstream>
#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>

namespace {
class ValidationSuiteTest : public ::testing::Test {
 protected:
  void SetUp() override {
    auto pattern = (std::filesystem::temp_directory_path() / "tinydb-validation-test-XXXXXX").string();
    const auto created = ::mkdtemp(pattern.data()); ASSERT_NE(created, nullptr); directory = created;
  }
  void TearDown() override { std::filesystem::remove_all(directory); }
  std::pair<int, std::string> run(const std::string& phase, const std::filesystem::path& path, const std::string& method = "indexed") {
    const auto pid = ::fork(); if (pid < 0) return {-1, "fork failed"};
    if (pid == 0) {
      const int fd = ::open((directory / "output").c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
      if (fd < 0 || ::dup2(fd, 1) < 0) ::_exit(126);
      ::close(fd);
      ::execl(VALIDATION_SUITE_PATH, VALIDATION_SUITE_PATH, "--phase", phase.c_str(), "--dir", path.c_str(),
              "--rows", "64", "--lookups", "32", "--frames", "1", "--workers", "4", "--transactions", "4", "--method", method.c_str(), nullptr);
      ::_exit(127);
    }
    int status = 0; pid_t waited;
    do { waited = ::waitpid(pid, &status, 0); } while (waited < 0 && errno == EINTR);
    std::ifstream source(directory / "output"); std::string output{std::istreambuf_iterator<char>(source), {}};
    return {waited == pid && WIFEXITED(status) ? WEXITSTATUS(status) : -1, output};
  }
  std::filesystem::path directory;
};
TEST_F(ValidationSuiteTest, RealLookupSamplesConcurrencyAndBufferCounters) {
  const auto data = directory / "data";
  ASSERT_EQ(run("prepare", data).first, 0);
  const auto [index_code, index] = run("lookup", data);
  EXPECT_EQ(index_code, 0); EXPECT_NE(index.find("\"operations\":\"32\""), std::string::npos);
  const auto [scan_code, scan] = run("lookup", data, "sequential");
  EXPECT_EQ(scan_code, 0); EXPECT_NE(scan.find("\"correctness\":\"pass\""), std::string::npos);
  std::ifstream a(data / "indexed-latencies.csv"), b(data / "sequential-latencies.csv");
  std::string x, y; ASSERT_TRUE(std::getline(a, x)); ASSERT_TRUE(std::getline(b, y)); EXPECT_EQ(x, y);
  std::size_t count = 0;
  while (std::getline(a, x)) {
    ASSERT_TRUE(std::getline(b, y)); EXPECT_EQ(x.substr(0, x.find(',')), y.substr(0, y.find(','))); ++count;
  }
  EXPECT_EQ(count, 32U); EXPECT_FALSE(std::getline(b, y));
  const auto [code, transactions] = run("concurrency", directory / "transactions");
  EXPECT_EQ(code, 0); EXPECT_NE(transactions.find("\"successes\":\"16\""), std::string::npos);
  EXPECT_NE(transactions.find("\"failures\":\"0\""), std::string::npos);
  const auto [buffer_code, buffer] = run("buffer", directory / "buffer");
  EXPECT_EQ(buffer_code, 0); EXPECT_NE(buffer.find("\"heap_evictions\":\"31\""), std::string::npos);
  EXPECT_NE(buffer.find("\"capacity_per_file\":\"1\""), std::string::npos);
  EXPECT_EQ(run("prepare", data).first, 1); // Existing data must never be overwritten.
}
TEST_F(ValidationSuiteTest, EveryCrashPointActuallyExitsAndRecoversTwice) {
  const auto [code, results] = run("crashes", directory / "crashes");
  EXPECT_EQ(code, 0) << results;
  std::size_t offset = 0, passed = 0;
  while ((offset = results.find("\"status\":\"pass\"", offset)) != std::string::npos) { ++passed; ++offset; }
  EXPECT_EQ(passed, 16U) << results;
  EXPECT_NE(results.find("recovery.after_publish_heap"), std::string::npos);
  EXPECT_NE(results.find("commit.after_force"), std::string::npos);
}
}
