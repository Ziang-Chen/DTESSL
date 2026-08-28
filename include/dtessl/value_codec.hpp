#pragma once

#include "dtessl/dtessl.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace dtessl {

struct ValueCodecLimits {
  std::size_t max_bytes{16U * 1024U * 1024U};
  std::size_t max_string_bytes{1024U * 1024U};
  std::size_t max_set_items{1024U * 1024U};
  std::size_t max_depth{64};
};

// Canonical format v1. Encoding is injective for every currently supported
// Value. The decoder rejects non-minimal lengths, unsorted sets, trailing bytes
// and inputs outside explicit limits.
[[nodiscard]] std::vector<std::uint8_t> encode_value(
    const Value& value, ValueCodecLimits limits = {});
[[nodiscard]] Value decode_value(std::span<const std::uint8_t> bytes,
                                 ValueCodecLimits limits = {});

}  // namespace dtessl
