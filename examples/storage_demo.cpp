#include <iostream>

#include "storage/heap_file.h"
#include "storage/record.h"

int main(int argc, char** argv) {
  if (argc != 2) {
    std::cerr << "Usage: storage_demo DATABASE_FILE\n";
    return 2;
  }
  try {
    tinydb::HeapFile heap(argv[1]);
    if (heap.scan().empty()) {
      (void)heap.insert(tinydb::serialize({1, "Alice", 1000}));
      (void)heap.insert(tinydb::serialize({2, "Bob", 500}));
    }
    for (const auto& [rid, bytes] : heap.scan()) {
      const auto record = tinydb::deserialize(bytes);
      std::cout << record.key << " | " << record.name << " | " << record.balance
                << " [RID " << rid.page << ':' << rid.slot << ':' << rid.generation << "]\n";
    }
    heap.close();  // Surface flush/close errors before reporting success.
  } catch (const std::exception& error) {
    std::cerr << "Storage error: " << error.what() << '\n';
    return 1;
  }
}
