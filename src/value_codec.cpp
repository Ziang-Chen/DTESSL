#include "dtessl/value_codec.hpp"

#include <bit>
#include <limits>
#include <string>
#include <utility>

namespace dtessl {
namespace {

enum class Tag : std::uint8_t { Bool = 0, Int = 1, String = 2, StringSet = 3 };

void append_varuint(std::vector<std::uint8_t>& output, std::size_t value) {
  do {
    std::uint8_t byte = static_cast<std::uint8_t>(value & 0x7fU);
    value >>= 7U;
    if (value != 0) byte = static_cast<std::uint8_t>(byte | 0x80U);
    output.push_back(byte);
  } while (value != 0);
}

void append_string(std::vector<std::uint8_t>& output, const std::string& value) {
  append_varuint(output, value.size());
  output.insert(output.end(), value.begin(), value.end());
}

class Decoder {
 public:
  Decoder(std::span<const std::uint8_t> bytes, ValueCodecLimits limits)
      : bytes_(bytes), limits_(limits) {
    if (bytes_.size() > limits_.max_bytes) throw Error("canonical value exceeds byte limit");
  }

  Value value() {
    const std::uint8_t raw_tag = byte();
    switch (static_cast<Tag>(raw_tag)) {
      case Tag::Bool: {
        const std::uint8_t raw = byte();
        if (raw > 1U) throw Error("non-canonical bool value");
        return Value(raw != 0);
      }
      case Tag::Int: {
        if (remaining() < 8U) throw Error("truncated canonical int");
        std::uint64_t raw = 0;
        for (unsigned index = 0; index < 8U; ++index) {
          raw = (raw << 8U) | byte();
        }
        return Value(std::bit_cast<std::int64_t>(raw));
      }
      case Tag::String: return Value(string());
      case Tag::StringSet: {
        const std::size_t count = varuint();
        if (count > limits_.max_set_items) throw Error("canonical set exceeds item limit");
        StringSet set;
        std::string previous;
        bool first = true;
        for (std::size_t index = 0; index < count; ++index) {
          std::string item = string();
          if (!first && item <= previous) {
            throw Error("canonical set is not strictly sorted");
          }
          first = false;
          previous = item;
          set.values.insert(std::move(item));
        }
        return Value(std::move(set));
      }
    }
    throw Error("unknown canonical value tag");
  }

  void finish() const {
    if (cursor_ != bytes_.size()) throw Error("trailing bytes after canonical value");
  }

 private:
  std::size_t remaining() const noexcept { return bytes_.size() - cursor_; }

  std::uint8_t byte() {
    if (cursor_ == bytes_.size()) throw Error("truncated canonical value");
    return bytes_[cursor_++];
  }

  std::size_t varuint() {
    std::size_t value = 0;
    unsigned shift = 0;
    std::uint8_t last = 0;
    do {
      last = byte();
      if (shift >= std::numeric_limits<std::size_t>::digits ||
          static_cast<std::size_t>(last & 0x7fU) >
              (std::numeric_limits<std::size_t>::max() >> shift)) {
        throw Error("canonical length overflow");
      }
      value |= static_cast<std::size_t>(last & 0x7fU) << shift;
      shift += 7U;
    } while ((last & 0x80U) != 0);
    if (shift > 7U && (last & 0x7fU) == 0) throw Error("non-minimal canonical length");
    return value;
  }

  std::string string() {
    const std::size_t size = varuint();
    if (size > limits_.max_string_bytes) throw Error("canonical string exceeds byte limit");
    if (size > remaining()) throw Error("truncated canonical string");
    const char* begin = reinterpret_cast<const char*>(bytes_.data() + cursor_);
    cursor_ += size;
    return std::string(begin, size);
  }

  std::span<const std::uint8_t> bytes_;
  ValueCodecLimits limits_;
  std::size_t cursor_{0};
};

}  // namespace

std::vector<std::uint8_t> encode_value(const Value& value) {
  std::vector<std::uint8_t> output;
  switch (value.kind()) {
    case Value::Kind::Bool:
      output.push_back(static_cast<std::uint8_t>(Tag::Bool));
      output.push_back(value.as_bool() ? 1U : 0U);
      break;
    case Value::Kind::Int: {
      output.push_back(static_cast<std::uint8_t>(Tag::Int));
      const std::uint64_t raw = std::bit_cast<std::uint64_t>(value.as_int());
      for (int shift = 56; shift >= 0; shift -= 8) {
        output.push_back(static_cast<std::uint8_t>(raw >> static_cast<unsigned>(shift)));
      }
      break;
    }
    case Value::Kind::String:
      output.push_back(static_cast<std::uint8_t>(Tag::String));
      append_string(output, value.as_string());
      break;
    case Value::Kind::StringSet:
      output.push_back(static_cast<std::uint8_t>(Tag::StringSet));
      append_varuint(output, value.as_string_set().values.size());
      for (const std::string& item : value.as_string_set().values) append_string(output, item);
      break;
  }
  return output;
}

Value decode_value(std::span<const std::uint8_t> bytes, ValueCodecLimits limits) {
  Decoder decoder(bytes, limits);
  Value result = decoder.value();
  decoder.finish();
  return result;
}

}  // namespace dtessl
