#include <fstream>
#include <string>
#include <unistd.h>

#include "transaction/database.h"

int main(int argc, char** argv) {
  if (argc != 4) return 2;
  const std::string scenario = argv[2], point = argv[3];
  bool committed = false, aborted = false;
  auto crash = [&](std::string_view event) {
    if (event == "commit.after_force") committed = true;
    if (event == "abort.after_force") aborted = true;
    if (event == point || (point == "torn_after_commit" && committed && event == "data.heap.after_half_write") ||
        (point == "torn_during_abort" && aborted && event == "data.heap.after_half_write")) ::_exit(86);
  };
  try {
    tinydb::DatabaseOptions options{1, 0, std::chrono::seconds(2), crash};
    tinydb::TransactionalDatabase db(argv[1], options);
    if (scenario == "recover") { (void)db.validate(); db.close(); return 0; }
    auto tx = db.begin();
    { std::ofstream out(std::filesystem::path(argv[1]) / "worker.txid"); out << tx.id(); out.close(); if (!out) return 3; }
    if (scenario == "structural") {
      for (const auto& row : tx.scan()) tx.erase(row.key);
      for (tinydb::IndexKey key = 100; key < 120; ++key) tx.insert({key, std::string(4032, 'n'), key});
    } else {
      auto alice = tx.get_for_update(1), bob = tx.get_for_update(2);
      if (!alice || !bob) return 4;
      alice->balance = 800; tx.update(1, *alice); crash("between_updates");
      bob->balance = 700; tx.update(2, *bob);
    }
    crash("before_commit");
    if (scenario == "abort") tx.rollback(); else tx.commit();
    if (scenario == "no_close") ::_exit(86);
    db.close();
    return 0;
  } catch (...) { return 10; }
}
