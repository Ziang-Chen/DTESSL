#include "dtessl/solver.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <iomanip>
#include <limits>
#include <sstream>

namespace dtessl {
namespace {

constexpr std::array<std::uint8_t, 8> embedding_magic{
    'D', 'T', 'E', 'M', 'B', 'E', 'D', 1U};

void append_varuint(std::vector<std::uint8_t>& output, std::size_t value) {
  do {
    std::uint8_t byte = static_cast<std::uint8_t>(value & 0x7fU);
    value >>= 7U;
    if (value != 0U) byte = static_cast<std::uint8_t>(byte | 0x80U);
    output.push_back(byte);
  } while (value != 0U);
}

void append_string(std::vector<std::uint8_t>& output, std::string_view value,
                   const EmbeddingCodecLimits& limits) {
  if (value.size() > limits.max_name_bytes) {
    throw Error("embedding name exceeds byte limit");
  }
  append_varuint(output, value.size());
  output.insert(output.end(), value.begin(), value.end());
}

class Decoder {
 public:
  Decoder(std::span<const std::uint8_t> bytes, EmbeddingCodecLimits limits)
      : bytes_(bytes), limits_(limits) {
    if (bytes.size() > limits.max_bytes) {
      throw Error("embedding exceeds byte limit");
    }
  }

  std::uint8_t byte() {
    if (cursor_ == bytes_.size()) throw Error("truncated embedding");
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
        throw Error("embedding length overflow");
      }
      value |= static_cast<std::size_t>(last & 0x7fU) << shift;
      shift += 7U;
    } while ((last & 0x80U) != 0U);
    if (shift > 7U && (last & 0x7fU) == 0U) {
      throw Error("non-minimal embedding length");
    }
    return value;
  }

  std::string string() {
    const std::size_t size = varuint();
    if (size > limits_.max_name_bytes || size > remaining()) {
      throw Error("invalid embedding string length");
    }
    const char* begin = reinterpret_cast<const char*>(bytes_.data() + cursor_);
    cursor_ += size;
    return std::string(begin, size);
  }

  std::span<const std::uint8_t> blob() {
    const std::size_t size = varuint();
    if (size > remaining()) throw Error("truncated embedding value");
    const auto result = bytes_.subspan(cursor_, size);
    cursor_ += size;
    return result;
  }

  std::size_t count() {
    const std::size_t value = varuint();
    if (value > limits_.max_entries) {
      throw Error("embedding exceeds entry limit");
    }
    return value;
  }

  void finish() const {
    if (cursor_ != bytes_.size()) {
      throw Error("trailing bytes after embedding");
    }
  }

  [[nodiscard]] std::size_t remaining() const noexcept {
    return bytes_.size() - cursor_;
  }

 private:
  std::span<const std::uint8_t> bytes_;
  EmbeddingCodecLimits limits_;
  std::size_t cursor_{0};
};

constexpr std::array<std::uint32_t, 64> sha256_round{
    0x428a2f98U,0x71374491U,0xb5c0fbcfU,0xe9b5dba5U,0x3956c25bU,0x59f111f1U,0x923f82a4U,0xab1c5ed5U,
    0xd807aa98U,0x12835b01U,0x243185beU,0x550c7dc3U,0x72be5d74U,0x80deb1feU,0x9bdc06a7U,0xc19bf174U,
    0xe49b69c1U,0xefbe4786U,0x0fc19dc6U,0x240ca1ccU,0x2de92c6fU,0x4a7484aaU,0x5cb0a9dcU,0x76f988daU,
    0x983e5152U,0xa831c66dU,0xb00327c8U,0xbf597fc7U,0xc6e00bf3U,0xd5a79147U,0x06ca6351U,0x14292967U,
    0x27b70a85U,0x2e1b2138U,0x4d2c6dfcU,0x53380d13U,0x650a7354U,0x766a0abbU,0x81c2c92eU,0x92722c85U,
    0xa2bfe8a1U,0xa81a664bU,0xc24b8b70U,0xc76c51a3U,0xd192e819U,0xd6990624U,0xf40e3585U,0x106aa070U,
    0x19a4c116U,0x1e376c08U,0x2748774cU,0x34b0bcb5U,0x391c0cb3U,0x4ed8aa4aU,0x5b9cca4fU,0x682e6ff3U,
    0x748f82eeU,0x78a5636fU,0x84c87814U,0x8cc70208U,0x90befffaU,0xa4506cebU,0xbef9a3f7U,0xc67178f2U};

