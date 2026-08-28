#pragma once

#include "dtessl/dtessl.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace dtessl {

inline constexpr std::uint32_t semantic_descriptor_format_v1 = 1;
inline constexpr std::size_t semantic_descriptor_size_limit = 4U * 1024U * 1024U;
inline constexpr std::size_t semantic_descriptor_item_limit = 4096;
inline constexpr std::size_t semantic_descriptor_fragment_limit = 64U * 1024U;

enum class SemanticProvenance {
  GeneratedOperationalMirror,
  IndependentAssuranceModel,
};

enum class SemanticGapKind {
  Unmodeled,
  Abstracted,
  Unsupported,
  ExternalRuntime,
};

struct SemanticOrigin {
  std::string kind;
  std::string locator;
  std::string digest;
  friend bool operator==(const SemanticOrigin&, const SemanticOrigin&) = default;
};

struct SemanticPort {
  std::string name;
  std::vector<std::string> parameter_types;
  friend bool operator==(const SemanticPort&, const SemanticPort&) = default;
};

struct SemanticField {
  std::string name;
  std::string type;
  std::string initial;
  std::string semantic_path;
  bool lifecycle{false};
  friend bool operator==(const SemanticField&, const SemanticField&) = default;
};

struct SemanticInvariant {
  std::string id;
  std::string expression;
  friend bool operator==(const SemanticInvariant&, const SemanticInvariant&) = default;
};

struct SemanticState {
  std::string name;
  std::string context;
  bool initial{false};
  std::vector<SemanticField> fields;
  std::vector<SemanticInvariant> invariants;
  friend bool operator==(const SemanticState&, const SemanticState&) = default;
};

struct SemanticParameter {
  std::string name;
  std::string type;
  friend bool operator==(const SemanticParameter&, const SemanticParameter&) = default;
};

struct SemanticEvent {
  std::string name;
  std::vector<SemanticParameter> fields;
  friend bool operator==(const SemanticEvent&, const SemanticEvent&) = default;
};

struct SemanticRequirement {
  std::string id;
  std::string predicate;
  friend bool operator==(const SemanticRequirement&, const SemanticRequirement&) = default;
};

struct SemanticSelection {
  std::string target_field;
  std::string binding;
  std::string domain;
  std::string predicate;
  std::vector<std::string> lex_score;
  friend bool operator==(const SemanticSelection&, const SemanticSelection&) = default;
};

struct SemanticAssignment {
  std::string target_field;
  std::string expression;
  friend bool operator==(const SemanticAssignment&, const SemanticAssignment&) = default;
};

struct SemanticAction {
  std::uint32_t stage{0};
  std::string label;
  std::string port;
  std::string context;
  std::vector<std::string> arguments;
  friend bool operator==(const SemanticAction&, const SemanticAction&) = default;
};

struct SemanticTransition {
  std::string name;
  std::string event;
  std::string from;
  std::string to;
  std::vector<SemanticRequirement> requirements;
  std::vector<SemanticSelection> selections;
  std::vector<SemanticAssignment> assignments;
  std::vector<SemanticAction> actions;
  friend bool operator==(const SemanticTransition&, const SemanticTransition&) = default;
};

struct LifecycleValue {
  std::string phase;
  std::string literal;
  friend bool operator==(const LifecycleValue&, const LifecycleValue&) = default;
};

struct LifecycleMapping {
  std::string id;
  std::string state;
  std::string field;
  std::vector<LifecycleValue> values;
  friend bool operator==(const LifecycleMapping&, const LifecycleMapping&) = default;
};

struct SemanticGap {
  std::string id;
  SemanticGapKind kind{SemanticGapKind::Unmodeled};
  std::string subject;
  std::string detail;
  friend bool operator==(const SemanticGap&, const SemanticGap&) = default;
};

