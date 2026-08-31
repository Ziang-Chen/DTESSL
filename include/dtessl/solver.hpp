#pragma once

#include "dtessl/dtessl.hpp"
#include "dtessl/value_codec.hpp"

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace dtessl {

// One semantic embedding of a source-level StateSchema.  The implementation
// may store control and value slots densely, but that raw layout is not the
// semantic object exposed to exploration, monitoring, or export.
struct Embedding {
  // One selected control value per schema choice slot. Descendant selections
  // remain as typed latent/history slots when an ancestor alternative is not
  // active; structural matching verifies the ancestor chain explicitly.
  std::map<std::string, std::string, std::less<>> control_slots;
  std::map<std::string, Value, std::less<>> values;

  friend bool operator==(const Embedding&, const Embedding&) = default;
};

struct EmbeddingCodecLimits {
  std::size_t max_bytes{32U * 1024U * 1024U};
  std::size_t max_entries{1024U * 1024U};
  std::size_t max_name_bytes{1024U * 1024U};
  ValueCodecLimits value_limits{};
};

// DTESSL Embedding v1. Entries are strictly sorted, values reuse
// Canonical Value Format v1, and decoding rejects alternate encodings.
[[nodiscard]] std::vector<std::uint8_t> encode_embedding(
    const Embedding& embedding, EmbeddingCodecLimits limits = {});
[[nodiscard]] Embedding decode_embedding(
    std::span<const std::uint8_t> bytes,
    EmbeddingCodecLimits limits = {});
[[nodiscard]] std::string embedding_digest(
    const Embedding& embedding, EmbeddingCodecLimits limits = {});

enum class RawSlotKind { Control, Value };

struct RawKeySlot {
  RawSlotKind kind{RawSlotKind::Control};
  std::string semantic_path;
  std::size_t offset{0};

  friend bool operator==(const RawKeySlot&, const RawKeySlot&) = default;
};

// Lowering layout from a semantic StateSchema/Embedding path to an offset in
// the implementation's raw embedding vector.  It describes representation;
// it is not an Embedding identity, digest, or search node key.
class RawKeyMap {
 public:
  [[nodiscard]] std::size_t add(RawSlotKind kind, std::string semantic_path);
  [[nodiscard]] std::optional<std::size_t> offset_of(
      RawSlotKind kind, std::string_view semantic_path) const;
  [[nodiscard]] const std::vector<RawKeySlot>& slots() const noexcept {
    return slots_;
  }
  [[nodiscard]] std::size_t size() const noexcept { return slots_.size(); }

 private:
  std::vector<RawKeySlot> slots_;
  std::map<std::pair<RawSlotKind, std::string>, std::size_t> offsets_;
};

struct EmbeddingRecord {
  std::string digest;
  std::vector<std::uint8_t> raw_key;
  Embedding embedding;
  // This is one discovery/witness predecessor, not the topology of the
  // reachable graph. Back-edges and joins are stored by the Solver separately.
  std::optional<std::size_t> witness_parent;
  std::string transition;
  std::size_t depth{0};
};

// Exact content-addressed deduplication scoped to one Solver search.
class EmbeddingStore {
 public:
  struct InsertResult {
    std::size_t index{0};
    bool inserted{false};
  };

  [[nodiscard]] InsertResult insert(Embedding embedding,
                                    std::optional<std::size_t> witness_parent = {},
                                    std::string transition = {});
  [[nodiscard]] const EmbeddingRecord& at(std::size_t index) const;
  [[nodiscard]] std::optional<std::size_t> find(const Embedding& embedding) const;
  [[nodiscard]] std::vector<std::size_t> path_to(std::size_t index) const;
  [[nodiscard]] std::size_t size() const noexcept { return nodes_.size(); }

 private:
  std::vector<EmbeddingRecord> nodes_;
  // Exact canonical raw bytes -> node offset. This is deliberately not named
  // RawKeyMap: RawKeyMap is the schema-to-vector layout above.
  std::map<std::vector<std::uint8_t>, std::size_t> raw_content_index_;
};

struct SolverLimits {
  std::size_t max_embeddings{100000};
  std::size_t max_depth{1000};
};

enum class ClaimSolveStatus {
  Counterexample,
  Verified,
  BoundedVerified,
  Inconclusive,
};

struct CounterexampleFrame {
  std::size_t depth{0};
  std::string embedding_digest;
  std::string transition;
  Embedding embedding;
};

struct ClaimSolveResult {
  std::string claim;
  ClaimSolveStatus status{ClaimSolveStatus::Inconclusive};
  std::size_t explored_embeddings{0};
  std::size_t explored_edges{0};
  // Product nodes are (Embedding, ClaimMonitorState).  This can exceed
  // explored_embeddings for history-sensitive `since`, bounded `within`, and
  // counting monitors without duplicating the base EmbeddingExpand node.
  std::size_t explored_product_states{0};
  std::size_t claim_monitor_states{0};
  std::size_t max_depth_reached{0};
  std::vector<CounterexampleFrame> counterexample;
  std::string detail;
};

// High-performance semantic layer between the typed frontend and execution or
// exploration backends. It searches enabled transitions over Embeddings;
// it does not mutate source-level state declarations or perform ActionPlans.
class Solver {
 public:
  explicit Solver(Program program,
                  SolverEncoding encoding = SolverEncoding::DenseIds);

  [[nodiscard]] Embedding initial_embedding() const;
  [[nodiscard]] const RawKeyMap& raw_key_map() const;
  [[nodiscard]] ClaimSolveResult verify_claim(
      std::string_view claim, SolverLimits limits = {}) const;

 private:
  Program program_;
  SolverEncoding encoding_{SolverEncoding::DenseIds};
};

[[nodiscard]] std::string_view claim_solve_status_name(
    ClaimSolveStatus status) noexcept;

}  // namespace dtessl
