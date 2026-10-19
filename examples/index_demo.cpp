#include <iostream>

#include "index/b_plus_tree.h"
#include "storage/heap_file.h"
#include "storage/record.h"

int main(int argc, char** argv) {
  if (argc != 3) {
    std::cerr << "Usage: index_demo HEAP_FILE INDEX_FILE\n";
    return 2;
  }
  try {
    tinydb::HeapFile heap(argv[1]);
    tinydb::BPlusTree index(argv[2]);
    const auto existing = heap.scan();
    if (existing.size() != index.size()) throw std::runtime_error("heap/index entry counts disagree; use a matched pair of files");
    if (existing.empty()) {
      for (const auto& record : {tinydb::Record{2, "Bob", 500}, tinydb::Record{1, "Alice", 1000},
                                 tinydb::Record{3, "Carol", 750}}) {
        const auto rid = heap.insert(tinydb::serialize(record));
        if (!index.insert(record.key, rid)) throw std::runtime_error("unexpected duplicate key");
      }
    }
    (void)index.validate();
    for (const auto& [key, rid] : index.scan()) {
      const auto record = tinydb::deserialize(heap.get(rid));
      if (record.key != key) throw std::runtime_error("heap and index keys disagree");
      std::cout << key << " | " << record.name << " | " << record.balance << '\n';
    }
    heap.close();
    index.close();  // Two checked closes, not an atomic transaction.
  } catch (const std::exception& error) {
    std::cerr << "Index demo error: " << error.what() << '\n';
    return 1;
  }
}
