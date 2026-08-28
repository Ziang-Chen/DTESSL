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

// One dynamic point in the transition system. A source-level `state`
// declaration is a static control location; only the active locations plus
// their typed variable valuation form a Configuration.
struct Configuration {
  std::map<std::string, std::string, std::less<>> active_locations;
  std::map<std::string, Value, std::less<>> values;

  friend bool operator==(const Configuration&, const Configuration&) = default;
};

struct ConfigurationCodecLimits {
  std::size_t max_bytes{32U * 1024U * 1024U};
  std::size_t max_entries{1024U * 1024U};
  std::size_t max_name_bytes{1024U * 1024U};
  ValueCodecLimits value_limits{};
};

// DTESSL Configuration v1. Entries are strictly sorted, values reuse
// Canonical Value Format v1, and decoding rejects alternate encodings.
[[nodiscard]] std::vector<std::uint8_t> encode_configuration(
    const Configuration& configuration, ConfigurationCodecLimits limits = {});
[[nodiscard]] Configuration decode_configuration(
    std::span<const std::uint8_t> bytes,
    ConfigurationCodecLimits limits = {});
[[nodiscard]] std::string configuration_digest(
    const Configuration& configuration, ConfigurationCodecLimits limits = {});

struct ConfigurationRecord {
  std::string digest;
  Configuration configuration;
  // This is one discovery/witness predecessor, not the topology of the
  // reachable graph. Back-edges and joins are stored by the Solver separately.
  std::optional<std::size_t> witness_parent;
  std::string transition;
  std::size_t depth{0};
};

// Exact content-addressed deduplication scoped to one Solver search. The digest
// is an accelerator; full Configuration equality resolves hash collisions.
class ConfigurationStore {
 public:
  struct InsertResult {
    std::size_t index{0};
    bool inserted{false};
  };

  [[nodiscard]] InsertResult insert(Configuration configuration,
                                    std::optional<std::size_t> witness_parent = {},
                                    std::string transition = {});
  [[nodiscard]] const ConfigurationRecord& at(std::size_t index) const;
  [[nodiscard]] std::optional<std::size_t> find(std::string_view digest) const;
  [[nodiscard]] std::vector<std::size_t> path_to(std::size_t index) const;
  [[nodiscard]] std::size_t size() const noexcept { return nodes_.size(); }

 private:
  std::vector<ConfigurationRecord> nodes_;
  std::map<std::string, std::size_t, std::less<>> index_;
};

struct SolverLimits {
  std::size_t max_configurations{100000};
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
  std::string configuration_digest;
  std::string transition;
  Configuration configuration;
};

struct ClaimSolveResult {
  std::string claim;
  ClaimSolveStatus status{ClaimSolveStatus::Inconclusive};
  std::size_t explored_configurations{0};
  std::size_t explored_edges{0};
  // Product nodes are (Configuration, ClaimMonitorState).  This can exceed
  // explored_configurations for history-sensitive `since`, bounded `within`,
  // and counting monitors without duplicating the base StateExpand node.
  std::size_t explored_product_states{0};
  std::size_t claim_monitor_states{0};
  std::size_t max_depth_reached{0};
  std::vector<CounterexampleFrame> counterexample;
  std::string detail;
};

// High-performance semantic layer between the typed frontend and execution or
// exploration backends. It searches enabled transitions over Configurations;
// it does not mutate source-level state declarations or perform ActionPlans.
class Solver {
 public:
  explicit Solver(Program program,
                  SolverEncoding encoding = SolverEncoding::DenseIds);

  [[nodiscard]] Configuration initial_configuration() const;
  [[nodiscard]] ClaimSolveResult verify_claim(
      std::string_view claim, SolverLimits limits = {}) const;

 private:
  Program program_;
  SolverEncoding encoding_{SolverEncoding::DenseIds};
};

[[nodiscard]] std::string_view claim_solve_status_name(
    ClaimSolveStatus status) noexcept;

}  // namespace dtessl