struct SemanticDescriptor {
  std::uint32_t format{semantic_descriptor_format_v1};
  std::string name;
  SemanticProvenance provenance{SemanticProvenance::GeneratedOperationalMirror};
  SemanticOrigin origin;
  std::vector<SemanticPort> ports;
  std::vector<SemanticState> states;
  std::vector<SemanticEvent> events;
  std::vector<SemanticTransition> transitions;
  std::vector<LifecycleMapping> lifecycle_mappings;
  std::vector<SemanticGap> gaps;
  friend bool operator==(const SemanticDescriptor&, const SemanticDescriptor&) = default;
};

struct SourceMapEntry {
  std::size_t generated_line{0};
  std::string descriptor_path;
  friend bool operator==(const SourceMapEntry&, const SourceMapEntry&) = default;
};

struct GeneratedSemanticSource {
  std::string source;
  std::vector<SourceMapEntry> source_map;
};

struct SemanticCoverageManifest {
  std::string descriptor_digest;
  std::string generated_source_digest;
  SemanticProvenance provenance{SemanticProvenance::GeneratedOperationalMirror};
  std::size_t states{0};
  std::size_t fields{0};
  std::size_t events{0};
  std::size_t relation_fields{0};
  std::size_t transitions{0};
  std::size_t requirements{0};
  std::size_t deterministic_selections{0};
  std::size_t invariants{0};
  std::size_t action_ports{0};
  std::size_t mapped_fields{0};
  std::size_t lifecycle_mappings{0};
  std::size_t lifecycle_fields{0};
  std::size_t used_action_ports{0};
  std::size_t unmodeled_gaps{0};
  std::size_t abstracted_gaps{0};
  std::size_t unsupported_gaps{0};
  std::size_t external_runtime_gaps{0};
  bool structural_coverage_complete{false};
  bool gap_free{false};
  bool complete{false};
  // A provenance assertion supplied by the descriptor author, never proof of
  // independence by itself.
  bool independent_assurance_claim{false};
};

struct SemanticCheckResult {
  GeneratedSemanticSource generated;
  SemanticCoverageManifest coverage;
};

[[nodiscard]] std::string_view semantic_provenance_name(
    SemanticProvenance provenance) noexcept;
[[nodiscard]] std::string_view semantic_gap_kind_name(SemanticGapKind kind) noexcept;
[[nodiscard]] std::string print_semantic_descriptor(const SemanticDescriptor& descriptor);
[[nodiscard]] SemanticDescriptor parse_semantic_descriptor(std::string_view text);
[[nodiscard]] std::string semantic_descriptor_digest(const SemanticDescriptor& descriptor);
// Consumer-supplied binding check. DTESSL never fetches or infers the origin.
void verify_semantic_origin(const SemanticDescriptor& descriptor,
                            const SemanticOrigin& expected,
                            SemanticProvenance expected_provenance);
[[nodiscard]] GeneratedSemanticSource generate_dtessl(const SemanticDescriptor& descriptor);
[[nodiscard]] SemanticCoverageManifest semantic_coverage(
    const SemanticDescriptor& descriptor, const GeneratedSemanticSource& generated);
[[nodiscard]] std::string print_source_map(const SemanticDescriptor& descriptor,
                                           const GeneratedSemanticSource& generated);
[[nodiscard]] std::string print_semantic_coverage(const SemanticCoverageManifest& coverage);
[[nodiscard]] SemanticCheckResult check_semantic_descriptor(
    const SemanticDescriptor& descriptor);
[[nodiscard]] ParallelStepResult run_semantic_descriptor(
    const SemanticDescriptor& descriptor, const std::vector<Event>& events);
[[nodiscard]] ParallelStepResult replay_semantic_descriptor(
    const SemanticDescriptor& descriptor, const std::vector<Event>& events);
[[nodiscard]] TraceResult run_semantic_descriptor_trace(
    const SemanticDescriptor& descriptor, const EventTrace& trace);
[[nodiscard]] TraceResult replay_semantic_descriptor_trace(
    const SemanticDescriptor& descriptor, const EventTrace& trace);

}  // namespace dtessl
