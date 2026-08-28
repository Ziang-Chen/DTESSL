#pragma once

#include "dtessl/dtessl.hpp"

#include <cstdint>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace dtessl {

enum class Projection {
  Execute,
  Monitor,
  Explore,
  FormalExport,
};

enum class LanguageFeature {
  TypedState,
  FiniteCollections,
  ExistentialSearch,
  PureSetUpdate,
  ActionDag,
  ParallelEventBag,
  EqualMerge,
  UnionMerge,
  AlgebraicDataTypes,
  NominalTypes,
  ExhaustiveMatch,
  ExactNumeric,
  RelationAlgebra,
  UniversalSearch,
  DeterministicSelect,
  TypedActionPorts,
  LogicalNames,
  DirectRelationBinding,
  CompositeStateSet,
  TypedTrace,
  TraceClaims,
  PureFunctions,
  ProcedureEntry,
  OptimizedTransition,
  TemporalLogic,
  ProcedureLambda,
  TransitionObligation,
};

using FeatureSet = std::set<LanguageFeature>;
using ProjectionSet = std::set<Projection>;

struct BackendId {
  std::string domain;
  std::string name;
  std::uint32_t abi{1};

  friend bool operator==(const BackendId&, const BackendId&) = default;
};

struct BackendDescriptor {
  BackendId id;
  ProjectionSet projections;
  FeatureSet features;
};

struct BackendCompatibility {
  bool compatible{false};
  bool projection_supported{false};
  FeatureSet required;
  FeatureSet missing;
};

struct SearchPlanSummary {
  std::string operation;
  std::size_t max_rows{relation_row_limit};
  std::size_t max_work{relation_work_limit};
  bool deterministic{true};
  bool rejects_ambiguous_score{false};

  friend bool operator==(const SearchPlanSummary&, const SearchPlanSummary&) = default;
};

// This interface intentionally stops at typed discovery/verification. Concrete
// output APIs are added with the versioned CanonicalModule; the private parser
// tree is never passed to a backend.
class BackendProvider {
 public:
  virtual ~BackendProvider() = default;
  [[nodiscard]] virtual BackendDescriptor descriptor() const = 0;
};

[[nodiscard]] FeatureSet required_features(const Program& program);
[[nodiscard]] std::vector<SearchPlanSummary> search_plans(const Program& program);
[[nodiscard]] BackendCompatibility negotiate_backend(
    const Program& program, const BackendDescriptor& backend, Projection projection);
[[nodiscard]] std::string_view feature_name(LanguageFeature feature) noexcept;
[[nodiscard]] std::string_view projection_name(Projection projection) noexcept;

}  // namespace dtessl
