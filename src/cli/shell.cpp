#include "cli/shell.h"

#include <charconv>
#include <cctype>
#include <istream>
#include <optional>
#include <ostream>
#include <string>
#include <string_view>
#include <vector>

#include "common/error.h"
#include "transaction/database.h"

namespace tinydb {
namespace {
struct Token { std::string text; bool quoted = false; };
[[noreturn]] void syntax(const std::string& message) { throw Error(ErrorCode::InvalidArgument, message); }
std::string upper(std::string value) {
  for (auto& c : value) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
  return value;
}
std::vector<Token> tokenize(std::string_view line) {
  std::vector<Token> tokens;
  std::size_t pos = 0;
  while (pos < line.size()) {
    if (std::isspace(static_cast<unsigned char>(line[pos]))) { ++pos; continue; }
    if (line[pos] == '#') break;
    if (line[pos] == ';') {
      ++pos;
      while (pos < line.size() && std::isspace(static_cast<unsigned char>(line[pos]))) ++pos;
      if (pos != line.size() && line[pos] != '#') syntax("one command per line; semicolon must be last");
      break;
    }
    Token token;
    if (line[pos] == '"') {
      token.quoted = true; ++pos; bool closed = false;
      while (pos < line.size()) {
        auto c = line[pos++];
        if (c == '"') { closed = true; break; }
        if (c == '\\') {
          if (pos == line.size()) syntax("unfinished quoted escape");
          c = line[pos++];
          switch (c) {
            case 'n': c = '\n'; break; case 'r': c = '\r'; break;
            case 't': c = '\t'; break; case '0': c = '\0'; break;
            case '\\': case '"': break; default: syntax("unsupported quoted escape");
          }
        }
        token.text += c;
      }
      if (!closed) syntax("unterminated quoted name");
      if (pos < line.size() && !std::isspace(static_cast<unsigned char>(line[pos])) && line[pos] != ';' && line[pos] != '#')
        syntax("expected whitespace after quoted name");
    } else {
      const auto start = pos;
      while (pos < line.size() && !std::isspace(static_cast<unsigned char>(line[pos])) && line[pos] != ';' && line[pos] != '#') {
        if (line[pos] == '"') syntax("quote must start a token");
        ++pos;
      }
      token.text = line.substr(start, pos - start);
    }
    tokens.push_back(std::move(token));
  }
  return tokens;
}
template <typename Integer> Integer number(const Token& token) {
  Integer value{};
  const auto [end, error] = std::from_chars(token.text.data(), token.text.data() + token.text.size(), value);
  if (token.quoted || error != std::errc{} || end != token.text.data() + token.text.size())
    syntax("invalid or out-of-range integer: " + token.text);
  return value;
}
enum class Kind { Help, Create, Begin, Commit, Rollback, Insert, Update, Delete, Select, Scan, Stats, Check, Flush, Exit };
struct Command {
  explicit Command(Kind value) : kind(value) {}
  Kind kind;
  IndexKey key = 0;
  std::string name;
  std::int64_t balance = 0;
  std::optional<IndexKey> lower, upper;
  std::size_t limit = std::numeric_limits<std::size_t>::max();
};
Command parse(const std::vector<Token>& tokens) {
  const auto verb = upper(tokens[0].text);
  if (tokens[0].quoted) syntax("command keyword cannot be quoted");
  auto count = [&](std::size_t n, const char* usage) { if (tokens.size() != n) syntax(usage); };
  auto table = [&](std::size_t i) {
    if (tokens[i].quoted || tokens[i].text != "accounts") syntax("unknown table; the fixed table is accounts");
  };
  for (const auto& [word, kind] : std::vector<std::pair<std::string_view, Kind>>{
      {"HELP", Kind::Help}, {"BEGIN", Kind::Begin}, {"COMMIT", Kind::Commit}, {"ROLLBACK", Kind::Rollback},
      {"STATS", Kind::Stats}, {"CHECK", Kind::Check}, {"FLUSH", Kind::Flush}, {"EXIT", Kind::Exit}, {"QUIT", Kind::Exit}}) {
    if (verb == word) { count(1, "command takes no arguments"); return Command{kind}; }
  }
  if (verb == "CREATE") {
    count(3, "usage: CREATE TABLE accounts");
    if (tokens[1].quoted || upper(tokens[1].text) != "TABLE") syntax("usage: CREATE TABLE accounts");
    table(2); return Command{Kind::Create};
  }
  if (verb == "INSERT" || verb == "UPDATE") {
    count(5, "usage: INSERT|UPDATE accounts KEY \"NAME\" BALANCE"); table(1);
    if (!tokens[3].quoted) syntax("NAME must be double quoted");
    Command command{verb == "INSERT" ? Kind::Insert : Kind::Update};
    command.key = number<IndexKey>(tokens[2]); command.name = tokens[3].text;
    command.balance = number<std::int64_t>(tokens[4]); return command;
  }
  if (verb == "DELETE") {
    count(3, "usage: DELETE accounts KEY"); table(1);
    Command command{Kind::Delete}; command.key = number<IndexKey>(tokens[2]); return command;
  }
  if (verb == "SELECT") {
    if (tokens.size() != 3 && tokens.size() != 5) syntax("usage: SELECT accounts KEY|* [LIMIT N]");
    table(1); Command command{Kind::Select};
    if (!tokens[2].quoted && tokens[2].text == "*") command.kind = Kind::Scan;
    else command.key = number<IndexKey>(tokens[2]);
    if (tokens.size() == 5) {
      if (command.kind != Kind::Scan || tokens[3].quoted || upper(tokens[3].text) != "LIMIT") syntax("LIMIT requires SELECT accounts *");
      command.limit = number<std::size_t>(tokens[4]);
    }
    return command;
  }
  if (verb == "RANGE") {
    if (tokens.size() != 4 && tokens.size() != 5) syntax("usage: RANGE accounts LOWER UPPER [LIMIT]");
    table(1); Command command{Kind::Scan};
    command.lower = number<IndexKey>(tokens[2]); command.upper = number<IndexKey>(tokens[3]);
    if (*command.lower > *command.upper) syntax("range lower bound exceeds upper bound");
    if (tokens.size() == 5) command.limit = number<std::size_t>(tokens[4]);
    return command;
  }
  syntax("unknown command; type HELP");
}
const char* error_name(ErrorCode code) {
  switch (code) {
    case ErrorCode::Io: return "Io"; case ErrorCode::Corruption: return "Corruption";
    case ErrorCode::InvalidArgument: return "InvalidArgument"; case ErrorCode::NotFound: return "NotFound";
    case ErrorCode::PageFull: return "PageFull"; case ErrorCode::Busy: return "Busy";
    case ErrorCode::Closed: return "Closed"; case ErrorCode::LockTimeout: return "LockTimeout";
    case ErrorCode::TransactionAborted: return "TransactionAborted";
    case ErrorCode::InvalidTransactionState: return "InvalidTransactionState";
    case ErrorCode::DuplicateKey: return "DuplicateKey"; case ErrorCode::TransactionFailed: return "TransactionFailed";
  }
  return "Unknown";
}
void print_record(std::ostream& out, const Record& record) {
  out << record.key << " | ";
  for (const char c : record.name) {
    switch (c) {
      case '\n': out << "\\n"; break; case '\r': out << "\\r"; break;
      case '\t': out << "\\t"; break; case '\0': out << "\\0"; break;
      case '\\': out << "\\\\"; break; default: out << c;
    }
  }
  out << " | " << record.balance << '\n';
}
constexpr auto help =
    "CREATE TABLE accounts (idempotent; one fixed-schema table)\n"
    "BEGIN | COMMIT | ROLLBACK\n"
    "INSERT|UPDATE accounts KEY \"NAME\" BALANCE\n"
    "DELETE accounts KEY\nSELECT accounts KEY\nSELECT accounts * [LIMIT N]\n"
    "RANGE accounts LOWER UPPER [LIMIT] (half-open)\n"
    "STATS | CHECK | FLUSH | HELP | EXIT | QUIT\n"
    "One command per line; optional trailing semicolon; # comments.\n"
    "Commands outside BEGIN autocommit. EOF/EXIT roll back unfinished work.\n";
}  // namespace

int run_shell(const std::filesystem::path& directory, std::istream& input,
              std::ostream& output, std::ostream& errors, bool prompts,
              const volatile std::sig_atomic_t* interrupted) {
  bool had_error = false;
  try {
    TransactionalDatabase db(directory);
    std::optional<Transaction> active;
    if (prompts) output << "TinyDB: fixed table accounts. Type HELP for commands.\n";
    std::string line;
    while (!(interrupted && *interrupted)) {
      if (prompts) output << (active ? "tx> " : "db> ") << std::flush;
      if (!std::getline(input, line)) {
        if (!input.eof() && !(interrupted && *interrupted)) throw std::runtime_error("input read failed");
        break;
      }
      if (interrupted && *interrupted) break;
      try {
        const auto tokens = tokenize(line); if (tokens.empty()) continue;
        const auto command = parse(tokens);
        if (command.kind == Kind::Exit) break;
        switch (command.kind) {
          case Kind::Help: output << help; break;
          case Kind::Create:
            if (active) syntax("CREATE requires no active transaction");
            output << "OK TABLE accounts\n"; break;
          case Kind::Begin:
            if (active) syntax("a transaction is already active");
            active.emplace(db.begin()); output << "BEGIN\n"; break;
          case Kind::Commit:
          case Kind::Rollback:
            if (!active) syntax("no active transaction");
            if (command.kind == Kind::Commit) active->commit(); else active->rollback();
            active.reset(); output << (command.kind == Kind::Commit ? "COMMIT\n" : "ROLLBACK\n"); break;
          case Kind::Stats: {
            const auto stats = db.wal_stats();
            output << "wal_records=" << stats.records << " updates=" << stats.updates
                   << " durable_lsn=" << stats.durable_lsn << " recovered_commits=" << stats.recovered_commits
                   << " recovered_losers=" << stats.recovered_losers << " redo_pages=" << stats.redo_pages << '\n'; break;
          }
          case Kind::Check: output << "OK entries=" << db.validate().entries << '\n'; break;
          case Kind::Flush: db.sync(); output << "FLUSH\n"; break;
          default: {
            std::optional<Transaction> automatic;
            if (!active) automatic.emplace(db.begin());
            auto& tx = active ? *active : *automatic;
            // Buffer query output until its autocommit has succeeded.
            std::vector<Record> rows;
            if (command.kind == Kind::Insert) tx.insert({command.key, command.name, command.balance});
            else if (command.kind == Kind::Update) tx.update(command.key, {command.key, command.name, command.balance});
            else if (command.kind == Kind::Delete) tx.erase(command.key);
            else if (command.kind == Kind::Select) { if (auto row = tx.get(command.key)) rows.push_back(*row); }
            else rows = tx.scan(command.lower, command.upper, command.limit);
            if (automatic) automatic->commit();
            if (command.kind == Kind::Select || command.kind == Kind::Scan) {
              for (const auto& row : rows) print_record(output, row);
              output << rows.size() << " row(s)\n";
            } else output << "OK\n";
          }
        }
      } catch (const Error& error) {
        had_error = true; errors << "ERROR " << error_name(error.code()) << ": " << error.what() << '\n';
        if (active && active->state() != TransactionState::Active) {
          errors << (active->state() == TransactionState::Aborted ? "Transaction rolled back\n" : "Transaction failed; reopen for recovery\n");
          active.reset();
        }
        if (error.code() == ErrorCode::Io || error.code() == ErrorCode::Corruption || error.code() == ErrorCode::TransactionFailed) throw;
      }
      output.flush();
      if (!output) throw std::runtime_error("output write failed");
    }
    if (active) { active->rollback(); active.reset(); output << "ROLLBACK (unfinished transaction)\n"; }
    db.close(); output.flush();
    if (!output) throw std::runtime_error("output write failed");
    if (interrupted && *interrupted) return 128 + *interrupted;
    return had_error ? 1 : 0;
  } catch (const std::exception& error) {
    errors << "FATAL: " << error.what() << '\n'; return 1;
  }
}
}  // namespace tinydb