std::string sha256(std::span<const std::uint8_t> input) {
  std::vector<std::uint8_t> padded(input.begin(), input.end());
  const auto bit_size = static_cast<std::uint64_t>(input.size()) * 8U;
  padded.push_back(0x80U);
  while (padded.size() % 64U != 56U) padded.push_back(0U);
  for (int shift = 56; shift >= 0; shift -= 8) {
    padded.push_back(static_cast<std::uint8_t>(bit_size >> static_cast<unsigned>(shift)));
  }
  std::array<std::uint32_t, 8> state{0x6a09e667U,0xbb67ae85U,0x3c6ef372U,0xa54ff53aU,
                                      0x510e527fU,0x9b05688cU,0x1f83d9abU,0x5be0cd19U};
  for (std::size_t offset = 0; offset < padded.size(); offset += 64U) {
    std::array<std::uint32_t, 64> words{};
    for (std::size_t index = 0; index < 16U; ++index) {
      for (std::size_t byte = 0; byte < 4U; ++byte) {
        words[index] = (words[index] << 8U) | padded[offset + index * 4U + byte];
      }
    }
    for (std::size_t index = 16; index < words.size(); ++index) {
      const auto s0 = std::rotr(words[index - 15U], 7) ^
                      std::rotr(words[index - 15U], 18) ^ (words[index - 15U] >> 3U);
      const auto s1 = std::rotr(words[index - 2U], 17) ^
                      std::rotr(words[index - 2U], 19) ^ (words[index - 2U] >> 10U);
      words[index] = words[index - 16U] + s0 + words[index - 7U] + s1;
    }
    auto a=state[0],b=state[1],c=state[2],d=state[3];
    auto e=state[4],f=state[5],g=state[6],h=state[7];
    for (std::size_t index = 0; index < words.size(); ++index) {
      const auto s1 = std::rotr(e, 6) ^ std::rotr(e, 11) ^ std::rotr(e, 25);
      const auto choose = (e & f) ^ (~e & g);
      const auto temp1 = h + s1 + choose + sha256_round[index] + words[index];
      const auto s0 = std::rotr(a, 2) ^ std::rotr(a, 13) ^ std::rotr(a, 22);
      const auto majority = (a & b) ^ (a & c) ^ (b & c);
      const auto temp2 = s0 + majority;
      h=g;g=f;f=e;e=d+temp1;d=c;c=b;b=a;a=temp1+temp2;
    }
    state[0]+=a;state[1]+=b;state[2]+=c;state[3]+=d;
    state[4]+=e;state[5]+=f;state[6]+=g;state[7]+=h;
  }
  std::ostringstream output;
  output << std::hex << std::setfill('0');
  for (const auto word : state) output << std::setw(8) << word;
  return output.str();
}

}  // namespace

std::vector<std::uint8_t> encode_embedding(
    const Embedding& embedding, EmbeddingCodecLimits limits) {
  if (embedding.control_slots.size() > limits.max_entries ||
      embedding.values.size() > limits.max_entries) {
    throw Error("embedding exceeds entry limit");
  }
  std::vector<std::uint8_t> output(embedding_magic.begin(),
                                   embedding_magic.end());
  append_varuint(output, embedding.control_slots.size());
  for (const auto& [context, active] : embedding.control_slots) {
    append_string(output, context, limits);
    append_string(output, active, limits);
  }
  append_varuint(output, embedding.values.size());
  for (const auto& [name, value] : embedding.values) {
    append_string(output, name, limits);
    const std::vector<std::uint8_t> encoded = encode_value(value, limits.value_limits);
    append_varuint(output, encoded.size());
    output.insert(output.end(), encoded.begin(), encoded.end());
  }
  if (output.size() > limits.max_bytes) {
    throw Error("embedding exceeds byte limit");
  }
  return output;
}

