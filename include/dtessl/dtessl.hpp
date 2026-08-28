#pragma once

#include "dtessl/exact_numeric.hpp"

#include <cstdint>
#include <map>
#include <memory>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace dtessl {

inline constexpr std::size_t relation_arity_limit = 64;
inline constexpr std::size_t relation_row_limit = 4096;
inline constexpr std::size_t relation_work_limit = 1'000'000;
inline constexpr std::size_t event_trace_round_limit = 100'000;
inline constexpr std::size_t event_batch_size_limit = 4096;

class Value;
struct ValueList;
struct ValueSet;
struct ValueMap;
struct ValueBag;
struct ValueRecord;
struct ValueVariant;
struct ValueNewtype;
struct ValueName;
struct ValueTuple;
struct ValueRelation;

struct StringSet {
  std::set<std::string, std::less<>> values;

  friend bool operator==(const StringSet&, const StringSet&) = default;
};

class Value {
 public:
  enum class Kind {
    Bool,
    Int,
    String,
    Rational,
    StringSet,
    List,
    Set,
    Map,
    Bag,
    Record,
    Variant,
    Newtype,
    Tuple,
    Relation,
    Name,
  };

  Value(bool value);
  Value(std::int64_t value);
  Value(ExactInt value);
  Value(Rational value);
  Value(std::string value);
  Value(const char* value);
  Value(StringSet value);
  Value(ValueList value);
  Value(ValueSet value);
  Value(ValueMap value);
  Value(ValueBag value);
  Value(ValueRecord value);
  Value(ValueVariant value);
  Value(ValueNewtype value);
  Value(ValueName value);
  Value(ValueTuple value);
  Value(ValueRelation value);

  [[nodiscard]] Kind kind() const noexcept;
  [[nodiscard]] bool as_bool() const;
  [[nodiscard]] std::int64_t as_int() const;
  [[nodiscard]] const ExactInt& as_exact_int() const;
  [[nodiscard]] const Rational& as_rational() const;
  [[nodiscard]] const std::string& as_string() const;
  [[nodiscard]] const StringSet& as_string_set() const;
  [[nodiscard]] const ValueList& as_list() const;
  [[nodiscard]] const ValueSet& as_set() const;
  [[nodiscard]] const ValueMap& as_map() const;
  [[nodiscard]] const ValueBag& as_bag() const;
  [[nodiscard]] const ValueRecord& as_record() const;
  [[nodiscard]] const ValueVariant& as_variant() const;
  [[nodiscard]] const ValueNewtype& as_newtype() const;
  [[nodiscard]] const ValueName& as_name() const;
  [[nodiscard]] const ValueTuple& as_tuple() const;
  [[nodiscard]] const ValueRelation& as_relation() const;

  friend bool operator==(const Value& left, const Value& right);

 private:
  Kind kind_;
  bool bool_value_{false};
  ExactInt int_value_;
  Rational rational_value_;
  std::string string_value_;
  StringSet set_value_;
  std::shared_ptr<const ValueList> list_value_;
  std::shared_ptr<const ValueSet> generic_set_value_;
  std::shared_ptr<const ValueMap> map_value_;
  std::shared_ptr<const ValueBag> bag_value_;
  std::shared_ptr<const ValueRecord> record_value_;
  std::shared_ptr<const ValueVariant> variant_value_;
  std::shared_ptr<const ValueNewtype> newtype_value_;
  std::shared_ptr<const ValueName> name_value_;
  std::shared_ptr<const ValueTuple> tuple_value_;
  std::shared_ptr<const ValueRelation> relation_value_;
};

// Total canonical order used by generic sets, maps, bags and codecs. It is a
// representation order, not a user-visible numeric or semantic comparison.
[[nodiscard]] int canonical_compare(const Value& left, const Value& right);

struct ValueList {
  std::vector<Value> values;
  friend bool operator==(const ValueList&, const ValueList&) = default;
};

struct ValueSet {
  std::vector<Value> values;
  friend bool operator==(const ValueSet&, const ValueSet&) = default;
};

struct ValueMap {
  std::vector<std::pair<Value, Value>> entries;
  friend bool operator==(const ValueMap&, const ValueMap&) = default;
};

