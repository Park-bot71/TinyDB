#include "storage/page.h"

#include "common/encoding.h"

namespace tinydb {
namespace {
constexpr std::uint32_t kMagic = 0x31424454;  // TDB1 in little endian
std::uint32_t checksum(const Page& page) {
  std::uint32_t crc = 0xffffffffU;
  for (std::size_t i = 0; i < kPageSize; ++i) {
    const auto byte = (i >= 12 && i < 16) ? 0U : std::to_integer<std::uint32_t>(page.bytes[i]);
    crc ^= byte;
    for (int bit = 0; bit < 8; ++bit)
      crc = (crc >> 1) ^ (0xedb88320U & (0U - (crc & 1U)));
  }
  return ~crc;
}
}  // namespace

Page Page::create(PageId id, PageType type) {
  Page page;
  encoding::store<std::uint32_t>(page.bytes, 0, kMagic);
  encoding::store<std::uint16_t>(page.bytes, 4, kFormatVersion);
  encoding::store<std::uint16_t>(page.bytes, 6, static_cast<std::uint16_t>(type));
  encoding::store<PageId>(page.bytes, 8, id);
  if (type == PageType::Heap)
    encoding::store<std::uint16_t>(page.bytes, 26, kPageSize);
  page.seal();
  return page;
}

PageId Page::id() const { return encoding::load<PageId>(bytes, 8); }
PageType Page::type() const { return static_cast<PageType>(encoding::load<std::uint16_t>(bytes, 6)); }
void Page::seal() { encoding::store<std::uint32_t>(bytes, 12, checksum(*this)); }

void Page::validate(PageId expected_id) const {
  if (encoding::load<std::uint32_t>(bytes, 0) != kMagic ||
      encoding::load<std::uint16_t>(bytes, 4) != kFormatVersion || id() != expected_id ||
      encoding::load<std::uint16_t>(bytes, 6) > static_cast<std::uint16_t>(PageType::Free) ||
      ((expected_id == 0) != (type() == PageType::Metadata)) ||
      encoding::load<std::uint32_t>(bytes, 12) != checksum(*this))
    throw Error(ErrorCode::Corruption, "invalid page header or checksum at page " + std::to_string(expected_id));
}
}  // namespace tinydb
