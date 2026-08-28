#include "dtessl/value_codec.hpp"

#include <bit>
#include <limits>
#include <string>
#include <utility>

namespace dtessl {
namespace {

enum class Tag : std::uint8_t {
  Bool = 0,
  Int = 1,
  String = 2,
  StringSet = 3,
  List = 4,
  Set = 5,
  Map = 6,
  Bag = 7,
};

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

  Value value(std::size_t depth = 0) {
    if (depth > limits_.max_depth) throw Error("canonical value exceeds depth limit");
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
      case Tag::List: {
        const std::size_t count = collection_count();
        ValueList list;
        list.values.reserve(count);
        for (std::size_t index = 0; index < count; ++index) {
          list.values.push_back(value(depth + 1U));
        }
        return Value(std::move(list));
      }
      case Tag::Set: {
        const std::size_t count = collection_count();
        ValueSet set;
        set.values.reserve(count);
        for (std::size_t index = 0; index < count; ++index) {
          Value item = value(depth + 1U);
          if (!set.values.empty() && canonical_compare(set.values.back(), item) >= 0) {
            throw Error("canonical generic set is not strictly sorted");
          }
          set.values.push_back(std::move(item));
        }
        return Value(std::move(set));
      }
      case Tag::Map: {
        const std::size_t count = collection_count();
        ValueMap map;
        map.entries.reserve(count);
        for (std::size_t index = 0; index < count; ++index) {
          Value key = value(depth + 1U);
          if (!map.entries.empty() && canonical_compare(map.entries.back().first, key) >= 0) {
            throw Error("canonical map keys are not strictly sorted");
          }
          Value item = value(depth + 1U);
          map.entries.emplace_back(std::move(key), std::move(item));
        }
        return Value(std::move(map));
      }
      case Tag::Bag: {
        const std::size_t count = collection_count();
        ValueBag bag;
        bag.entries.reserve(count);
        for (std::size_t index = 0; index < count; ++index) {
          Value item = value(depth + 1U);
          if (!bag.entries.empty() && canonical_compare(bag.entries.back().first, item) >= 0) {
            throw Error("canonical bag values are not strictly sorted");
          }
          const std::size_t multiplicity = varuint();
          if (multiplicity == 0) throw Error("canonical bag count must be positive");
          bag.entries.emplace_back(std::move(item), static_cast<std::uint64_t>(multiplicity));
        }
        return Value(std::move(bag));
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

  std::size_t collection_count() {
    const std::size_t count = varuint();
    if (count > limits_.max_set_items) throw Error("canonical collection exceeds item limit");
    return count;
  }

  std::span<const std::uint8_t> bytes_;
  ValueCodecLimits limits_;
  std::size_t cursor_{0};
};

}  // namespace

void encode_into(const Value& value, std::vector<std::uint8_t>& output,
                 const ValueCodecLimits& limits, std::size_t depth) {
  if (depth > limits.max_depth) throw Error("canonical value exceeds depth limit");
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
    case Value::Kind::List:
      output.push_back(static_cast<std::uint8_t>(Tag::List));
      if (value.as_list().values.size() > limits.max_set_items) {
        throw Error("canonical collection exceeds item limit");
      }
      append_varuint(output, value.as_list().values.size());
      for (const Value& item : value.as_list().values) {
        encode_into(item, output, limits, depth + 1U);
      }
      break;
    case Value::Kind::Set:
      output.push_back(static_cast<std::uint8_t>(Tag::Set));
      if (value.as_set().values.size() > limits.max_set_items) {
        throw Error("canonical collection exceeds item limit");
      }
      append_varuint(output, value.as_set().values.size());
      for (const Value& item : value.as_set().values) {
        encode_into(item, output, limits, depth + 1U);
      }
      break;
    case Value::Kind::Map:
      output.push_back(static_cast<std::uint8_t>(Tag::Map));
      if (value.as_map().entries.size() > limits.max_set_items) {
        throw Error("canonical collection exceeds item limit");
      }
      append_varuint(output, value.as_map().entries.size());
      for (const auto& [key, item] : value.as_map().entries) {
        encode_into(key, output, limits, depth + 1U);
        encode_into(item, output, limits, depth + 1U);
      }
      break;
    case Value::Kind::Bag:
      output.push_back(static_cast<std::uint8_t>(Tag::Bag));
      if (value.as_bag().entries.size() > limits.max_set_items) {
        throw Error("canonical collection exceeds item limit");
      }
      append_varuint(output, value.as_bag().entries.size());
      for (const auto& [item, count] : value.as_bag().entries) {
        encode_into(item, output, limits, depth + 1U);
        if (count > std::numeric_limits<std::size_t>::max()) {
          throw Error("bag count exceeds canonical platform limit");
        }
        append_varuint(output, static_cast<std::size_t>(count));
      }
      break;
  }
  if (output.size() > limits.max_bytes) throw Error("canonical value exceeds byte limit");
}

std::vector<std::uint8_t> encode_value(const Value& value, ValueCodecLimits limits) {
  std::vector<std::uint8_t> output;
  encode_into(value, output, limits, 0);
  return output;
}

Value decode_value(std::span<const std::uint8_t> bytes, ValueCodecLimits limits) {
  Decoder decoder(bytes, limits);
  Value result = decoder.value();
  decoder.finish();
  return result;
}

}  // namespace dtessl
