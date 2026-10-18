#pragma once

#include <cstdint>
#include <span>
#include <string>

#include "storage/slotted_page.h"

namespace tinydb {
struct Record {
  std::int64_t key;
  std::string name;
  std::int64_t balance;
  bool operator==(const Record&) const = default;
};
// Names are byte strings (embedded NUL is supported), not validated Unicode.
Bytes serialize(const Record& record);
Record deserialize(std::span<const std::byte> bytes);
}  // namespace tinydb
