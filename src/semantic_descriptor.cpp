#include "dtessl/semantic_descriptor.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <iomanip>
#include <limits>
#include <map>
#include <set>
#include <sstream>
#include <span>
#include <string>
#include <unordered_map>
#include <utility>

namespace dtessl {
namespace {

constexpr std::array<std::uint32_t, 64> sha256_round{
    0x428a2f98U,0x71374491U,0xb5c0fbcfU,0xe9b5dba5U,0x3956c25bU,0x59f111f1U,0x923f82a4U,0xab1c5ed5U,
    0xd807aa98U,0x12835b01U,0x243185beU,0x550c7dc3U,0x72be5d74U,0x80deb1feU,0x9bdc06a7U,0xc19bf174U,
    0xe49b69c1U,0xefbe4786U,0x0fc19dc6U,0x240ca1ccU,0x2de92c6fU,0x4a7484aaU,0x5cb0a9dcU,0x76f988daU,
    0x983e5152U,0xa831c66dU,0xb00327c8U,0xbf597fc7U,0xc6e00bf3U,0xd5a79147U,0x06ca6351U,0x14292967U,
    0x27b70a85U,0x2e1b2138U,0x4d2c6dfcU,0x53380d13U,0x650a7354U,0x766a0abbU,0x81c2c92eU,0x92722c85U,
    0xa2bfe8a1U,0xa81a664bU,0xc24b8b70U,0xc76c51a3U,0xd192e819U,0xd6990624U,0xf40e3585U,0x106aa070U,
    0x19a4c116U,0x1e376c08U,0x2748774cU,0x34b0bcb5U,0x391c0cb3U,0x4ed8aa4aU,0x5b9cca4fU,0x682e6ff3U,
    0x748f82eeU,0x78a5636fU,0x84c87814U,0x8cc70208U,0x90befffaU,0xa4506cebU,0xbef9a3f7U,0xc67178f2U};

std::string sha256(std::string_view input) {
  std::vector<std::uint8_t> padded(input.begin(), input.end());
  const auto bit_size = static_cast<std::uint64_t>(input.size()) * 8U;
  padded.push_back(0x80U);
  while (padded.size() % 64U != 56U) padded.push_back(0U);
  for (int shift = 56; shift >= 0; shift -= 8) {
    padded.push_back(static_cast<std::uint8_t>((bit_size >> shift) & 0xffU));
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

bool identifier(std::string_view value) {
  if (value.empty() || value.size() > 256U ||
      (std::isalpha(static_cast<unsigned char>(value.front())) == 0 &&
                        value.front() != '_')) return false;
  return std::all_of(value.begin() + 1, value.end(), [](char character) {
    return std::isalnum(static_cast<unsigned char>(character)) != 0 || character == '_';
  });
}

bool path(std::string_view value) {
  if (value.empty()) return false;
  std::size_t start = 0;
  for (;;) {
    const std::size_t dot = value.find('.', start);
    const std::string_view part = value.substr(start, dot == std::string_view::npos
                                                         ? dot : dot - start);
    if (!identifier(part)) return false;
    if (dot == std::string_view::npos) return true;
    start = dot + 1U;
  }
}

bool digest(std::string_view value) {
  return value.size() == 64U && std::all_of(value.begin(), value.end(), [](char character) {
    return (character >= '0' && character <= '9') ||
           (character >= 'a' && character <= 'f');
  });
}

void fragment(std::string_view value, std::string_view description) {
  if (value.empty() || value.size() > semantic_descriptor_fragment_limit ||
      value.find('\n') != std::string_view::npos ||
      value.find('\r') != std::string_view::npos) {
    throw Error(std::string(description) + " must be one non-empty source line");
  }
}

template <class T>
void bounded(const std::vector<T>& values, std::string_view description) {
  if (values.size() > semantic_descriptor_item_limit) {
    throw Error(std::string(description) + " exceeds item limit");
  }
}

void type_fragment(std::string_view value) {
  fragment(value, "semantic type");
  static constexpr std::array forbidden{"Handle", "Binding", "Lease"};
  for (const std::string_view word : forbidden) {
    if (value.find(word) != std::string_view::npos) {
      throw Error("SemanticDescriptor v1 cannot mint " + std::string(word));
    }
  }
}

template <class T, class Key>
void sort_by(std::vector<T>& values, Key key) {
  std::sort(values.begin(), values.end(), [&](const T& left, const T& right) {
    return key(left) < key(right);
  });
}

SemanticDescriptor canonicalized(SemanticDescriptor value) {
  sort_by(value.ports, [](const auto& item) { return item.name; });
  sort_by(value.states, [](const auto& item) { return item.name; });
  for (auto& state : value.states) {
    sort_by(state.fields, [](const auto& item) { return item.name; });
    sort_by(state.invariants, [](const auto& item) { return item.id; });
  }
  sort_by(value.events, [](const auto& item) { return item.name; });
  for (auto& event : value.events) {
    sort_by(event.fields, [](const auto& item) { return item.name; });
  }
  sort_by(value.transitions, [](const auto& item) { return item.name; });
  for (auto& transition : value.transitions) {
    sort_by(transition.requirements, [](const auto& item) { return item.id; });
    sort_by(transition.selections, [](const auto& item) { return item.target_field; });
    sort_by(transition.assignments, [](const auto& item) { return item.target_field; });
    std::sort(transition.actions.begin(), transition.actions.end(),
              [](const auto& left, const auto& right) {
                return std::pair{left.stage, left.label} < std::pair{right.stage, right.label};
              });
  }
  sort_by(value.lifecycle_mappings, [](const auto& item) { return item.id; });
  for (auto& mapping : value.lifecycle_mappings) {
    sort_by(mapping.values, [](const auto& item) { return item.phase; });
  }
  sort_by(value.gaps, [](const auto& item) { return item.id; });
  return value;
}

std::string quoted(std::string_view value) {
  std::ostringstream output;
  output << std::quoted(std::string(value));
  return output.str();
}

void finish(std::istringstream& input, std::size_t line) {
  input >> std::ws;
  if (!input.eof()) throw Error("trailing SemanticDescriptor data", line, 1);
}

std::string read_quoted(std::istringstream& input, std::size_t line,
                        std::string_view description) {
  std::string result;
  if (!(input >> std::quoted(result))) {
    throw Error("missing " + std::string(description), line, 1);
  }
  return result;
}

template <class T>
T number(std::istringstream& input, std::size_t line, std::string_view description) {
  std::uint64_t value = 0;
  if (!(input >> value) || value > static_cast<std::uint64_t>(std::numeric_limits<T>::max())) {
    throw Error("invalid " + std::string(description), line, 1);
  }
  return static_cast<T>(value);
}

bool boolean(std::istringstream& input, std::size_t line, std::string_view description) {
  const auto value = number<std::uint32_t>(input, line, description);
  if (value > 1U) throw Error("invalid " + std::string(description), line, 1);
  return value != 0U;
}

std::vector<std::string> strings(std::istringstream& input, std::size_t line,
                                 std::string_view description) {
  const auto count = number<std::uint32_t>(input, line, description);
  if (count > 4096U) throw Error("SemanticDescriptor list exceeds limit", line, 1);
  std::vector<std::string> result;
  result.reserve(count);
  for (std::uint32_t index = 0; index < count; ++index) {
    result.push_back(read_quoted(input, line, description));
  }
  return result;
}

SemanticProvenance parse_provenance(std::string_view value, std::size_t line = 0) {
  if (value == "generated-operational-mirror") {
    return SemanticProvenance::GeneratedOperationalMirror;
  }
  if (value == "independent-assurance-model") {
    return SemanticProvenance::IndependentAssuranceModel;
  }
  throw Error("unknown SemanticDescriptor provenance", line, 1);
}

SemanticGapKind parse_gap_kind(std::string_view value, std::size_t line = 0) {
  if (value == "unmodeled") return SemanticGapKind::Unmodeled;
  if (value == "abstracted") return SemanticGapKind::Abstracted;
  if (value == "unsupported") return SemanticGapKind::Unsupported;
  if (value == "external-runtime") return SemanticGapKind::ExternalRuntime;
  throw Error("unknown SemanticDescriptor gap kind", line, 1);
}

const SemanticState& state_named(const SemanticDescriptor& descriptor,
                                 std::string_view name) {
  const auto found = std::find_if(descriptor.states.begin(), descriptor.states.end(),
                                  [&](const auto& item) { return item.name == name; });
  if (found == descriptor.states.end()) throw Error("unknown semantic state '" + std::string(name) + "'");
  return *found;
}

const SemanticEvent& event_named(const SemanticDescriptor& descriptor,
                                 std::string_view name) {
  const auto found = std::find_if(descriptor.events.begin(), descriptor.events.end(),
                                  [&](const auto& item) { return item.name == name; });
  if (found == descriptor.events.end()) throw Error("unknown semantic event '" + std::string(name) + "'");
  return *found;
}

const SemanticPort& port_named(const SemanticDescriptor& descriptor,
                               std::string_view name) {
  const auto found = std::find_if(descriptor.ports.begin(), descriptor.ports.end(),
                                  [&](const auto& item) { return item.name == name; });
  if (found == descriptor.ports.end()) throw Error("unknown semantic action port '" + std::string(name) + "'");
  return *found;
}

void validate(const SemanticDescriptor& descriptor) {
  if (descriptor.format != semantic_descriptor_format_v1) {
    throw Error("unsupported SemanticDescriptor format");
  }
  if (!identifier(descriptor.name)) throw Error("invalid SemanticDescriptor name");
  if (descriptor.origin.kind.empty() || descriptor.origin.locator.empty() ||
      !digest(descriptor.origin.digest)) {
    throw Error("SemanticDescriptor origin requires kind, locator and lowercase sha256");
  }
  fragment(descriptor.origin.kind, "origin kind");
  fragment(descriptor.origin.locator, "origin locator");
  if (descriptor.states.empty() || descriptor.events.empty() || descriptor.transitions.empty()) {
    throw Error("SemanticDescriptor needs states, events and transitions");
  }
  bounded(descriptor.ports, "semantic ports");
  bounded(descriptor.states, "semantic states");
  bounded(descriptor.events, "semantic events");
  bounded(descriptor.transitions, "semantic transitions");
  bounded(descriptor.lifecycle_mappings, "lifecycle mappings");
  bounded(descriptor.gaps, "semantic gaps");
  std::set<std::string, std::less<>> names;
  for (const auto& port : descriptor.ports) {
    if (!path(port.name) || !names.insert(port.name).second) {
      throw Error("invalid or duplicate semantic port '" + port.name + "'");
    }
    bounded(port.parameter_types, "port parameters");
    for (const auto& type : port.parameter_types) type_fragment(type);
  }
  names.clear();
  std::size_t initial_states = 0;
  std::set<std::string, std::less<>> semantic_paths;
  for (const auto& state : descriptor.states) {
    if (!identifier(state.name) || !names.insert(state.name).second) {
      throw Error("invalid or duplicate semantic state '" + state.name + "'");
    }
    if (!state.context.empty() && !identifier(state.context)) throw Error("invalid state context");
    initial_states += state.initial ? 1U : 0U;
    bounded(state.fields, "state fields");
    bounded(state.invariants, "state invariants");
    std::set<std::string, std::less<>> fields;
    for (const auto& field : state.fields) {
      if (!identifier(field.name) || !fields.insert(field.name).second) {
        throw Error("invalid or duplicate semantic field '" + field.name + "'");
      }
      type_fragment(field.type);
      fragment(field.initial, "field initializer");
      if (!field.semantic_path.empty() && !path(field.semantic_path)) {
        throw Error("invalid semantic field mapping path");
      }
      if (!field.semantic_path.empty() && !semantic_paths.insert(field.semantic_path).second) {
        throw Error("duplicate semantic field mapping path '" + field.semantic_path + "'");
      }
    }
    if (state.fields.empty()) throw Error("semantic state must define data fields");
    std::set<std::string, std::less<>> invariants;
    for (const auto& invariant : state.invariants) {
      if (!identifier(invariant.id) || !invariants.insert(invariant.id).second) {
        throw Error("invalid or duplicate invariant id");
      }
      fragment(invariant.expression, "invariant expression");
    }
  }
  if (initial_states != 1U) throw Error("SemanticDescriptor needs exactly one initial state");
  names.clear();
  for (const auto& event : descriptor.events) {
    if (!identifier(event.name) || !names.insert(event.name).second) {
      throw Error("invalid or duplicate semantic event '" + event.name + "'");
    }
    std::set<std::string, std::less<>> fields;
    bounded(event.fields, "event fields");
    for (const auto& field : event.fields) {
      if (!identifier(field.name) || !fields.insert(field.name).second) {
        throw Error("invalid or duplicate event field");
      }
      type_fragment(field.type);
    }
  }
  names.clear();
  for (const auto& transition : descriptor.transitions) {
    if (!identifier(transition.name) || !names.insert(transition.name).second) {
      throw Error("invalid or duplicate semantic transition '" + transition.name + "'");
    }
    static_cast<void>(state_named(descriptor, transition.from));
    const auto& target = state_named(descriptor, transition.to);
    static_cast<void>(event_named(descriptor, transition.event));
    bounded(transition.requirements, "transition requirements");
    bounded(transition.selections, "transition selections");
    bounded(transition.assignments, "transition assignments");
    bounded(transition.actions, "transition actions");
    std::set<std::string, std::less<>> outputs;
    const auto check_output = [&](const std::string& name) {
      if (!outputs.insert(name).second) throw Error("transition writes field twice: " + name);
      if (std::none_of(target.fields.begin(), target.fields.end(),
                       [&](const auto& field) { return field.name == name; })) {
        throw Error("transition writes unknown target field '" + name + "'");
      }
    };
    std::set<std::string, std::less<>> requirement_ids;
    for (const auto& requirement : transition.requirements) {
      if (!identifier(requirement.id) || !requirement_ids.insert(requirement.id).second) {
        throw Error("invalid or duplicate requirement id");
      }
      fragment(requirement.predicate, "requirement predicate");
    }
    for (const auto& selection : transition.selections) {
      check_output(selection.target_field);
      if (!identifier(selection.binding)) throw Error("invalid selection binding");
      fragment(selection.domain, "selection domain");
      fragment(selection.predicate, "selection predicate");
      if (selection.lex_score.empty()) throw Error("deterministic selection needs lex score");
      for (const auto& score : selection.lex_score) fragment(score, "selection score");
    }
    for (const auto& assignment : transition.assignments) {
      check_output(assignment.target_field);
      fragment(assignment.expression, "assignment expression");
    }
    if (outputs.empty()) throw Error("semantic transition needs at least one target update");
    std::set<std::string, std::less<>> labels;
    std::set<std::uint32_t> stages;
    for (const auto& action : transition.actions) {
      if (!identifier(action.label) || !labels.insert(action.label).second) {
        throw Error("invalid or duplicate action label");
      }
      const auto& port = port_named(descriptor, action.port);
      if (action.arguments.size() != port.parameter_types.size()) {
        throw Error("action argument count does not match typed port '" + action.port + "'");
      }
      bounded(action.arguments, "action arguments");
      if (!action.context.empty() && !path(action.context)) throw Error("invalid action context");
      for (const auto& argument : action.arguments) fragment(argument, "action argument");
      stages.insert(action.stage);
    }
    if (!stages.empty()) {
      std::uint32_t expected = 0;
      for (const auto stage : stages) {
        if (stage != expected++) throw Error("action stages must be contiguous from zero");
      }
    }
  }
  names.clear();
  std::set<std::pair<std::string, std::string>> lifecycle_targets;
  for (const auto& mapping : descriptor.lifecycle_mappings) {
    if (!identifier(mapping.id) || !names.insert(mapping.id).second || mapping.values.empty()) {
      throw Error("invalid, duplicate or empty lifecycle mapping");
    }
    const auto& state = state_named(descriptor, mapping.state);
    bounded(mapping.values, "lifecycle values");
    const auto field = std::find_if(state.fields.begin(), state.fields.end(),
                                    [&](const auto& item) { return item.name == mapping.field; });
    if (field == state.fields.end() || !field->lifecycle) {
      throw Error("lifecycle mapping must reference a lifecycle field");
    }
    if (!lifecycle_targets.emplace(mapping.state, mapping.field).second) {
      throw Error("lifecycle field has multiple mappings");
    }
    std::set<std::string, std::less<>> phases;
    std::set<std::string, std::less<>> literals;
    for (const auto& value : mapping.values) {
      if (!identifier(value.phase) || !phases.insert(value.phase).second ||
          !literals.insert(value.literal).second) {
        throw Error("invalid, duplicate or ambiguous lifecycle value");
      }
      fragment(value.literal, "lifecycle literal");
    }
  }
  names.clear();
  for (const auto& gap : descriptor.gaps) {
    if (!identifier(gap.id) || !names.insert(gap.id).second) {
      throw Error("invalid or duplicate semantic gap id");
    }
    fragment(gap.subject, "gap subject");
    fragment(gap.detail, "gap detail");
  }
}

class SourceWriter {
 public:
  void line(std::string value, std::string descriptor_path) {
    source_ += std::move(value);
    source_ += '\n';
    map_.push_back({++line_, std::move(descriptor_path)});
  }
  void blank() { source_ += '\n'; ++line_; }
  GeneratedSemanticSource take() && { return {std::move(source_), std::move(map_)}; }
 private:
  std::string source_;
  std::vector<SourceMapEntry> map_;
  std::size_t line_{0};
};

std::string action_text(const SemanticAction& action) {
  std::string result = action.label + ": $" + action.port + "(";
  for (std::size_t index = 0; index < action.arguments.size(); ++index) {
    if (index != 0U) result += ", ";
    result += action.arguments[index];
  }
  result += ")";
  if (!action.context.empty()) result += " @ " + action.context;
  return result;
}

void verify_source_map(const GeneratedSemanticSource& generated) {
  std::set<std::size_t> mapped_lines;
  for (const auto& entry : generated.source_map) {
    if (entry.generated_line == 0U || entry.descriptor_path.empty() ||
        !mapped_lines.insert(entry.generated_line).second) {
      throw Error("generated SemanticDescriptor source map is invalid");
    }
  }
  std::istringstream input(generated.source);
  std::string line;
  std::size_t line_number = 0;
  while (std::getline(input, line)) {
    ++line_number;
    if (!line.empty() && !mapped_lines.contains(line_number)) {
      throw Error("generated SemanticDescriptor source line is unmapped");
    }
    if (line.empty() && mapped_lines.contains(line_number)) {
      throw Error("blank generated SemanticDescriptor line is mapped");
    }
  }
  if (!mapped_lines.empty() && *mapped_lines.rbegin() > line_number) {
    throw Error("generated SemanticDescriptor source map exceeds source");
  }
}

void verify_lifecycle_literals(const SemanticDescriptor& descriptor) {
  for (const auto& mapping : descriptor.lifecycle_mappings) {
    const auto& state = state_named(descriptor, mapping.state);
    const auto field = std::find_if(state.fields.begin(), state.fields.end(),
                                    [&](const auto& item) {
                                      return item.name == mapping.field;
                                    });
    for (const auto& value : mapping.values) {
      const std::string check_source =
          "state LifecycleLiteralCheck initial:\n  value: " + field->type + " = " +
          value.literal + "\n";
      try {
        static_cast<void>(parse(check_source));
      } catch (const Error& error) {
        throw Error("lifecycle value '" + value.phase + "' does not match field type: " +
                    error.what());
      }
    }
  }
}

}  // namespace

std::string_view semantic_provenance_name(SemanticProvenance provenance) noexcept {
  switch (provenance) {
    case SemanticProvenance::GeneratedOperationalMirror:
      return "generated-operational-mirror";
    case SemanticProvenance::IndependentAssuranceModel:
      return "independent-assurance-model";
  }
  return "invalid";
}

std::string_view semantic_gap_kind_name(SemanticGapKind kind) noexcept {
  switch (kind) {
    case SemanticGapKind::Unmodeled: return "unmodeled";
    case SemanticGapKind::Abstracted: return "abstracted";
    case SemanticGapKind::Unsupported: return "unsupported";
    case SemanticGapKind::ExternalRuntime: return "external-runtime";
  }
  return "invalid";
}

std::string print_semantic_descriptor(const SemanticDescriptor& input) {
  validate(input);
  const SemanticDescriptor descriptor = canonicalized(input);
  std::ostringstream output;
  output << "dtessl.semantic " << descriptor.format << ' ' << quoted(descriptor.name) << ' '
         << semantic_provenance_name(descriptor.provenance) << '\n';
  output << "origin " << quoted(descriptor.origin.kind) << ' '
         << quoted(descriptor.origin.locator) << ' ' << quoted(descriptor.origin.digest) << '\n';
  for (const auto& port : descriptor.ports) {
    output << "port " << quoted(port.name) << ' ' << port.parameter_types.size();
    for (const auto& type : port.parameter_types) output << ' ' << quoted(type);
    output << '\n';
  }
  for (const auto& state : descriptor.states) {
    output << "state " << quoted(state.name) << ' ' << quoted(state.context) << ' '
           << (state.initial ? 1 : 0) << '\n';
    for (const auto& field : state.fields) {
      output << "field " << quoted(state.name) << ' ' << quoted(field.name) << ' '
             << quoted(field.type) << ' ' << quoted(field.initial) << ' '
             << quoted(field.semantic_path) << ' ' << (field.lifecycle ? 1 : 0) << '\n';
    }
    for (const auto& invariant : state.invariants) {
      output << "invariant " << quoted(state.name) << ' ' << quoted(invariant.id) << ' '
             << quoted(invariant.expression) << '\n';
    }
  }
  for (const auto& event : descriptor.events) {
    output << "event " << quoted(event.name) << '\n';
    for (const auto& field : event.fields) {
      output << "event.field " << quoted(event.name) << ' ' << quoted(field.name) << ' '
             << quoted(field.type) << '\n';
    }
  }
  for (const auto& transition : descriptor.transitions) {
    output << "transition " << quoted(transition.name) << ' ' << quoted(transition.event) << ' '
           << quoted(transition.from) << ' ' << quoted(transition.to) << '\n';
    for (const auto& requirement : transition.requirements) {
      output << "require " << quoted(transition.name) << ' ' << quoted(requirement.id) << ' '
             << quoted(requirement.predicate) << '\n';
    }
    for (const auto& selection : transition.selections) {
      output << "select " << quoted(transition.name) << ' ' << quoted(selection.target_field)
             << ' ' << quoted(selection.binding) << ' ' << quoted(selection.domain) << ' '
             << quoted(selection.predicate) << ' ' << selection.lex_score.size();
      for (const auto& score : selection.lex_score) output << ' ' << quoted(score);
      output << '\n';
    }
    for (const auto& assignment : transition.assignments) {
      output << "assign " << quoted(transition.name) << ' ' << quoted(assignment.target_field)
             << ' ' << quoted(assignment.expression) << '\n';
    }
    for (const auto& action : transition.actions) {
      output << "action " << quoted(transition.name) << ' ' << action.stage << ' '
             << quoted(action.label) << ' ' << quoted(action.port) << ' '
             << quoted(action.context) << ' ' << action.arguments.size();
      for (const auto& argument : action.arguments) output << ' ' << quoted(argument);
      output << '\n';
    }
  }
  for (const auto& mapping : descriptor.lifecycle_mappings) {
    output << "lifecycle " << quoted(mapping.id) << ' ' << quoted(mapping.state) << ' '
           << quoted(mapping.field) << ' ' << mapping.values.size();
    for (const auto& value : mapping.values) {
      output << ' ' << quoted(value.phase) << ' ' << quoted(value.literal);
    }
    output << '\n';
  }
  for (const auto& gap : descriptor.gaps) {
    output << "gap " << quoted(gap.id) << ' ' << semantic_gap_kind_name(gap.kind) << ' '
           << quoted(gap.subject) << ' ' << quoted(gap.detail) << '\n';
  }
  output << "end\n";
  return output.str();
}

SemanticDescriptor parse_semantic_descriptor(std::string_view text) {
  if (text.size() > semantic_descriptor_size_limit) {
    throw Error("SemanticDescriptor exceeds size limit");
  }
  SemanticDescriptor descriptor;
  std::unordered_map<std::string, std::size_t> states;
  std::unordered_map<std::string, std::size_t> events;
  std::unordered_map<std::string, std::size_t> transitions;
  bool header = false;
  bool origin = false;
  bool ended = false;
  std::istringstream lines{std::string(text)};
  std::string line_text;
  std::size_t line_number = 0;
  while (std::getline(lines, line_text)) {
    ++line_number;
    if (!line_text.empty() && line_text.back() == '\r') line_text.pop_back();
    if (line_text.empty()) continue;
    std::istringstream input(line_text);
    std::string command;
    input >> command;
    if (!header) {
      if (command != "dtessl.semantic") throw Error("missing SemanticDescriptor header", line_number, 1);
      descriptor.format = number<std::uint32_t>(input, line_number, "format");
      descriptor.name = read_quoted(input, line_number, "descriptor name");
      std::string provenance;
      if (!(input >> provenance)) throw Error("missing provenance", line_number, 1);
      descriptor.provenance = parse_provenance(provenance, line_number);
      finish(input, line_number);
      header = true;
      continue;
    }
    if (ended) throw Error("data follows SemanticDescriptor end", line_number, 1);
    if (command == "origin") {
      if (origin) throw Error("duplicate origin", line_number, 1);
      descriptor.origin.kind = read_quoted(input, line_number, "origin kind");
      descriptor.origin.locator = read_quoted(input, line_number, "origin locator");
      descriptor.origin.digest = read_quoted(input, line_number, "origin digest");
      finish(input, line_number); origin = true; continue;
    }
    if (command == "port") {
      SemanticPort value;
      value.name = read_quoted(input, line_number, "port name");
      value.parameter_types = strings(input, line_number, "port parameter type");
      finish(input, line_number); descriptor.ports.push_back(std::move(value)); continue;
    }
    if (command == "state") {
      SemanticState value;
      value.name = read_quoted(input, line_number, "state name");
      value.context = read_quoted(input, line_number, "state context");
      value.initial = boolean(input, line_number, "initial flag");
      finish(input, line_number);
      if (!states.emplace(value.name, descriptor.states.size()).second) {
        throw Error("duplicate state", line_number, 1);
      }
      descriptor.states.push_back(std::move(value)); continue;
    }
    if (command == "field" || command == "invariant") {
      const auto state_name = read_quoted(input, line_number, "state name");
      const auto found = states.find(state_name);
      if (found == states.end()) throw Error("field/invariant precedes state", line_number, 1);
      auto& state = descriptor.states[found->second];
      if (command == "field") {
        SemanticField value;
        value.name = read_quoted(input, line_number, "field name");
        value.type = read_quoted(input, line_number, "field type");
        value.initial = read_quoted(input, line_number, "field initializer");
        value.semantic_path = read_quoted(input, line_number, "semantic path");
        value.lifecycle = boolean(input, line_number, "lifecycle flag");
        state.fields.push_back(std::move(value));
      } else {
        SemanticInvariant value;
        value.id = read_quoted(input, line_number, "invariant id");
        value.expression = read_quoted(input, line_number, "invariant expression");
        state.invariants.push_back(std::move(value));
      }
      finish(input, line_number); continue;
    }
    if (command == "event") {
      SemanticEvent value;
      value.name = read_quoted(input, line_number, "event name");
      finish(input, line_number);
      if (!events.emplace(value.name, descriptor.events.size()).second) {
        throw Error("duplicate event", line_number, 1);
      }
      descriptor.events.push_back(std::move(value)); continue;
    }
    if (command == "event.field") {
      const auto event_name = read_quoted(input, line_number, "event name");
      const auto found = events.find(event_name);
      if (found == events.end()) throw Error("event field precedes event", line_number, 1);
      SemanticParameter value;
      value.name = read_quoted(input, line_number, "event field name");
      value.type = read_quoted(input, line_number, "event field type");
      finish(input, line_number);
      descriptor.events[found->second].fields.push_back(std::move(value)); continue;
    }
    if (command == "transition") {
      SemanticTransition value;
      value.name = read_quoted(input, line_number, "transition name");
      value.event = read_quoted(input, line_number, "transition event");
      value.from = read_quoted(input, line_number, "source state");
      value.to = read_quoted(input, line_number, "target state");
      finish(input, line_number);
      if (!transitions.emplace(value.name, descriptor.transitions.size()).second) {
        throw Error("duplicate transition", line_number, 1);
      }
      descriptor.transitions.push_back(std::move(value)); continue;
    }
    if (command == "require" || command == "select" || command == "assign" ||
        command == "action") {
      const auto transition_name = read_quoted(input, line_number, "transition name");
      const auto found = transitions.find(transition_name);
      if (found == transitions.end()) throw Error("transition detail precedes transition", line_number, 1);
      auto& transition = descriptor.transitions[found->second];
      if (command == "require") {
        SemanticRequirement value;
        value.id = read_quoted(input, line_number, "requirement id");
        value.predicate = read_quoted(input, line_number, "requirement predicate");
        transition.requirements.push_back(std::move(value));
      } else if (command == "select") {
        SemanticSelection value;
        value.target_field = read_quoted(input, line_number, "selection target");
        value.binding = read_quoted(input, line_number, "selection binding");
        value.domain = read_quoted(input, line_number, "selection domain");
        value.predicate = read_quoted(input, line_number, "selection predicate");
        value.lex_score = strings(input, line_number, "selection score");
        transition.selections.push_back(std::move(value));
      } else if (command == "assign") {
        SemanticAssignment value;
        value.target_field = read_quoted(input, line_number, "assignment target");
        value.expression = read_quoted(input, line_number, "assignment expression");
        transition.assignments.push_back(std::move(value));
      } else {
        SemanticAction value;
        value.stage = number<std::uint32_t>(input, line_number, "action stage");
        value.label = read_quoted(input, line_number, "action label");
        value.port = read_quoted(input, line_number, "action port");
        value.context = read_quoted(input, line_number, "action context");
        value.arguments = strings(input, line_number, "action argument");
        transition.actions.push_back(std::move(value));
      }
      finish(input, line_number); continue;
    }
    if (command == "lifecycle") {
      LifecycleMapping value;
      value.id = read_quoted(input, line_number, "lifecycle mapping id");
      value.state = read_quoted(input, line_number, "lifecycle state");
      value.field = read_quoted(input, line_number, "lifecycle field");
      const auto count = number<std::uint32_t>(input, line_number, "lifecycle value count");
      if (count > 1024U) throw Error("too many lifecycle values", line_number, 1);
      for (std::uint32_t index = 0; index < count; ++index) {
        value.values.push_back({read_quoted(input, line_number, "lifecycle phase"),
                                read_quoted(input, line_number, "lifecycle literal")});
      }
      finish(input, line_number); descriptor.lifecycle_mappings.push_back(std::move(value)); continue;
    }
    if (command == "gap") {
      SemanticGap value;
      value.id = read_quoted(input, line_number, "gap id");
      std::string kind;
      if (!(input >> kind)) throw Error("missing gap kind", line_number, 1);
      value.kind = parse_gap_kind(kind, line_number);
      value.subject = read_quoted(input, line_number, "gap subject");
      value.detail = read_quoted(input, line_number, "gap detail");
      finish(input, line_number); descriptor.gaps.push_back(std::move(value)); continue;
    }
    if (command == "end") {
      finish(input, line_number); ended = true; continue;
    }
    throw Error("unknown SemanticDescriptor directive '" + command + "'", line_number, 1);
  }
  if (!header || !origin || !ended) throw Error("incomplete SemanticDescriptor");
  validate(descriptor);
  return descriptor;
}

std::string semantic_descriptor_digest(const SemanticDescriptor& descriptor) {
  return sha256(print_semantic_descriptor(descriptor));
}

void verify_semantic_origin(const SemanticDescriptor& descriptor,
                            const SemanticOrigin& expected,
                            SemanticProvenance expected_provenance) {
  validate(descriptor);
  if (descriptor.origin != expected) {
    throw Error("SemanticDescriptor origin binding mismatch");
  }
  if (descriptor.provenance != expected_provenance) {
    throw Error("SemanticDescriptor provenance binding mismatch");
  }
}

GeneratedSemanticSource generate_dtessl(const SemanticDescriptor& input) {
  validate(input);
  const SemanticDescriptor descriptor = canonicalized(input);
  SourceWriter output;
  for (const auto& port : descriptor.ports) {
    std::string declaration = "port " + port.name + "(";
    for (std::size_t index = 0; index < port.parameter_types.size(); ++index) {
      if (index != 0U) declaration += ", ";
      declaration += port.parameter_types[index];
    }
    declaration += ")";
    output.line(std::move(declaration), "ports." + port.name);
  }
  if (!descriptor.ports.empty()) output.blank();
  for (const auto& state : descriptor.states) {
    std::string header = "state " + state.name;
    if (!state.context.empty()) header += " @ " + state.context;
    if (state.initial) header += " initial";
    header += ":";
    output.line(std::move(header), "states." + state.name);
    for (const auto& field : state.fields) {
      output.line("  " + field.name + ": " + field.type + " = " + field.initial,
                  "states." + state.name + ".fields." + field.name);
    }
    for (const auto& invariant : state.invariants) {
      output.line("  invariant:", "states." + state.name + ".invariants." + invariant.id);
      output.line("    " + invariant.expression,
                  "states." + state.name + ".invariants." + invariant.id);
    }
    output.blank();
  }
  for (const auto& transition : descriptor.transitions) {
    const auto& event = event_named(descriptor, transition.event);
    std::string header = "transition " + transition.name + " @ " + event.name + "(";
    for (std::size_t index = 0; index < event.fields.size(); ++index) {
      if (index != 0U) header += ", ";
      header += event.fields[index].name + ": " + event.fields[index].type;
    }
    header += "):";
    output.line(std::move(header), "transitions." + transition.name);
    output.line("  from " + transition.from, "transitions." + transition.name + ".from");
    output.line("  to " + transition.to + ":", "transitions." + transition.name + ".to");
    for (const auto& selection : transition.selections) {
      std::string expression = "    " + selection.target_field + " = select " +
                               selection.binding + " in " + selection.domain + " where " +
                               selection.predicate + " by lex(";
      for (std::size_t index = 0; index < selection.lex_score.size(); ++index) {
        if (index != 0U) expression += ", ";
        expression += selection.lex_score[index];
      }
      expression += ")";
      output.line(std::move(expression), "transitions." + transition.name +
                                         ".select." + selection.target_field);
    }
    for (const auto& assignment : transition.assignments) {
      output.line("    " + assignment.target_field + " = " + assignment.expression,
                  "transitions." + transition.name + ".assign." + assignment.target_field);
    }
    if (!transition.requirements.empty()) {
      output.line("  where:", "transitions." + transition.name + ".requirements");
      for (std::size_t index = 0; index < transition.requirements.size(); ++index) {
        const auto& requirement = transition.requirements[index];
        output.line("    " + std::string(index == 0U ? "" : "and ") + requirement.predicate,
                    "transitions." + transition.name + ".requirements." + requirement.id);
      }
    }
    if (!transition.actions.empty()) {
      output.line("  do:", "transitions." + transition.name + ".actions");
      std::map<std::uint32_t, std::vector<const SemanticAction*>> stages;
      for (const auto& action : transition.actions) stages[action.stage].push_back(&action);
      std::string expression = "    ";
      bool first_stage = true;
      for (const auto& [stage, actions] : stages) {
        static_cast<void>(stage);
        if (!first_stage) expression += ", ";
        first_stage = false;
        if (actions.size() > 1U) expression += "(";
        for (std::size_t index = 0; index < actions.size(); ++index) {
          if (index != 0U) expression += " | ";
          expression += action_text(*actions[index]);
        }
        if (actions.size() > 1U) expression += ")";
      }
      output.line(std::move(expression), "transitions." + transition.name + ".actions");
    }
    output.blank();
  }
  return std::move(output).take();
}

SemanticCoverageManifest semantic_coverage(const SemanticDescriptor& descriptor,
                                            const GeneratedSemanticSource& generated) {
  validate(descriptor);
  SemanticCoverageManifest result;
  result.descriptor_digest = semantic_descriptor_digest(descriptor);
  result.generated_source_digest = sha256(generated.source);
  result.provenance = descriptor.provenance;
  result.states = descriptor.states.size();
  result.events = descriptor.events.size();
  result.transitions = descriptor.transitions.size();
  result.action_ports = descriptor.ports.size();
  result.lifecycle_mappings = descriptor.lifecycle_mappings.size();
  std::set<std::string, std::less<>> used_ports;
  for (const auto& state : descriptor.states) {
    result.invariants += state.invariants.size();
    for (const auto& field : state.fields) {
      ++result.fields;
      result.relation_fields += field.type.starts_with("relation<") ? 1U : 0U;
      result.mapped_fields += field.semantic_path.empty() ? 0U : 1U;
      result.lifecycle_fields += field.lifecycle ? 1U : 0U;
    }
  }
  for (const auto& transition : descriptor.transitions) {
    result.requirements += transition.requirements.size();
    result.deterministic_selections += transition.selections.size();
    for (const auto& action : transition.actions) used_ports.insert(action.port);
  }
  result.used_action_ports = used_ports.size();
  for (const auto& gap : descriptor.gaps) {
    switch (gap.kind) {
      case SemanticGapKind::Unmodeled: ++result.unmodeled_gaps; break;
      case SemanticGapKind::Abstracted: ++result.abstracted_gaps; break;
      case SemanticGapKind::Unsupported: ++result.unsupported_gaps; break;
      case SemanticGapKind::ExternalRuntime: ++result.external_runtime_gaps; break;
    }
  }
  result.structural_coverage_complete =
      result.states != 0U && result.events != 0U && result.relation_fields != 0U &&
      result.transitions != 0U && result.requirements != 0U &&
      result.deterministic_selections != 0U && result.invariants != 0U &&
      result.action_ports != 0U && result.used_action_ports == result.action_ports &&
      result.mapped_fields == result.fields && result.lifecycle_fields != 0U &&
      result.lifecycle_mappings == result.lifecycle_fields;
  result.gap_free = descriptor.gaps.empty();
  result.complete = result.structural_coverage_complete && result.gap_free;
  result.independent_assurance_claim =
      descriptor.provenance == SemanticProvenance::IndependentAssuranceModel;
  return result;
}

std::string print_source_map(const SemanticDescriptor& descriptor,
                             const GeneratedSemanticSource& generated) {
  std::ostringstream output;
  output << "dtessl-source-map 1\n"
         << "descriptor-digest " << quoted(semantic_descriptor_digest(descriptor)) << '\n'
         << "source-digest " << quoted(sha256(generated.source)) << '\n';
  for (const auto& entry : generated.source_map) {
    output << "line " << entry.generated_line << ' ' << quoted(entry.descriptor_path) << '\n';
  }
  output << "end\n";
  return output.str();
}

std::string print_semantic_coverage(const SemanticCoverageManifest& coverage) {
  std::ostringstream output;
  output << "dtessl-semantic-coverage 1\n"
         << "descriptor-digest " << quoted(coverage.descriptor_digest) << '\n'
         << "source-digest " << quoted(coverage.generated_source_digest) << '\n'
         << "provenance " << semantic_provenance_name(coverage.provenance) << '\n'
         << "states " << coverage.states << '\n'
         << "fields " << coverage.fields << '\n'
         << "events " << coverage.events << '\n'
         << "relation-fields " << coverage.relation_fields << '\n'
         << "transitions " << coverage.transitions << '\n'
         << "requirements " << coverage.requirements << '\n'
         << "deterministic-selections " << coverage.deterministic_selections << '\n'
         << "invariants " << coverage.invariants << '\n'
         << "action-ports " << coverage.action_ports << '\n'
         << "used-action-ports " << coverage.used_action_ports << '\n'
         << "mapped-fields " << coverage.mapped_fields << '\n'
         << "lifecycle-mappings " << coverage.lifecycle_mappings << '\n'
         << "lifecycle-fields " << coverage.lifecycle_fields << '\n'
         << "gaps.unmodeled " << coverage.unmodeled_gaps << '\n'
         << "gaps.abstracted " << coverage.abstracted_gaps << '\n'
         << "gaps.unsupported " << coverage.unsupported_gaps << '\n'
         << "gaps.external-runtime " << coverage.external_runtime_gaps << '\n'
         << "structural-coverage-complete "
         << (coverage.structural_coverage_complete ? "yes" : "no") << '\n'
         << "gap-free " << (coverage.gap_free ? "yes" : "no") << '\n'
         << "complete " << (coverage.complete ? "yes" : "no") << '\n'
         << "independent-assurance-claim "
         << (coverage.independent_assurance_claim ? "yes" : "no") << '\n'
         << "notice "
         << quoted("provenance is an author assertion; generated mirrors are not independent proofs")
         << "\nend\n";
  return output.str();
}

SemanticCheckResult check_semantic_descriptor(const SemanticDescriptor& descriptor) {
  GeneratedSemanticSource generated = generate_dtessl(descriptor);
  verify_source_map(generated);
  static_cast<void>(parse(generated.source));
  verify_lifecycle_literals(descriptor);
  return {generated, semantic_coverage(descriptor, generated)};
}

ParallelStepResult run_semantic_descriptor(const SemanticDescriptor& descriptor,
                                           const std::vector<Event>& events) {
  const auto checked = check_semantic_descriptor(descriptor);
  Engine engine(parse(checked.generated.source));
  return engine.step_parallel(events);
}

ParallelStepResult replay_semantic_descriptor(const SemanticDescriptor& descriptor,
                                              const std::vector<Event>& events) {
  const ParallelStepResult first = run_semantic_descriptor(descriptor, events);
  const ParallelStepResult second = run_semantic_descriptor(descriptor, events);
  if (first != second) throw Error("SemanticDescriptor replay diverged");
  return first;
}

TraceResult run_semantic_descriptor_trace(const SemanticDescriptor& descriptor,
                                          const EventTrace& trace) {
  const auto checked = check_semantic_descriptor(descriptor);
  return run_trace(parse(checked.generated.source), trace);
}

TraceResult replay_semantic_descriptor_trace(const SemanticDescriptor& descriptor,
                                             const EventTrace& trace) {
  const auto checked = check_semantic_descriptor(descriptor);
  return replay_trace(parse(checked.generated.source), trace);
}

}  // namespace dtessl