Embedding decode_embedding(std::span<const std::uint8_t> bytes,
                           EmbeddingCodecLimits limits) {
  Decoder decoder(bytes, limits);
  for (const std::uint8_t expected : embedding_magic) {
    if (decoder.byte() != expected) {
      throw Error("invalid embedding magic or version");
    }
  }
  Embedding embedding;
  std::string previous;
  const std::size_t active_count = decoder.count();
  for (std::size_t index = 0; index < active_count; ++index) {
    std::string context = decoder.string();
    if (index != 0U && context <= previous) {
      throw Error("embedding contexts are not strictly sorted");
    }
    previous = context;
    embedding.control_slots.emplace(std::move(context), decoder.string());
  }
  previous.clear();
  const std::size_t value_count = decoder.count();
  for (std::size_t index = 0; index < value_count; ++index) {
    std::string name = decoder.string();
    if (index != 0U && name <= previous) {
      throw Error("embedding values are not strictly sorted");
    }
    previous = name;
    embedding.values.emplace(
        std::move(name), decode_value(decoder.blob(), limits.value_limits));
  }
  decoder.finish();
  return embedding;
}

std::string embedding_digest(const Embedding& embedding,
                             EmbeddingCodecLimits limits) {
  return sha256(encode_embedding(embedding, limits));
}

std::size_t RawKeyMap::add(RawSlotKind kind, std::string semantic_path) {
  const auto key = std::pair{kind, semantic_path};
  if (const auto found = offsets_.find(key); found != offsets_.end()) {
    return found->second;
  }
  const std::size_t offset = slots_.size();
  offsets_.emplace(std::move(key), offset);
  slots_.push_back(RawKeySlot{kind, std::move(semantic_path), offset});
  return offset;
}

std::optional<std::size_t> RawKeyMap::offset_of(
    RawSlotKind kind, std::string_view semantic_path) const {
  const auto found = offsets_.find(std::pair{kind, std::string(semantic_path)});
  return found == offsets_.end() ? std::nullopt
                                 : std::optional<std::size_t>(found->second);
}

EmbeddingStore::InsertResult EmbeddingStore::insert(
    Embedding embedding, std::optional<std::size_t> witness_parent,
    std::string transition) {
  if (witness_parent && *witness_parent >= nodes_.size()) {
    throw Error("embedding witness parent is out of range");
  }
  std::vector<std::uint8_t> raw_key = encode_embedding(embedding);
  if (const auto existing = raw_content_index_.find(raw_key);
      existing != raw_content_index_.end()) {
    return {existing->second, false};
  }
  const std::string digest = sha256(raw_key);
  const std::size_t index = nodes_.size();
  const std::size_t depth =
      witness_parent ? nodes_[*witness_parent].depth + 1U : 0U;
  nodes_.push_back(EmbeddingRecord{digest, raw_key, std::move(embedding),
                                   witness_parent, std::move(transition), depth});
  raw_content_index_.emplace(std::move(raw_key), index);
  return {index, true};
}

const EmbeddingRecord& EmbeddingStore::at(std::size_t index) const {
  return nodes_.at(index);
}

std::optional<std::size_t> EmbeddingStore::find(const Embedding& embedding) const {
  const auto found = raw_content_index_.find(encode_embedding(embedding));
  return found == raw_content_index_.end()
             ? std::nullopt
             : std::optional<std::size_t>(found->second);
}

std::vector<std::size_t> EmbeddingStore::path_to(
    std::size_t index) const {
  std::vector<std::size_t> path;
  for (;;) {
    path.push_back(index);
    const auto witness_parent = nodes_.at(index).witness_parent;
    if (!witness_parent) break;
    index = *witness_parent;
  }
  std::reverse(path.begin(), path.end());
  return path;
}

}  // namespace dtessl