struct ValueBag {
  std::vector<std::pair<Value, std::uint64_t>> entries;
  friend bool operator==(const ValueBag&, const ValueBag&) = default;
};

// Nominal values carry their canonical type identity. For v0.1.x this is the
// declaration name in one source unit; package-qualified identities replace it
// when CanonicalModule lands without changing the value shape.
struct ValueRecord {
  std::string type_id;
  std::vector<std::pair<std::string, Value>> fields;
  friend bool operator==(const ValueRecord&, const ValueRecord&) = default;
};

struct ValueVariant {
  std::string type_id;
  std::string constructor;
  std::vector<Value> payload;
  friend bool operator==(const ValueVariant&, const ValueVariant&) = default;
};

struct ValueNewtype {
  std::string type_id;
  std::vector<Value> payload;
  friend bool operator==(const ValueNewtype&, const ValueNewtype&) = default;
};

// A logical name is a nominal, canonical atom. It is neither human text nor
// authority: WorkerId(a) and TaskId(a) are distinct values.
struct ValueName {
  std::string type_id;
  std::string atom;
  friend bool operator==(const ValueName&, const ValueName&) = default;
};

struct ValueTuple {
  std::vector<Value> fields;
  friend bool operator==(const ValueTuple&, const ValueTuple&) = default;
};

struct ValueRelation {
  std::size_t arity{0};
  std::vector<ValueTuple> rows;
  friend bool operator==(const ValueRelation&, const ValueRelation&) = default;
};

struct Event {
  std::string name;
  std::map<std::string, Value, std::less<>> fields;
};

struct ActionCall {
  std::string label;
  std::string function;
  std::vector<Value> arguments;
  std::string context;

  friend bool operator==(const ActionCall&, const ActionCall&) = default;
};

struct ActionPlan {
  std::vector<ActionCall> calls;
  std::vector<std::pair<std::size_t, std::size_t>> dependencies;

  friend bool operator==(const ActionPlan&, const ActionPlan&) = default;
};

struct StepResult {
  // A round is one atomic simulation batch, not a per-transition clock.
  // Independent transitions in the same batch share this value.
  std::uint64_t round{0};
  // Stable within the canonical event bag. The numeric suffix is an identity,
  // not a happens-before relation between same-round decisions.
  std::string id;
  // Qualified as Transition.caseName when the selected case is named.
  std::string transition;
  std::string case_name;
  std::string from_state;
  std::string to_state;
  // Active state per explicit @ context after this decision. The empty
  // context is the legacy single-state root.
  std::map<std::string, std::string, std::less<>> active_states;
  std::map<std::string, Value, std::less<>> state;
  ActionPlan actions;
  std::set<std::string, std::less<>> reads;
  std::set<std::string, std::less<>> writes;
  std::set<std::string, std::less<>> causal_predecessors;

  friend bool operator==(const StepResult&, const StepResult&) = default;
};

struct ParallelStepResult {
  std::uint64_t round{0};
  std::vector<StepResult> transitions;
  std::map<std::string, Value, std::less<>> state;

  friend bool operator==(const ParallelStepResult&, const ParallelStepResult&) = default;
};

struct EventBatch {
  std::vector<Event> events;
  friend bool operator==(const EventBatch&, const EventBatch&) = default;
};

struct EventTrace {
  std::vector<EventBatch> rounds;
  friend bool operator==(const EventTrace&, const EventTrace&) = default;
};

struct TraceResult {
  std::vector<ParallelStepResult> rounds;
  std::string final_state_name;
  std::map<std::string, Value, std::less<>> final_state;
  friend bool operator==(const TraceResult&, const TraceResult&) = default;
};

enum class TraceCaptureMode {
  Static,
  Closed,
  Projected,
};

struct TraceSnapshot {
  std::string name;
  std::string root_context;
  TraceCaptureMode mode{TraceCaptureMode::Static};
  bool closed{false};
  std::set<std::string, std::less<>> captured_contexts;
  std::set<std::string, std::less<>> captured_paths;
  std::vector<ParallelStepResult> rounds;
  std::map<std::string, Value, std::less<>> final_state;
  std::vector<std::string> causal_gaps;

