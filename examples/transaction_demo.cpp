#include <iostream>

#include "transaction/database.h"

int main(int argc, char** argv) {
  if (argc != 2) { std::cerr << "Usage: transaction_demo DATABASE_DIRECTORY\n"; return 2; }
  try {
    tinydb::TransactionalDatabase db(argv[1]);
    {
      auto tx = db.begin();
      if (tx.scan().empty()) { tx.insert({1, "Alice", 1000}); tx.insert({2, "Bob", 500}); }
      tx.commit();
    }
    {
      auto tx = db.begin();
      auto alice = tx.get_for_update(1), bob = tx.get_for_update(2);
      if (!alice || !bob) throw std::runtime_error("demo requires keys 1 and 2");
      alice->balance = 800; bob->balance = 700;
      tx.update(1, *alice); tx.update(2, *bob); tx.commit();
      std::cout << "Committed both account updates\n";
    }
    {
      auto tx = db.begin();
      tx.update(1, {1, "Alice", 0}); tx.update(2, {2, "Bob", 0}); tx.rollback();
      std::cout << "Rolled back both temporary updates\n";
    }
    auto tx = db.begin();
    for (const auto& row : tx.scan()) std::cout << row.key << " | " << row.name << " | " << row.balance << '\n';
    tx.commit();
    (void)db.validate();
    db.close();
  } catch (const std::exception& error) { std::cerr << "Transaction error: " << error.what() << '\n'; return 1; }
}
