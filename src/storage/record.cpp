#include "storage/record.h"

#include <bit>
#include <cstring>

#include "common/encoding.h"

namespace tinydb {
Bytes serialize(const Record& record) {
  constexpr std::size_t header = 20;
  if (record.name.size() > SlottedPage::kMaxRecordSize - header)
    throw Error(ErrorCode::InvalidArgument, "name exceeds single-page record capacity");
  Bytes bytes(header + record.name.size());
  encoding::store<std::uint16_t>(bytes, 0, 1);
  encoding::store<std::uint16_t>(bytes, 2, static_cast<std::uint16_t>(record.name.size()));
  encoding::store<std::uint64_t>(bytes, 4, std::bit_cast<std::uint64_t>(record.key));
  encoding::store<std::uint64_t>(bytes, 12, std::bit_cast<std::uint64_t>(record.balance));
  std::memcpy(bytes.data() + header, record.name.data(), record.name.size());
  return bytes;
}
Record deserialize(std::span<const std::byte> bytes) {
  if (bytes.size() < 20 || bytes.size() > SlottedPage::kMaxRecordSize ||
      encoding::load<std::uint16_t>(bytes, 0) != 1 ||
      bytes.size() != 20U + encoding::load<std::uint16_t>(bytes, 2))
    throw Error(ErrorCode::Corruption, "invalid typed record length or version");
  return {std::bit_cast<std::int64_t>(encoding::load<std::uint64_t>(bytes, 4)),
          std::string(reinterpret_cast<const char*>(bytes.data() + 20), bytes.size() - 20),
          std::bit_cast<std::int64_t>(encoding::load<std::uint64_t>(bytes, 12))};
}
}  // namespace tinydb