  friend bool operator==(const TraceSnapshot&, const TraceSnapshot&) = default;
};

enum class ClaimStatus {
  Satisfied,
  Violated,
  Pending,
};

struct ClaimEvaluation {
  std::string name;
  std::string trace;
  ClaimStatus status{ClaimStatus::Pending};
  std::uint64_t witness_round{0};
  std::string detail;

  friend bool operator==(const ClaimEvaluation&, const ClaimEvaluation&) = default;
};

class Error : public std::runtime_error {
 public:
  Error(std::string message, std::size_t line = 0, std::size_t column = 0);

  [[nodiscard]] std::size_t line() const noexcept { return line_; }
  [[nodiscard]] std::size_t column() const noexcept { return column_; }

 private:
  std::size_t line_;
  std::size_t column_;
};

class Program {
 public:
  struct Impl;

  Program();
  explicit Program(std::shared_ptr<const Impl> impl);

  [[nodiscard]] bool empty() const noexcept;
  [[nodiscard]] const std::shared_ptr<const Impl>& implementation() const noexcept;

 private:
  std::shared_ptr<const Impl> impl_;
};

// Parses and verifies one complete DTESSL source unit.
[[nodiscard]] Program parse(std::string_view source);

class Engine {
 public:
  explicit Engine(Program program);
  Engine(Program program,
         std::map<std::string, std::string, std::less<>> initial_states);

  // A procedure is only a named Engine entry configuration. It supplies the
  // initial orthogonal states and lexical entry context; all later progress is
  // still selected by the program's global transition engine.
  [[nodiscard]] static Engine from_procedure(Program program,
                                             std::string_view procedure_name);

  // Executes one logical event. External calls are only described in the
  // returned ActionPlan; this standalone engine never performs ambient I/O.
  [[nodiscard]] StepResult step(const Event& event);

  // Evaluates every event against one immutable before snapshot and commits
  // non-conflicting field updates atomically in one discrete round. Concurrent
  // writes to the same field are rejected until an explicit merge relation is
  // available in the language.
  [[nodiscard]] ParallelStepResult step_parallel(const std::vector<Event>& events);

  [[nodiscard]] std::string current_state() const;
  [[nodiscard]] const std::map<std::string, std::string, std::less<>>&
  current_states() const noexcept;
  [[nodiscard]] const std::string& initial_context() const noexcept;
  [[nodiscard]] std::uint64_t current_round() const noexcept;
  [[nodiscard]] const std::map<std::string, Value, std::less<>>& values() const;
  [[nodiscard]] TraceSnapshot captured_trace(std::string_view name,
                                             bool close = false) const;
  [[nodiscard]] std::vector<ClaimEvaluation> evaluate_claims(
      std::string_view trace_name, bool close = false) const;

 private:
  Program program_;
  std::string initial_context_;
  std::uint64_t round_{0};
  std::map<std::string, std::string, std::less<>> active_states_;
  std::map<std::string, Value, std::less<>> values_;
  std::map<std::string, std::set<std::string, std::less<>>, std::less<>> last_writers_;
  std::map<std::string, TraceSnapshot, std::less<>> captured_traces_;
};

[[nodiscard]] std::string value_text(const Value& value);
[[nodiscard]] std::string result_text(const StepResult& result);
// Native DTESSL replay only: Program + typed EventTrace. It never consumes a
// runtime journal or provider receipt and never executes an ActionPlan.
[[nodiscard]] TraceResult run_trace(const Program& program, const EventTrace& trace);
[[nodiscard]] TraceResult replay_trace(const Program& program, const EventTrace& trace);
[[nodiscard]] std::vector<std::string> declared_traces(const Program& program);
[[nodiscard]] std::vector<std::string> declared_procedures(const Program& program);
[[nodiscard]] TraceSnapshot run_named_trace(const Program& program,
                                            std::string_view name);
[[nodiscard]] std::vector<ClaimEvaluation> evaluate_named_trace(
    const Program& program, std::string_view name);
[[nodiscard]] std::string_view claim_status_name(ClaimStatus status) noexcept;

}  // namespace dtessl
