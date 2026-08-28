#pragma once

#include "dtessl/dtessl.hpp"

#include <cstdint>
#include <set>
#include <string>
#include <string_view>

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

// This interface intentionally stops at typed discovery/verification. Concrete
// output APIs are added with the versioned CanonicalModule; the private parser
// tree is never passed to a backend.
class BackendProvider {
 public:
  virtual ~BackendProvider() = default;
  [[nodiscard]] virtual BackendDescriptor descriptor() const = 0;
};

[[nodiscard]] FeatureSet required_features(const Program& program);
[[nodiscard]] BackendCompatibility negotiate_backend(
    const Program& program, const BackendDescriptor& backend, Projection projection);
[[nodiscard]] std::string_view feature_name(LanguageFeature feature) noexcept;
[[nodiscard]] std::string_view projection_name(Projection projection) noexcept;

}  // namespace dtessl
