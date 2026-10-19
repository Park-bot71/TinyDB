#include <gtest/gtest.h>

#include <cerrno>
#include <csignal>
#include <fcntl.h>
#include <fstream>
#include <sstream>
#include <sys/wait.h>
#include <unistd.h>

#include "cli/shell.h"

namespace tinydb {
namespace {
class CliTest : public ::testing::Test {
 protected:
  void SetUp() override {
    auto pattern = (std::filesystem::temp_directory_path() / "tinydb-cli-XXXXXX").string();
    const auto path = ::mkdtemp(pattern.data()); ASSERT_NE(path, nullptr); dir = path;
  }
  void TearDown() override { std::filesystem::remove_all(dir); }
  int run(const std::string& text, std::string* result = nullptr, std::string* diagnostics = nullptr) {
    std::istringstream input(text); std::ostringstream out, error;
    const int status = run_shell(dir / "database", input, out, error);
    if (result) *result = out.str();
    if (diagnostics) *diagnostics = error.str();
    return status;
  }
  int wait(pid_t pid) {
    int status = 0; pid_t result;
    do { result = ::waitpid(pid, &status, 0); } while (result < 0 && errno == EINTR);
    return result == pid && WIFEXITED(status) ? WEXITSTATUS(status) : -1;
  }
  std::string file(const char* name) {
    std::ifstream input(dir / name); return {std::istreambuf_iterator<char>(input), {}};
  }
  std::filesystem::path dir;
};
TEST_F(CliTest, AccountTransferRollbackAndRestart) {
  std::string output, errors;
  EXPECT_EQ(run("CREATE TABLE accounts\nBEGIN\nINSERT accounts 1 \"Alice\" 1000\nINSERT accounts 2 \"Bob\" 500\nCOMMIT\n"
                "BEGIN\nUPDATE accounts 1 \"Alice\" 800\nUPDATE accounts 2 \"Bob\" 700\nCOMMIT\n"
                "BEGIN\nUPDATE accounts 1 \"Alice\" 0\nDELETE accounts 2\nROLLBACK\nSELECT accounts *\nCHECK\nEXIT\n", &output, &errors), 0);
  EXPECT_TRUE(errors.empty()); EXPECT_NE(output.find("1 | Alice | 800\n2 | Bob | 700\n2 row(s)"), std::string::npos);
  EXPECT_NE(output.find("OK entries=2"), std::string::npos);
  EXPECT_EQ(run("SELECT accounts 2\nRANGE accounts 1 2 1\n", &output), 0);
  EXPECT_NE(output.find("2 | Bob | 700"), std::string::npos); EXPECT_NE(output.find("1 | Alice | 800"), std::string::npos);
}
TEST_F(CliTest, EscapesCaseCommentsAndIntegerLimits) {
  std::string output;
  EXPECT_EQ(run("# comment\ncreate table accounts; # comment\n"
                "insert accounts -9223372036854775808 \"A \\\"quote\\\"\\n\\t\\0\\\\\" -9223372036854775808;\n"
                "INSERT accounts 9223372036854775807 \"\" 9223372036854775807\nselect accounts *\n", &output), 0);
  EXPECT_NE(output.find("-9223372036854775808 | A \"quote\"\\n\\t\\0\\\\ | -9223372036854775808"), std::string::npos);
  EXPECT_NE(output.find("9223372036854775807 |  | 9223372036854775807"), std::string::npos);
}
TEST_F(CliTest, SyntaxErrorsDoNotPartiallyExecuteOrAbortActiveWork) {
  std::string output, errors;
  EXPECT_EQ(run("BEGIN\nINSERT accounts 1 \"valid\" 10\n"
                "INSERT accounts 2 \"bad\" 20 trailing\nINSERT accounts 2 \"bad\" 9223372036854775808\n"
                "INSERT accounts 2 \"bad\\z\" 20\nINSERT accounts 2 \"unterminated\n"
                "INSERT accounts 2 \"bad\" 20; DELETE accounts 1\n"
                "SELECT accounts * LIMIT -1\nCOMMIT\nSELECT accounts *\n", &output, &errors), 1);
  EXPECT_NE(errors.find("InvalidArgument"), std::string::npos);
  EXPECT_NE(output.find("1 | valid | 10\n1 row(s)"), std::string::npos);
  EXPECT_EQ(run("CHECK\n", &output), 0); EXPECT_NE(output.find("entries=1"), std::string::npos);
}
TEST_F(CliTest, EngineErrorAbortsEntireTransactionAndNextCommandAutocommits) {
  std::string output, errors;
  EXPECT_EQ(run("BEGIN\nINSERT accounts 1 \"first\" 10\nINSERT accounts 1 \"duplicate\" 20\n"
                "INSERT accounts 2 \"survivor\" 30\nSELECT accounts *\n", &output, &errors), 1);
  EXPECT_NE(errors.find("DuplicateKey"), std::string::npos);
  EXPECT_NE(errors.find("Transaction rolled back"), std::string::npos);
  EXPECT_NE(output.find("2 | survivor | 30\n1 row(s)"), std::string::npos);
  EXPECT_EQ(output.find("1 | first"), std::string::npos);
}
TEST_F(CliTest, EofAndExitExplicitlyRollbackUnfinishedWork) {
  std::string output;
  EXPECT_EQ(run("BEGIN\nINSERT accounts 1 \"unfinished\" 1\n", &output), 0);
  EXPECT_NE(output.find("ROLLBACK (unfinished transaction)"), std::string::npos);
  EXPECT_EQ(run("BEGIN\nINSERT accounts 2 \"unfinished\" 2\nQUIT\nCOMMIT\n", &output), 0);
  EXPECT_EQ(run("SELECT accounts *\n", &output), 0); EXPECT_EQ(output, "0 row(s)\n");
}
TEST_F(CliTest, RejectsUnknownTablesAndTransactionMisuse) {
  std::string output, errors;
  EXPECT_EQ(run("CREATE TABLE other\nSELECT other 1\nCOMMIT\nROLLBACK\nBEGIN\nBEGIN\n"
                "CREATE TABLE accounts\nCHECK\nFLUSH\nROLLBACK\nCHECK\n", &output, &errors), 1);
  EXPECT_NE(errors.find("unknown table"), std::string::npos);
  EXPECT_NE(errors.find("Busy"), std::string::npos); EXPECT_NE(output.find("OK entries=0"), std::string::npos);
}
TEST_F(CliTest, HelpStatsFlushAndEmptyRanges) {
  std::string output;
  EXPECT_EQ(run("HELP\nINSERT accounts 1 \"one\" 1\nSELECT accounts 99\nSELECT accounts * LIMIT 0\n"
                "RANGE accounts 2 2\nSTATS\nFLUSH\nCHECK\n", &output), 0);
  EXPECT_NE(output.find("wal_records="), std::string::npos);
  EXPECT_NE(output.find("half-open"), std::string::npos); EXPECT_NE(output.find("FLUSH\nOK entries=1"), std::string::npos);
}
TEST_F(CliTest, ExecutableBatchReportsErrorsAndPersistsOtherCommands) {
  { std::ofstream input(dir / "input"); input << "INSERT accounts 1 \"saved\" 42\nNONSENSE\nEXIT\n"; }
  const auto pid = ::fork(); ASSERT_GE(pid, 0);
  if (pid == 0) {
    const int in = ::open((dir / "input").c_str(), O_RDONLY);
    const int out = ::open((dir / "output").c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
    const int err = ::open((dir / "error").c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (in < 0 || out < 0 || err < 0 || ::dup2(in, 0) < 0 || ::dup2(out, 1) < 0 || ::dup2(err, 2) < 0) ::_exit(126);
    ::close(in); ::close(out); ::close(err);
    ::execl(TINYDB_SHELL_PATH, TINYDB_SHELL_PATH, (dir / "database").c_str(), "--batch", nullptr); ::_exit(127);
  }
  EXPECT_EQ(wait(pid), 1); EXPECT_EQ(file("output"), "OK\n"); EXPECT_NE(file("error").find("unknown command"), std::string::npos);
  std::string output; EXPECT_EQ(run("SELECT accounts 1\n", &output), 0); EXPECT_NE(output.find("1 | saved | 42"), std::string::npos);
}
TEST_F(CliTest, ShutdownSignalsDuringPartialInputRollBackAndClose) {
  for (int signal : {SIGINT, SIGTERM}) {
  int input[2], output[2]; ASSERT_EQ(::pipe(input), 0); ASSERT_EQ(::pipe(output), 0);
  const auto pid = ::fork(); ASSERT_GE(pid, 0);
  if (pid == 0) {
    ::close(input[1]); ::close(output[0]);
    if (::dup2(input[0], 0) < 0 || ::dup2(output[1], 1) < 0) ::_exit(126);
    ::close(input[0]); ::close(output[1]);
    ::execl(TINYDB_SHELL_PATH, TINYDB_SHELL_PATH, (dir / "database").c_str(), "--batch", nullptr); ::_exit(127);
  }
  ::close(input[0]); ::close(output[1]);
  const std::string commands = "BEGIN\nINSERT accounts 1 \"pending\" 12\nSELECT accounts";
  EXPECT_EQ(::write(input[1], commands.data(), commands.size()), static_cast<ssize_t>(commands.size()));
  std::string received;
  while (received.find("OK\n") == std::string::npos) {
    char bytes[128]; const auto size = ::read(output[0], bytes, sizeof(bytes));
    if (size < 0 && errno == EINTR) continue;
    if (size <= 0) break;
    received.append(bytes, static_cast<std::size_t>(size));
  }
  EXPECT_NE(received.find("OK\n"), std::string::npos);
  EXPECT_EQ(::kill(pid, signal), 0); EXPECT_EQ(wait(pid), 128 + signal);
  ::close(input[1]); ::close(output[0]);
  std::string rows; EXPECT_EQ(run("SELECT accounts *\n", &rows), 0); EXPECT_EQ(rows, "0 row(s)\n");
  }
}
TEST_F(CliTest, BenchmarkExercisesAllWorkloadsAndRefusesExistingDirectory) {
  auto invoke = [&] {
    const auto pid = ::fork(); if (pid < 0) return -1;
    if (pid == 0) {
      const int out = ::open((dir / "benchmark.csv").c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
      if (out < 0 || ::dup2(out, 1) < 0) ::_exit(126);
      ::close(out);
      ::execl(TINYDB_BENCH_PATH, TINYDB_BENCH_PATH, "--dir", (dir / "bench").c_str(), "--rows", "12", "--lookups", "6",
              "--transactions", "2", "--recovery-transactions", "2", "--clients", "2", "--frames", "1", nullptr); ::_exit(127);
    }
    return wait(pid);
  };
  ASSERT_EQ(invoke(), 0); const auto csv = file("benchmark.csv");
  EXPECT_NE(csv.find("raw_heap_scan_lookup"), std::string::npos);
  EXPECT_NE(csv.find("durable_update_2_clients"), std::string::npos);
  EXPECT_NE(csv.find("recovery_stage2_sample2"), std::string::npos);
  EXPECT_EQ(invoke(), 1); EXPECT_TRUE(std::filesystem::exists(dir / "bench" / "transactions" / "database.wal"));
}
}  // namespace
}  // namespace tinydb
