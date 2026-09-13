#include "dtessl/dtessl.hpp"
#include "dtessl/backend.hpp"
#include "dtessl/language_service.hpp"
#include "dtessl/scratch_pool.hpp"
#include "dtessl/semantic_descriptor.hpp"
#include "dtessl/solver.hpp"
#include "dtessl/value_codec.hpp"
#include "dtessl/version.hpp"

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <set>
#include <string>

namespace {

constexpr std::string_view source = R"DTESSL(
newtype SessionId = string

record Session:
  id: SessionId
  owner: string
  active: bool

variant Decision:
  Accepted(Session)
  Rejected(string)

enum Phase:
  Idle
  Running

state Scheduler @ local initial:
  mode: string = "Idle"
  credits: int = 2
  workers: set<string> = {"worker-a", "worker-b"}
  busy: set<string> = {}
  last_round: int = 0
  note: string = ""
  tags: set<string> = {} merge union
  tag_count: int = 0
  numbers: set<int> = {3, 1} merge union
  queue: list<string> = ["a", "b"]
  weights: map<string, int> = {"a": 1, "b": 2}
  inventory: bag<string> = bag{"cpu": 2, "gpu": 1}
  nested: list<set<int>> = [{2, 1}, {4, 3}]
  session: Session = Session{id: SessionId("session-0"), owner: "root", active: true}
  decision: Decision = Decision.Rejected("not-decided")
  phase: Phase = Phase.Idle
  maybe_priority: option<int> = none
  outcome: result<int, string> = ok(0)
  huge: int = 9223372036854775808
  ratio: rational = 1/3
  invariant:
    credits >= 0 and count(workers) = 2 and count(busy) <= count(workers)
    and count(numbers) >= 2 and count(queue) = 2 and count(weights) = 2
    and count(inventory) = 2 and count(nested) = 2

transition Schedule @ Submit(task: string, worker: string):
  from Scheduler
  to Scheduler:
    mode = "Waiting"
    credits = before.credits - 1
    busy = insert(before.busy, worker)
    last_round = round
  where:
    before.mode = "Idle"
    and exists item in before.workers where item = worker
  do:
    accepted: $ipc.accept(task), (reserved: $pool.reserve(task) @ worker | logged: $log.add(task))

transition Complete @ Done(task: string, worker: string):
  from Scheduler
  to Scheduler:
    mode = "Idle"
    credits = before.credits + 1
    busy = erase(before.busy, worker)
    last_round = round
  where:
    before.mode = "Waiting"
  do:
    completed: $ipc.complete(task)

transition Mark @ Note(text: string):
  from Scheduler
  to Scheduler:
    note = text
  where:
    before.mode = "Idle"
  do:
    noted: $log.note(text)

transition AddTag @ Tag(value: string):
  from Scheduler
  to Scheduler:
    tags = insert(before.tags, value)

transition SnapshotTags @ Snapshot():
  from Scheduler
  to Scheduler:
    tag_count = count(before.tags)

transition AddNumber @ Number(value: int):
  from Scheduler
  to Scheduler:
    numbers = insert(before.numbers, value)

transition Decide @ Classify(value: int):
  from Scheduler
  to Scheduler:
    session = Session{id: SessionId("session-1"), owner: "alice", active: true}
    decision = Decision.Accepted(Session{id: SessionId("session-1"), owner: "alice", active: true})
    phase = Phase.Running
    maybe_priority = some(value)
    outcome = ok<string>(value)
    note = match before.decision { Decision.Accepted(found) -> found.owner, Decision.Rejected(reason) -> reason }
  where:
    match before.maybe_priority { none -> true, some(current) -> current >= 0 }

transition RejectDecision @ Reject(reason: string):
  from Scheduler
  to Scheduler:
    decision = Decision.Rejected(reason)
    phase = Phase.Idle
    maybe_priority = none<int>()
    outcome = err<int>(reason)
    note = match before.decision { Accepted(found) -> found.owner, Rejected(previous) -> previous }

transition Calculate @ Exact(delta: int):
  from Scheduler
  to Scheduler:
    huge = before.huge * delta + 1
    ratio = before.ratio + 1 / 6
  where:
    delta > 0

transition DivideZero @ Zero():
  from Scheduler
  to Scheduler:
    ratio = before.ratio / 0
)DTESSL";

constexpr std::string_view relation_source = R"DTESSL(
state Graph initial:
  edges: relation<string, string> = relation{("a", "b"), ("b", "c"), ("c", "d")}
  other: relation<string, string> = relation{("d", "e")}
  weights: relation<string, int> = relation{("a", 2), ("b", 1), ("c", 1), ("d", 3)}
  empty: relation<string> = relation{}
  reversed: relation<string, string> = relation{}
  paths: relation<string, string> = relation{}
  projected: relation<string> = relation{}
  joined: relation<string, string, string, int> = relation{}
  composed: relation<string, string> = relation{}
  united: relation<string, string> = relation{}
  common: relation<string, string> = relation{}
  remaining: relation<string, string> = relation{}
  int_paths: relation<int, int> = relation{}
  chosen: option<tuple<string, int>> = none
  universal: bool = false
  existential: bool = false
  empty_all: bool = false
  empty_any: bool = true
  invariant:
    tuple("a", "b") in edges

transition AnalyzeGraph @ Analyze(minimum: int):
  from Graph
  to Graph:
    reversed = inverse(before.edges)
    paths = closure(before.edges)
    projected = project(before.edges, 0)
    joined = join(before.edges, 1, before.weights, 0)
    composed = compose(before.edges, before.edges)
    united = union(before.edges, before.other)
    common = intersection(before.edges, before.other)
    remaining = difference(before.edges, before.other)
    chosen = select row in before.weights where row.1 >= minimum by lex(row.1, row.0)
    universal = A edge in before.edges: edge.0 != edge.1
    existential = E edge in before.edges: edge.0 = "a"
    empty_all = A item in before.empty: item.0 = "impossible"
    empty_any = E item in before.empty: item.0 = "impossible"
  where:
    (A edge in before.edges: edge.0 != edge.1)
    and (E edge in before.edges: edge.0 = "a")

transition AmbiguousChoice @ Pick():
  from Graph
  to Graph:
    chosen = select row in before.weights where row.1 >= 0 by lex(row.1)

transition CloseInput @ Close(input: relation<int, int>):
  from Graph
  to Graph:
    int_paths = closure(input)
)DTESSL";

constexpr std::string_view core_logic_source = R"DTESSL(
name WorkerId

record Worker:
  id: WorkerId
  capacity: int

state Scheduler initial:
  workers: ~Worker = ~{
    Worker{id: WorkerId(a), capacity: 2},
    Worker{id: WorkerId(b), capacity: 1},
    Worker{id: WorkerId(c), capacity: 3}
  }
  chosen: [Worker] = []
  edges: ~(WorkerId, WorkerId) = ~{
    (WorkerId(a), WorkerId(b)),
    (WorkerId(b), WorkerId(c))
  }
  connected: bool = false
  queue: list<WorkerId> = list[WorkerId(a), WorkerId(c)]
  had_choice: bool = false
  invariant:
    E worker ~ workers: worker.capacity > 0

transition Choose @ Submit(minimum: int):
  from Scheduler
  to Scheduler:
    chosen = select worker ~ before.workers where worker.capacity >= minimum by lex(worker.capacity, worker.id)
    connected = tuple(WorkerId(a), WorkerId(b)) ~ before.edges

transition Clear @ Reset():
  from Scheduler
  to Scheduler:
    chosen = []
    had_choice = match before.chosen { [] -> false, [worker] -> worker.capacity > 0 }
)DTESSL";

constexpr std::string_view anonymous_relation_source = R"DTESSL(
state Model initial:
  A: set<int> = {1, 2}
  B: set<int> = {2, 3}
  union_ab: set<int> = {}
  union_ba: set<int> = {}
  grouped: set<int> = {}

relation P1(x: int):
  x != 2

relation P2(x: int):
  x >= 2

transition Derive @ Go():
  from Model
  to Model:
    union_ab = {a: a in before.A, a ~ P1, b: b in before.B, b ~ P2}
    union_ba = {b: b in before.B, b ~ P2, a: a in before.A, a ~ P1}
    grouped = {a: a in before.A, (a ~ P1 or a = 2) and not (a = 3)}
)DTESSL";

[[noreturn]] void fail(const std::string& message) {
  std::cerr << "dtessl test failed: " << message << '\n';
  std::exit(1);
}

void require(bool condition, const std::string& message) {
  if (!condition) fail(message);
}

dtessl::SemanticDescriptor semantic_fixture() {
  dtessl::SemanticDescriptor descriptor;
  descriptor.name = "SchedulerMirror";
  descriptor.origin = {"declared-model-projection", "tests/scheduler",
                       std::string(64, 'a')};
  descriptor.ports = {{"audit.observe", {"string"}},
                      {"scheduler.reserve", {"string"}},
                      {"scheduler.commit", {"string"}}};
  descriptor.states = {{
      "Scheduler", "local", true,
      {{"mode", "string", "\"Idle\"", "scheduler.lifecycle", true},
       {"accepted", "int", "0", "scheduler.accepted", false},
       {"edges", "relation<string,string>",
        "relation{(\"submit\", \"reserve\")}", "scheduler.flow", false},
       {"weights", "relation<string,int>",
        "relation{(\"worker-a\", 2), (\"worker-b\", 1)}",
        "scheduler.weights", false},
       {"chosen", "option<tuple<string,int>>", "none", "scheduler.selection", false}},
      {{"accepted_nonnegative", "accepted >= 0"}}}};
  descriptor.events = {{"Submit", {{"task", "string"}, {"minimum", "int"}}}};
  descriptor.transitions = {{
      "Schedule", "Submit", "Scheduler", "Scheduler",
      {{"flow_exists", "E edge in before.edges: edge.0 = \"submit\""}},
      {{"chosen", "worker", "before.weights", "worker.1 >= minimum",
        {"worker.1", "worker.0"}}},
      {{"accepted", "before.accepted + 1"}, {"mode", "\"Active\""}},
      {{0, "reserve", "scheduler.reserve", "", {"task"}},
       {0, "audit", "audit.observe", "", {"task"}},
       {1, "commit", "scheduler.commit", "", {"task"}}}}};
  descriptor.lifecycle_mappings = {{
      "scheduler_lifecycle", "Scheduler", "mode",
      {{"idle", "\"Idle\""}, {"active", "\"Active\""}}}};
  descriptor.gaps = {{"physical_receipts", dtessl::SemanticGapKind::ExternalRuntime,
                      "provider receipts",
                      "owned by host runtime journal and excluded from this DTESSL model"}};
  return descriptor;
}

}  // namespace

int main() {
  require(dtessl::version == "0.4.8", "compiled version must be v0.4.8");
  constexpr std::string_view language_source =
      "// model\nstate Model initial:\n  value: int = 1\n";
  const dtessl::LanguageAnalysis language_analysis =
      dtessl::analyze_source(language_source, "test://model", 7);
  require(language_analysis.valid() && language_analysis.version == 7 &&
              language_analysis.uri == "test://model" &&
              std::any_of(language_analysis.highlights.begin(),
                          language_analysis.highlights.end(), [](const auto& token) {
                            return token.syntax == dtessl::SyntaxClass::Comment;
                          }) &&
              std::any_of(language_analysis.highlights.begin(),
                          language_analysis.highlights.end(), [](const auto& token) {
                            return token.syntax == dtessl::SyntaxClass::BuiltinType;
                          }),
          "language service did not reuse the parser or classify syntax spans");
  const dtessl::LanguageAnalysis syntax_error =
      dtessl::analyze_source("state Broken initial\n", "test://broken", 1);
  require(!syntax_error.valid() && syntax_error.diagnostics.size() == 1 &&
              syntax_error.diagnostics.front().code == "DTESSL1002" &&
              syntax_error.diagnostics.front().range.start.line == 1,
          "language service did not return a stable located parser diagnostic");
  const dtessl::LanguageAnalysis core_language_analysis =
      dtessl::analyze_source(core_logic_source, "test://core-logic", 3);
  const auto has_highlight = [&](dtessl::SyntaxClass syntax, std::string_view spelling) {
    return std::any_of(
        core_language_analysis.highlights.begin(), core_language_analysis.highlights.end(),
        [&](const auto& token) {
          return token.syntax == syntax &&
                 core_logic_source.substr(
                     token.range.start.offset,
                     token.range.end.offset - token.range.start.offset) == spelling;
        });
  };
  require(core_language_analysis.valid() &&
              has_highlight(dtessl::SyntaxClass::Keyword, "name") &&
              has_highlight(dtessl::SyntaxClass::Operator, "~") &&
              has_highlight(dtessl::SyntaxClass::Punctuation, "[") &&
              has_highlight(dtessl::SyntaxClass::BuiltinType, "list"),
          "core syntax did not pass production parsing and semantic highlighting");
  constexpr std::string_view semantic_error_source =
      "state Model initial:\n"
      "  value: int = 1\n"
      "\n"
      "transition Break @ Go(text: string):\n"
      "  from Model\n"
      "  to Model:\n"
      "    value = text\n";
  const dtessl::LanguageAnalysis semantic_error =
      dtessl::analyze_source(semantic_error_source, "test://semantic", 1);
  require(!semantic_error.valid() && semantic_error.diagnostics.front().code ==
              "DTESSL2001" &&
              semantic_error.diagnostics.front().range.start.line == 7 &&
              semantic_error.diagnostics.front().range.start.column == 5,
          "semantic diagnostic did not retain the offending assignment location");

  dtessl::LanguageService language_service;
  const std::string editable = "state Model initial:\n  value: int = 1\n";
  language_service.open("test://editable", editable, 1);
  const std::size_t one = editable.rfind('1');
  const dtessl::TextEdit replace_one{
      {{2, 16, one}, {2, 17, one + 1}}, "2"};
  language_service.change("test://editable", {replace_one}, 2);
  require(language_service.document("test://editable").source().find("= 2") !=
              std::string::npos &&
              language_service.analyze("test://editable").valid(),
          "versioned text edit did not update and re-verify the document");
  bool stale_edit_rejected = false;
  try {
    language_service.change("test://editable", {}, 2);
  } catch (const dtessl::Error&) {
    stale_edit_rejected = true;
  }
  require(stale_edit_rejected, "language service accepted a stale document version");
  language_service.close("test://editable");
  const auto roundtrip = [](const dtessl::Value& value) {
    const std::vector<std::uint8_t> encoded = dtessl::encode_value(value);
    require(dtessl::decode_value(encoded) == value, "canonical value roundtrip failed");
    require(dtessl::encode_value(dtessl::decode_value(encoded)) == encoded,
            "canonical value re-encoding changed bytes");
  };
  roundtrip(dtessl::Value(false));
  roundtrip(dtessl::Value(std::int64_t{-42}));
  roundtrip(dtessl::Value("hello"));
  roundtrip(dtessl::Value(dtessl::StringSet{{"alpha", "beta"}}));
  const dtessl::Value generic_set(
      dtessl::ValueSet{{dtessl::Value(std::int64_t{2}), dtessl::Value(std::int64_t{1})}});
  const dtessl::Value generic_list(dtessl::ValueList{
      {dtessl::Value(true), generic_set, dtessl::Value("nested")}});
  const dtessl::Value generic_map(dtessl::ValueMap{{
      {dtessl::Value("items"), generic_list},
      {dtessl::Value("name"), dtessl::Value("demo")},
  }});
  const dtessl::Value generic_bag(dtessl::ValueBag{{
      {dtessl::Value("x"), 2},
      {dtessl::Value("x"), 3},
      {dtessl::Value("y"), 1},
  }});
  roundtrip(generic_set);
  roundtrip(generic_list);
  roundtrip(generic_map);
  roundtrip(generic_bag);
  const dtessl::Value record(dtessl::ValueRecord{
      "Session", {{"owner", dtessl::Value("alice")},
                  {"active", dtessl::Value(true)},
                  {"id", dtessl::Value(dtessl::ValueNewtype{
                             "SessionId", {dtessl::Value("session-1")}})}}});
  const dtessl::Value variant(
      dtessl::ValueVariant{"Decision", "Accepted", {record}});
  const dtessl::Value enumeration(dtessl::ValueVariant{"Phase", "Running", {}});
  roundtrip(record);
  roundtrip(variant);
  roundtrip(enumeration);
  const dtessl::ExactInt huge =
      dtessl::ExactInt::parse("1234567890123456789012345678901234567890");
  const dtessl::ExactInt factor = dtessl::ExactInt::parse("98765432109876543210");
  require(((huge * factor) / factor) == huge && ((huge * factor) % factor).is_zero(),
          "arbitrary integer multiply/divide identity failed");
  for (std::int64_t left = -50; left <= 50; ++left) {
    for (std::int64_t right = -50; right <= 50; ++right) {
      const dtessl::ExactInt lhs(left);
      const dtessl::ExactInt rhs(right);
      require((lhs + rhs).to_int64() == left + right &&
                  (lhs - rhs).to_int64() == left - right &&
                  (lhs * rhs).to_int64() == left * right,
              "exact integer small-domain arithmetic disagrees with int64");
      if (right != 0) {
        require((lhs / rhs).to_int64() == left / right &&
                    (lhs % rhs).to_int64() == left % right,
                "exact integer signed division disagrees with truncation profile");
      }
    }
  }
  const dtessl::Rational half(dtessl::ExactInt::parse("-2"),
                              dtessl::ExactInt::parse("-4"));
  const dtessl::Rational third(1, 3);
  require(half.text() == "1/2" && (half + third).text() == "5/6" &&
              (half * third).text() == "1/6" && (half / third).text() == "3/2",
          "rational normalization or arithmetic failed");
  bool source_budget_error = false;
  try {
    static_cast<void>(dtessl::ExactInt::parse(std::string(4097, '9')));
  } catch (const dtessl::Error&) {
    source_budget_error = true;
  }
  require(source_budget_error, "exact integer source digit budget was not enforced");
  const dtessl::ExactInt limit_operand = dtessl::ExactInt::parse(std::string(4096, '9'));
  const dtessl::ExactInt wide = limit_operand * limit_operand;
  bool runtime_budget_error = false;
  try {
    static_cast<void>(wide * limit_operand);
  } catch (const dtessl::Error&) {
    runtime_budget_error = true;
  }
  require(runtime_budget_error, "exact integer runtime magnitude budget was not enforced");
  roundtrip(dtessl::Value(huge));
  roundtrip(dtessl::Value(-huge));
  roundtrip(dtessl::Value(half));
  const dtessl::Value tuple(dtessl::ValueTuple{
      {dtessl::Value("edge"), dtessl::Value(std::int64_t{7})}});
  const dtessl::Value relation(dtessl::ValueRelation{
      2,
      {
          dtessl::ValueTuple{{dtessl::Value("b"), dtessl::Value(std::int64_t{2})}},
          dtessl::ValueTuple{{dtessl::Value("a"), dtessl::Value(std::int64_t{1})}},
          dtessl::ValueTuple{{dtessl::Value("a"), dtessl::Value(std::int64_t{1})}},
      }});
  roundtrip(tuple);
  roundtrip(relation);
  const dtessl::Value higher_order_relation(dtessl::ValueRelation{
      1U, {dtessl::ValueTuple{{relation}}}});
  roundtrip(higher_order_relation);
  require(higher_order_relation.as_relation().rows.front().fields.front() ==
              relation,
          "a canonical RelationValue could not be nested as a relation row");
  const dtessl::Value worker_name(dtessl::ValueName{"WorkerId", "a"});
  roundtrip(worker_name);
  require(dtessl::value_text(worker_name) == "WorkerId(a)",
          "logical name was rendered as string text");
  const dtessl::Value rendered_relation(dtessl::ValueRelation{
      1U, {dtessl::ValueTuple{{worker_name}}}});
  require(dtessl::value_text(rendered_relation) == "relation{WorkerId(a)}",
          "canonical relation renderer retained the legacy prefix tilde form");

  const dtessl::Program core_logic = dtessl::parse(core_logic_source);
  const dtessl::FeatureSet core_features = dtessl::required_features(core_logic);
  require(core_features.contains(dtessl::LanguageFeature::LogicalNames) &&
              core_features.contains(dtessl::LanguageFeature::DirectRelationBinding),
          "backend negotiation omitted new core logical value semantics");
  dtessl::Engine core_engine(core_logic);
  const dtessl::StepResult chosen = core_engine.step(
      dtessl::Event{"Submit", {{"minimum", dtessl::Value(std::int64_t{2})}}});
  const dtessl::ValueVariant& chosen_value = chosen.state.at("chosen").as_variant();
  require(chosen_value.constructor == "some" && chosen_value.payload.size() == 1U,
          "unary relation select did not produce [Worker]");
  require(chosen.state.at("connected").as_bool(),
          "~(A, B) relation type or ~ membership failed");
  const dtessl::ValueRecord& selected_worker = chosen_value.payload.front().as_record();
  const auto selected_id = std::find_if(
      selected_worker.fields.begin(), selected_worker.fields.end(),
      [](const auto& field) { return field.first == "id"; });
  require(selected_id != selected_worker.fields.end() &&
              selected_id->second.as_name() == dtessl::ValueName{"WorkerId", "a"},
          "named lexicographic tie-break selected the wrong worker");
  const dtessl::StepResult cleared = core_engine.step(dtessl::Event{"Reset", {}});
  require(cleared.state.at("chosen").as_variant().constructor == "none" &&
              cleared.state.at("had_choice").as_bool(),
          "[]/[value] option matching failed");
  require(relation.as_relation().rows.size() == 2 &&
              relation.as_relation().rows.front().fields.front().as_string() == "a",
          "relation rows were not deduplicated and canonically sorted");
  dtessl::ValueRelation oversized_relation{1, {}};
  for (std::size_t index = 0; index <= dtessl::relation_row_limit; ++index) {
    oversized_relation.rows.push_back(
        dtessl::ValueTuple{{dtessl::Value(static_cast<std::int64_t>(index))}});
  }
  bool row_limit_error = false;
  try {
    static_cast<void>(dtessl::Value(std::move(oversized_relation)));
  } catch (const dtessl::Error&) {
    row_limit_error = true;
  }
  require(row_limit_error, "relation row limit was not enforced");
  require(record.as_record().fields.front().first == "active",
          "record fields were not canonically sorted");
  require(generic_set.as_set().values.front().as_int() == 1,
          "generic set did not canonicalize element order");
  require(generic_bag.as_bag().entries.front().second == 5,
          "generic bag did not combine duplicate multiplicities");
  std::vector<std::uint8_t> unsorted_generic_set{5, 2};
  const std::vector<std::uint8_t> encoded_two =
      dtessl::encode_value(dtessl::Value(std::int64_t{2}));
  const std::vector<std::uint8_t> encoded_one =
      dtessl::encode_value(dtessl::Value(std::int64_t{1}));
  unsorted_generic_set.insert(unsorted_generic_set.end(), encoded_two.begin(), encoded_two.end());
  unsorted_generic_set.insert(unsorted_generic_set.end(), encoded_one.begin(), encoded_one.end());
  bool generic_order_rejected = false;
  try {
    static_cast<void>(dtessl::decode_value(unsorted_generic_set));
  } catch (const dtessl::Error&) {
    generic_order_rejected = true;
  }
  require(generic_order_rejected, "decoder accepted an unsorted generic set");
  require(dtessl::encode_value(dtessl::Value(std::int64_t{42})) ==
              std::vector<std::uint8_t>({1, 0, 0, 0, 0, 0, 0, 0, 42}),
          "canonical int golden bytes changed");
  require(dtessl::encode_value(dtessl::Value(dtessl::StringSet{{"a", "b"}})) ==
              std::vector<std::uint8_t>({3, 2, 1, 'a', 1, 'b'}),
          "canonical set golden bytes changed");
  require(dtessl::encode_value(dtessl::Value(
              dtessl::ValueNewtype{"SessionId", {dtessl::Value("x")}})) ==
              std::vector<std::uint8_t>({10, 9, 'S', 'e', 's', 's', 'i', 'o', 'n', 'I', 'd',
                                         2, 1, 'x'}),
          "canonical newtype golden bytes changed");
  require(dtessl::encode_value(enumeration) ==
              std::vector<std::uint8_t>({9, 5, 'P', 'h', 'a', 's', 'e', 7,
                                         'R', 'u', 'n', 'n', 'i', 'n', 'g', 0}),
          "canonical enum golden bytes changed");
  require(dtessl::encode_value(dtessl::Value(
              dtessl::ExactInt::parse("9223372036854775808"))) ==
              std::vector<std::uint8_t>({11, 0, 8, 0x80, 0, 0, 0, 0, 0, 0, 0}),
          "canonical big integer golden bytes changed");
  require(dtessl::encode_value(dtessl::Value(dtessl::Rational(1, 2))) ==
              std::vector<std::uint8_t>({12,
                                         1, 0, 0, 0, 0, 0, 0, 0, 1,
                                         1, 0, 0, 0, 0, 0, 0, 0, 2}),
          "canonical rational golden bytes changed");
  require(dtessl::encode_value(tuple) ==
              std::vector<std::uint8_t>({13, 2,
                                         2, 4, 'e', 'd', 'g', 'e',
                                         1, 0, 0, 0, 0, 0, 0, 0, 7}),
          "canonical tuple golden bytes changed");
  require(dtessl::encode_value(dtessl::Value(dtessl::ValueRelation{
              2, {dtessl::ValueTuple{{dtessl::Value("a"),
                                      dtessl::Value(std::int64_t{1})}}}})) ==
              std::vector<std::uint8_t>({14, 2, 1,
                                         2, 1, 'a',
                                         1, 0, 0, 0, 0, 0, 0, 0, 1}),
          "canonical relation golden bytes changed");
  require(dtessl::encode_value(worker_name) ==
              std::vector<std::uint8_t>({15, 8, 'W', 'o', 'r', 'k', 'e', 'r', 'I', 'd',
                                         1, 'a'}),
          "canonical logical name golden bytes changed");
  bool noncanonical = false;
  try {
    static_cast<void>(dtessl::decode_value(
        std::vector<std::uint8_t>({3, 2, 1, 'b', 1, 'a'})));
  } catch (const dtessl::Error&) {
    noncanonical = true;
  }
  require(noncanonical, "decoder must reject unsorted set encodings");
  for (const std::vector<std::uint8_t>& malformed : {
           std::vector<std::uint8_t>{2, 0x80, 0x00},
           std::vector<std::uint8_t>{0, 1, 0},
           std::vector<std::uint8_t>{2, 4, 'a'},
           std::vector<std::uint8_t>{8, 1, 'T', 2, 1, 'b', 0, 0, 1, 'a', 0, 0},
           std::vector<std::uint8_t>{9, 1, 'T', 1, 'C', 2},
           std::vector<std::uint8_t>{10, 1, 'T'},
           std::vector<std::uint8_t>{11, 0, 1, 42},
           std::vector<std::uint8_t>{12,
                                     1, 0, 0, 0, 0, 0, 0, 0, 2,
                                     1, 0, 0, 0, 0, 0, 0, 0, 4},
           std::vector<std::uint8_t>{13, 0},
           std::vector<std::uint8_t>{14, 65, 0},
           std::vector<std::uint8_t>{14, 1, 0x81, 0x20},
           std::vector<std::uint8_t>{14, 2, 2,
                                     2, 1, 'b', 1, 0, 0, 0, 0, 0, 0, 0, 2,
                                     2, 1, 'a', 1, 0, 0, 0, 0, 0, 0, 0, 1},
           std::vector<std::uint8_t>{15, 8, 'W', 'o', 'r', 'k', 'e', 'r', 'I', 'd',
                                     3, 'a', '-', 'b'},
           std::vector<std::uint8_t>{15, 1, 'T'},
           std::vector<std::uint8_t>{9}}) {
    bool rejected_codec = false;
    try {
      static_cast<void>(dtessl::decode_value(malformed));
    } catch (const dtessl::Error&) {
      rejected_codec = true;
    }
    require(rejected_codec, "decoder accepted malformed canonical bytes");
  }
  bool limited = false;
  try {
    static_cast<void>(dtessl::decode_value(
        std::vector<std::uint8_t>{2, 2, 'o', 'k'},
        dtessl::ValueCodecLimits{4, 1, 1}));
  } catch (const dtessl::Error&) {
    limited = true;
  }
  require(limited, "decoder ignored configured value limits");
  const dtessl::Program program = dtessl::parse(source);
  const dtessl::FeatureSet features = dtessl::required_features(program);
  require(features.contains(dtessl::LanguageFeature::ParallelEventBag) &&
              features.contains(dtessl::LanguageFeature::UnionMerge) &&
              features.contains(dtessl::LanguageFeature::ActionDag),
          "program feature discovery is incomplete");
  dtessl::BackendDescriptor vm_backend{
      {"example", "vm", 1},
      {dtessl::Projection::Execute},
      features};
  vm_backend.features.erase(dtessl::LanguageFeature::UnionMerge);
  const dtessl::BackendCompatibility missing =
      dtessl::negotiate_backend(program, vm_backend, dtessl::Projection::Execute);
  require(!missing.compatible &&
              missing.missing == dtessl::FeatureSet{dtessl::LanguageFeature::UnionMerge},
          "backend negotiation must report typed missing features");
  vm_backend.features.insert(dtessl::LanguageFeature::UnionMerge);
  require(dtessl::negotiate_backend(program, vm_backend, dtessl::Projection::Execute).compatible,
          "a backend supporting every required feature must be accepted");
  const dtessl::BackendCompatibility wrong_projection =
      dtessl::negotiate_backend(program, vm_backend, dtessl::Projection::Monitor);
  require(!wrong_projection.compatible && !wrong_projection.projection_supported,
          "a backend must explicitly support the requested projection");
  dtessl::Event submit{"Submit", {{"task", dtessl::Value("task-1")},
                                    {"worker", dtessl::Value("worker-a")}}};

  dtessl::Engine engine(program);
  const dtessl::StepResult first = engine.step(submit);
  require(first.round == 1, "the first accepted batch must advance the round to one");
  require(first.transition == "Schedule", "wrong transition selected");
  require(first.input.kind == dtessl::OccurrenceInputKind::Event &&
              first.input.symbol == "Submit" && first.input.event == "Submit" &&
              first.input.fields.at("task").as_string() == "task-1" &&
              first.input.fields.at("worker").as_string() == "worker-a" &&
              first.input.target_procedure.empty() &&
              first.before_state.at("credits").as_int() == 2,
          "standalone trace decision did not retain its complete typed input/before snapshot");
  require(first.state.at("mode").as_string() == "Waiting", "state update was not committed");
  require(first.state.at("credits").as_int() == 1, "integer update is wrong");
  require(first.state.at("busy").as_string_set().values.contains("worker-a"),
          "set resource update is wrong");
  require(first.state.at("last_round").as_int() == 0,
          "round must expose the pre-batch simulation round");
  require(first.actions.calls.size() == 3, "action plan must contain three calls");
  require(first.actions.dependencies.size() == 2, "serial/parallel DAG is wrong");
  require(first.actions.dependencies[0] == std::pair<std::size_t, std::size_t>{0, 1},
          "reserve must depend on accept");
  require(first.actions.dependencies[1] == std::pair<std::size_t, std::size_t>{0, 2},
          "log must depend on accept");
  require(first.actions.calls[1].context == "worker-a", "event-bound @ context was not resolved");
  require(first.actions.calls[2].context == "local", "default state context was not applied");

  dtessl::Event done{"Done", {{"task", dtessl::Value("task-1")},
                                {"worker", dtessl::Value("worker-a")}}};
  const dtessl::StepResult second = engine.step(done);
  require(second.round == 2 && second.state.at("mode").as_string() == "Idle",
          "second event did not evolve the state");
  require(second.state.at("busy").as_string_set().values.empty(),
          "set resource release is wrong");
  require(second.state.at("last_round").as_int() == 1, "simulation round did not advance");
  require(second.causal_predecessors.contains(first.id),
          "a later state read must name the prior writer as a causal predecessor");

  dtessl::Engine replay(program);
  require(replay.step(submit) == first, "fresh replay must be byte-for-byte deterministic");

  bool rejected = false;
  try {
    dtessl::Engine invalid(program);
    static_cast<void>(invalid.step(dtessl::Event{
        "Submit", {{"task", dtessl::Value("task-2")},
                   {"worker", dtessl::Value("unknown")}}}));
  } catch (const dtessl::Error&) {
    rejected = true;
  }
  require(rejected, "a failed relational predicate must disable the transition");

  dtessl::Engine parallel_engine(program);
  const dtessl::ParallelStepResult parallel = parallel_engine.step_parallel(
      {submit, dtessl::Event{"Note", {{"text", dtessl::Value("same-round")}}}});
  require(parallel.round == 1 && parallel.transitions.size() == 2,
          "parallel transitions must share one simulation round");
  require(parallel.state.at("mode").as_string() == "Waiting" &&
              parallel.state.at("note").as_string() == "same-round",
          "disjoint parallel writes were not committed atomically");
  require(parallel.transitions[0].round == parallel.transitions[1].round,
          "parallel decisions must not receive an artificial total order");
  require(parallel.transitions[0].causal_predecessors.empty() &&
              parallel.transitions[1].causal_predecessors.empty(),
          "same-round decisions must not become causal predecessors");
  dtessl::Engine permuted_engine(program);
  const dtessl::ParallelStepResult permuted = permuted_engine.step_parallel(
      {dtessl::Event{"Note", {{"text", dtessl::Value("same-round")}}}, submit});
  require(permuted == parallel, "event bag input order must not affect the decision set");

  bool conflict = false;
  try {
    dtessl::Engine conflicting(program);
    static_cast<void>(conflicting.step_parallel(
        {dtessl::Event{"Note", {{"text", dtessl::Value("a")}}},
         dtessl::Event{"Note", {{"text", dtessl::Value("b")}}}}));
  } catch (const dtessl::Error&) {
    conflict = true;
  }
  require(conflict, "parallel writes need an explicit merge relation");

  dtessl::Engine merging(program);
  const dtessl::ParallelStepResult merged = merging.step_parallel(
      {dtessl::Event{"Tag", {{"value", dtessl::Value("blue")}}},
       dtessl::Event{"Tag", {{"value", dtessl::Value("green")}}}});
  require(merged.state.at("tags").as_string_set().values ==
              std::set<std::string, std::less<>>{"blue", "green"},
          "typed union merge did not combine concurrent writes");
  const dtessl::StepResult snapshot = merging.step(dtessl::Event{"Snapshot", {}});
  require(snapshot.state.at("tag_count").as_int() == 2,
          "the next round did not observe the merged value");
  require(snapshot.causal_predecessors.size() == 2,
          "a merged field must retain every same-round causal writer");

  dtessl::Engine generic_collections(program);
  const dtessl::ParallelStepResult numbers = generic_collections.step_parallel(
      {dtessl::Event{"Number", {{"value", dtessl::Value(std::int64_t{4})}}},
       dtessl::Event{"Number", {{"value", dtessl::Value(std::int64_t{5})}}}});
  require(numbers.state.at("numbers").as_set().values.size() == 4 &&
              numbers.state.at("numbers").as_set().values.front().as_int() == 1,
          "generic set execution or union merge failed");
  require(numbers.state.at("weights").as_map().entries.size() == 2 &&
              numbers.state.at("inventory").as_bag().entries.size() == 2 &&
              numbers.state.at("nested").as_list().values.size() == 2,
          "generic collection initial values were not preserved");

  dtessl::Engine algebraic(program);
  const dtessl::StepResult classified = algebraic.step(
      dtessl::Event{"Classify", {{"value", dtessl::Value(std::int64_t{7})}}});
  require(classified.state.at("session").as_record().type_id == "Session" &&
              classified.state.at("session").as_record().fields.at(2).second.as_string() ==
                  "alice",
          "record construction or canonical field projection failed");
  require(classified.state.at("decision").as_variant().constructor == "Accepted" &&
              classified.state.at("phase").as_variant().constructor == "Running",
          "variant or enum construction failed");
  require(classified.state.at("maybe_priority").as_variant().type_id == "option<int>" &&
              classified.state.at("maybe_priority").as_variant().payload.front().as_int() == 7,
          "option construction lost its canonical type identity");
  require(classified.state.at("outcome").as_variant().type_id == "result<int,string>" &&
              classified.state.at("note").as_string() == "not-decided",
          "result construction or exhaustive match execution failed");
  const dtessl::StepResult rejected_decision = algebraic.step(
      dtessl::Event{"Reject", {{"reason", dtessl::Value("policy")}}});
  require(rejected_decision.state.at("note").as_string() == "alice" &&
              rejected_decision.state.at("maybe_priority").as_variant().constructor == "none" &&
              rejected_decision.state.at("maybe_priority").as_variant().type_id == "option<int>" &&
              rejected_decision.state.at("outcome").as_variant().constructor == "err" &&
              rejected_decision.state.at("outcome").as_variant().payload.front().as_string() == "policy",
          "payload match, typed none, or err construction failed");

  dtessl::Engine numeric(program);
  const dtessl::StepResult calculated = numeric.step(
      dtessl::Event{"Exact", {{"delta", dtessl::Value(std::int64_t{2})}}});
  require(calculated.state.at("huge").as_exact_int().text() ==
              "18446744073709551617" &&
              calculated.state.at("ratio").as_rational().text() == "1/2",
          "exact numeric transition arithmetic failed");
  dtessl::Engine divide_zero(program);
  bool zero_error = false;
  try {
    static_cast<void>(divide_zero.step(dtessl::Event{"Zero", {}}));
  } catch (const dtessl::Error&) {
    zero_error = true;
  }
  require(zero_error && divide_zero.current_round() == 0 &&
              divide_zero.values().at("ratio").as_rational().text() == "1/3",
          "division by zero must reject the round without committing state");

  const dtessl::Program relation_program = dtessl::parse(relation_source);
  const dtessl::FeatureSet relation_features = dtessl::required_features(relation_program);
  require(relation_features.contains(dtessl::LanguageFeature::RelationAlgebra) &&
              relation_features.contains(dtessl::LanguageFeature::UniversalSearch) &&
              relation_features.contains(dtessl::LanguageFeature::DeterministicSelect),
          "relation/search feature discovery is incomplete");
  const std::vector<dtessl::SearchPlanSummary> plans = dtessl::search_plans(relation_program);
  require(std::any_of(plans.begin(), plans.end(), [](const auto& plan) {
            return plan.operation == "transition-id-index" && plan.deterministic;
          }) &&
              std::any_of(plans.begin(), plans.end(), [](const auto& plan) {
                return plan.operation == "state-signature-index" && plan.deterministic;
              }) &&
              std::any_of(plans.begin(), plans.end(), [](const auto& plan) {
            return plan.operation == "select-by-lex" && plan.deterministic &&
                   plan.rejects_ambiguous_score &&
                   plan.max_rows == dtessl::relation_row_limit;
          }) &&
              std::any_of(plans.begin(), plans.end(), [](const auto& plan) {
                return plan.operation == "closure" &&
                       plan.max_work == dtessl::relation_work_limit;
              }),
          "static search plan summaries lost determinism or budget metadata");
  dtessl::Engine graph(relation_program);
  const dtessl::StepResult analyzed = graph.step(
      dtessl::Event{"Analyze", {{"minimum", dtessl::Value(std::int64_t{1})}}});
  require(analyzed.state.at("reversed").as_relation().rows.size() == 3 &&
              analyzed.state.at("paths").as_relation().rows.size() == 6 &&
              analyzed.state.at("projected").as_relation().rows.size() == 3 &&
              analyzed.state.at("joined").as_relation().rows.size() == 3 &&
              analyzed.state.at("composed").as_relation().rows.size() == 2,
          "relation project/join/compose/inverse/closure result is wrong");
  require(analyzed.state.at("united").as_relation().rows.size() == 4 &&
              analyzed.state.at("common").as_relation().rows.empty() &&
              analyzed.state.at("remaining").as_relation().rows.size() == 3,
          "relation union/intersection/difference result is wrong");
  const dtessl::ValueVariant& selected = analyzed.state.at("chosen").as_variant();
  require(selected.constructor == "some" &&
              selected.payload.front().as_tuple().fields.front().as_string() == "b" &&
              analyzed.state.at("universal").as_bool() &&
              analyzed.state.at("existential").as_bool() &&
              analyzed.state.at("empty_all").as_bool() &&
              !analyzed.state.at("empty_any").as_bool(),
          "E/A or deterministic lex selection result is wrong");

  dtessl::Engine no_candidate(relation_program);
  const dtessl::StepResult none_selected = no_candidate.step(
      dtessl::Event{"Analyze", {{"minimum", dtessl::Value(std::int64_t{99})}}});
  require(none_selected.state.at("chosen").as_variant().constructor == "none",
          "select with no candidate must return typed none");

  dtessl::Engine ambiguous(relation_program);
  bool ambiguity_error = false;
  try {
    static_cast<void>(ambiguous.step(dtessl::Event{"Pick", {}}));
  } catch (const dtessl::Error&) {
    ambiguity_error = true;
  }
  require(ambiguity_error && ambiguous.current_round() == 0,
          "ambiguous lex score must reject the round without implicit tie-breaking");

  dtessl::ValueRelation long_chain{2, {}};
  for (std::int64_t index = 0; index < 1001; ++index) {
    long_chain.rows.push_back(dtessl::ValueTuple{
        {dtessl::Value(index), dtessl::Value(index + 1)}});
  }
  dtessl::Engine bounded_search(relation_program);
  bool budget_error = false;
  try {
    static_cast<void>(bounded_search.step(
        dtessl::Event{"Close", {{"input", dtessl::Value(std::move(long_chain))}}}));
  } catch (const dtessl::Error&) {
    budget_error = true;
  }
  require(budget_error && bounded_search.current_round() == 0,
          "relation work budget must reject expansion without committing state");

  const dtessl::Program anonymous_relation_program =
      dtessl::parse(anonymous_relation_source);
  const auto anonymous_plans = dtessl::search_plans(anonymous_relation_program);
  require(std::count_if(anonymous_plans.begin(), anonymous_plans.end(),
                        [](const auto& plan) {
                          return plan.operation == "typed-relation-membership";
                        }) == 2,
          "typed decidable relations were not exposed as membership plans");
  dtessl::Engine anonymous_relation_engine(anonymous_relation_program);
  const dtessl::StepResult anonymous_result =
      anonymous_relation_engine.step(dtessl::Event{"Go", {}});
  require(anonymous_result.state.at("union_ab") ==
              anonymous_result.state.at("union_ba") &&
              anonymous_result.state.at("union_ab").as_set().values.size() == 3U,
          "unordered anonymous relation union depends on source branch order");
  require(anonymous_result.state.at("grouped").as_set().values ==
              std::vector<dtessl::Value>{dtessl::Value(std::int64_t{1}),
                                         dtessl::Value(std::int64_t{2})},
          "parenthesized anonymous-relation filter precedence is wrong");

  bool derived_cycle_rejected = false;
  try {
    static_cast<void>(dtessl::parse(R"DTESSL(
state Model initial:
  values: relation int = relation{1}

relation Left: relation int = Right
relation Right: relation int = Left
)DTESSL"));
  } catch (const dtessl::Error&) {
    derived_cycle_rejected = true;
  }
  require(derived_cycle_rejected,
          "a cyclic derived RelationPlan dependency was accepted");

  bool unknown_relation_claim_rejected = false;
  try {
    static_cast<void>(dtessl::parse(R"DTESSL(
state Model initial:
  value: int = 0

Claim Missing @ relation Unknown:
  true
)DTESSL"));
  } catch (const dtessl::Error&) {
    unknown_relation_claim_rejected = true;
  }
  require(unknown_relation_claim_rejected,
          "relation Claim accepted an unknown RelationPlan target");

  const dtessl::Program carrier_property_program = dtessl::parse(R"DTESSL(
state Model initial:
  carrier: set<int> = {1, 2}
  relation_value: relation <int, int> = relation{<1, 1>, <2, 2>, <3, 3>}
  valid: bool = true

transition Check @ Go():
  from Model
  to Model:
    valid = equivalence(before.relation_value, before.carrier)
)DTESSL");
  dtessl::Engine carrier_property_engine(carrier_property_program);
  const dtessl::StepResult carrier_property_result =
      carrier_property_engine.step(dtessl::Event{"Go", {}});
  require(!carrier_property_result.state.at("valid").as_bool(),
          "carrier-relative relation property ignored rows outside the carrier");

  const dtessl::SemanticDescriptor semantic = semantic_fixture();
  const std::string canonical_descriptor = dtessl::print_semantic_descriptor(semantic);
  const dtessl::SemanticDescriptor decoded_descriptor =
      dtessl::parse_semantic_descriptor(canonical_descriptor);
  dtessl::verify_semantic_origin(
      decoded_descriptor, semantic.origin,
      dtessl::SemanticProvenance::GeneratedOperationalMirror);
  require(dtessl::print_semantic_descriptor(decoded_descriptor) == canonical_descriptor,
          "SemanticDescriptor canonical parse/print is unstable");
  dtessl::SemanticDescriptor reordered_descriptor = semantic;
  std::reverse(reordered_descriptor.ports.begin(), reordered_descriptor.ports.end());
  std::reverse(reordered_descriptor.states.front().fields.begin(),
               reordered_descriptor.states.front().fields.end());
  std::reverse(reordered_descriptor.transitions.front().actions.begin(),
               reordered_descriptor.transitions.front().actions.end());
  require(dtessl::semantic_descriptor_digest(reordered_descriptor) ==
              dtessl::semantic_descriptor_digest(semantic),
          "SemanticDescriptor digest depends on non-semantic record order");
  bool origin_binding_error = false;
  try {
    dtessl::verify_semantic_origin(
        decoded_descriptor,
        dtessl::SemanticOrigin{"declared-model-projection", "tests/other",
                               std::string(64, 'a')},
        dtessl::SemanticProvenance::GeneratedOperationalMirror);
  } catch (const dtessl::Error&) {
    origin_binding_error = true;
  }
  require(origin_binding_error, "SemanticDescriptor origin mismatch was accepted");
  const dtessl::SemanticCheckResult semantic_check =
      dtessl::check_semantic_descriptor(decoded_descriptor);
  require(semantic_check.coverage.structural_coverage_complete &&
              !semantic_check.coverage.complete && !semantic_check.coverage.gap_free &&
              semantic_check.coverage.external_runtime_gaps == 1 &&
              !semantic_check.coverage.independent_assurance_claim &&
              semantic_check.coverage.relation_fields == 2 &&
              semantic_check.coverage.deterministic_selections == 1 &&
              semantic_check.coverage.action_ports == 3,
          "SemanticDescriptor coverage or provenance classification is wrong");
  require(semantic_check.coverage.descriptor_digest.size() == 64 &&
              semantic_check.coverage.generated_source_digest ==
                  "8b7b34570f814a0f5d95b590b9468fc07c3fbf746625f6578290ba072846a761" &&
              !semantic_check.generated.source_map.empty() &&
              dtessl::print_source_map(decoded_descriptor, semantic_check.generated)
                      .find("transitions.Schedule.select.chosen") != std::string::npos,
          "SemanticDescriptor digest or source map is missing");
  const dtessl::Program generated_program = dtessl::parse(semantic_check.generated.source);
  require(dtessl::required_features(generated_program)
              .contains(dtessl::LanguageFeature::TypedActionPorts),
          "generated model did not preserve typed action ports");
  dtessl::BackendDescriptor untyped_backend{
      {"example", "untyped", 1}, {dtessl::Projection::Execute},
      dtessl::required_features(generated_program)};
  untyped_backend.features.erase(dtessl::LanguageFeature::TypedActionPorts);
  const dtessl::BackendCompatibility untyped_compatibility =
      dtessl::negotiate_backend(generated_program, untyped_backend,
                                dtessl::Projection::Execute);
  require(!untyped_compatibility.compatible &&
              untyped_compatibility.missing.contains(
                  dtessl::LanguageFeature::TypedActionPorts),
          "backend negotiation ignored typed action-port support");
  const dtessl::Event semantic_event{
      "Submit", {{"minimum", dtessl::Value(std::int64_t{1})},
                   {"task", dtessl::Value("task-1")}}};
  const dtessl::ParallelStepResult semantic_run =
      dtessl::run_semantic_descriptor(decoded_descriptor, {semantic_event});
  const dtessl::ParallelStepResult semantic_replay =
      dtessl::replay_semantic_descriptor(decoded_descriptor, {semantic_event});
  require(semantic_run == semantic_replay && semantic_run.transitions.size() == 1 &&
              semantic_run.transitions.front().actions.calls.size() == 3 &&
              semantic_run.transitions.front().actions.dependencies.size() == 2 &&
              semantic_run.state.at("accepted").as_int() == 1,
          "SemanticDescriptor check/run/replay or typed ActionPlan is wrong");
  const dtessl::EventTrace native_trace{{{{semantic_event}}, {{semantic_event}}}};
  const dtessl::TraceResult trace_result =
      dtessl::replay_semantic_descriptor_trace(decoded_descriptor, native_trace);
  require(trace_result.rounds.size() == 2 &&
              trace_result.final_state.at("accepted").as_int() == 2,
          "native DTESSL EventTrace replay did not preserve sequential logical rounds");

  dtessl::SemanticDescriptor forbidden_authority = semantic;
  forbidden_authority.states.front().fields.front().type = "Handle";
  bool authority_error = false;
  try {
    static_cast<void>(dtessl::check_semantic_descriptor(forbidden_authority));
  } catch (const dtessl::Error&) {
    authority_error = true;
  }
  require(authority_error,
          "SemanticDescriptor was allowed to mint a Handle/Binding/Lease-like authority type");

  dtessl::SemanticDescriptor wrong_port = semantic;
  wrong_port.ports.front().parameter_types.front() = "int";
  bool port_type_error = false;
  try {
    static_cast<void>(dtessl::check_semantic_descriptor(wrong_port));
  } catch (const dtessl::Error&) {
    port_type_error = true;
  }
  require(port_type_error, "typed action port accepted an argument of the wrong type");

  dtessl::SemanticDescriptor bad_lifecycle = semantic;
  bad_lifecycle.lifecycle_mappings.front().values.front().literal = "7";
  bool lifecycle_type_error = false;
  try {
    static_cast<void>(dtessl::check_semantic_descriptor(bad_lifecycle));
  } catch (const dtessl::Error&) {
    lifecycle_type_error = true;
  }
  require(lifecycle_type_error, "lifecycle mapping bypassed its field type");

  dtessl::SemanticDescriptor ambient_effect = semantic;
  ambient_effect.transitions.front().assignments.front().expression = "$host.call()";
  bool ambient_effect_error = false;
  try {
    static_cast<void>(dtessl::check_semantic_descriptor(ambient_effect));
  } catch (const dtessl::Error&) {
    ambient_effect_error = true;
  }
  require(ambient_effect_error, "descriptor expression admitted an ambient host call");

  bool descriptor_limit_error = false;
  try {
    static_cast<void>(dtessl::parse_semantic_descriptor(
        std::string(dtessl::semantic_descriptor_size_limit + 1U, 'x')));
  } catch (const dtessl::Error&) {
    descriptor_limit_error = true;
  }
  require(descriptor_limit_error, "oversized SemanticDescriptor was accepted");

  bool empty_batch_error = false;
  dtessl::EventTrace empty_batch_trace;
  empty_batch_trace.rounds.push_back(dtessl::EventBatch{});
  try {
    static_cast<void>(dtessl::run_trace(generated_program, empty_batch_trace));
  } catch (const dtessl::Error&) {
    empty_batch_error = true;
  }
  require(empty_batch_error, "native EventTrace accepted an empty logical round");

  dtessl::ScratchPool pool(64, 128);
  struct Pair {
    std::int64_t first;
    std::int64_t second;
  };
  Pair* pair = pool.make<Pair>(7, 9);
  require(pair->first == 7 && pair->second == 9, "scratch pool construction failed");
  void* first_allocation = pair;
  require(pool.used() >= sizeof(Pair) && pool.reserved() == 64,
          "scratch pool accounting is wrong");
  pool.reset();
  require(pool.make<Pair>(1, 2) == first_allocation,
          "scratch pool must reuse retained blocks after reset");

  constexpr std::string_view invalid_types = R"DTESSL(
state Broken initial:
  count: int = 0

transition Break @ Value(value: string):
  from Broken
  to Broken:
    count = value
)DTESSL";
  bool type_error = false;
  try {
    static_cast<void>(dtessl::parse(invalid_types));
  } catch (const dtessl::Error&) {
    type_error = true;
  }
  require(type_error, "the verifier must reject a statically wrong assignment");

  constexpr std::string_view non_exhaustive = R"DTESSL(
variant Choice:
  Yes(int)
  No

state Broken initial:
  choice: Choice = Choice.No
  value: int = 0

transition Break @ Go():
  from Broken
  to Broken:
    value = match before.choice { Yes(found) -> found }
)DTESSL";
  bool match_error = false;
  try {
    static_cast<void>(dtessl::parse(non_exhaustive));
  } catch (const dtessl::Error&) {
    match_error = true;
  }
  require(match_error, "the verifier accepted a non-exhaustive variant match");

  constexpr std::string_view nominal_mismatch = R"DTESSL(
newtype UserId = string
newtype SessionId = string

state Broken initial:
  id: UserId = UserId("u")

transition Break @ Go(value: SessionId):
  from Broken
  to Broken:
    id = value
)DTESSL";
  bool nominal_error = false;
  try {
    static_cast<void>(dtessl::parse(nominal_mismatch));
  } catch (const dtessl::Error&) {
    nominal_error = true;
  }
  require(nominal_error, "distinct newtypes were treated as structurally interchangeable");

  constexpr std::string_view name_mismatch = R"DTESSL(
name WorkerId
name TaskId

state Broken initial:
  id: WorkerId = WorkerId(a)

transition Break @ Go(value: TaskId):
  from Broken
  to Broken:
    id = value
)DTESSL";
  bool name_type_error = false;
  try {
    static_cast<void>(dtessl::parse(name_mismatch));
  } catch (const dtessl::Error&) {
    name_type_error = true;
  }
  require(name_type_error, "distinct logical name types were interchangeable");

  bool invalid_name_atom = false;
  try {
    static_cast<void>(dtessl::Value(dtessl::ValueName{"WorkerId", "not-an-atom"}));
  } catch (const dtessl::Error&) {
    invalid_name_atom = true;
  }
  require(invalid_name_atom, "non-canonical logical name atom was accepted");

  constexpr std::string_view composite_trace_source = R"DTESSL(
function positive(value: int) -> bool:
  value > 0

state Idle @ scheduler initial:
  credits: int = 2
  invariant:
    credits >= 0

state Busy @ scheduler:
  credits: int = 0
  invariant:
    credits >= 0

state Ready @ process initial:
  started: bool = false

state Retrying @ process:
  started: bool = false

state Running @ process:
  started: bool = true

transition Dispatch():
  case ready (Idle @ scheduler, {Ready, Retrying} @ process) -> (Busy @ scheduler, Running @ process):
    where:
      positive(scheduler.credits)
    set:
      credits = before.scheduler.credits - 1 @ scheduler
      started = true @ process

transition BreakInvariant @ Break():
  case violate (Idle @ scheduler, Ready @ process) -> (Idle @ scheduler, Running @ process):
    set:
      credits = -1 @ scheduler
      started = true @ process

procedure RetryEntry @ system:
  initial (Idle @ scheduler, Retrying @ process)

trace Happy:
  replay:
    inject Dispatch() @ RetryEntry -> Dispatch.ready
  capture closed:
    state (Busy @ scheduler, Running @ process)
    transition (Dispatch.ready)

trace ProcessView @ system:
  capture projected:
    state (Running @ process)
    transition (Dispatch.ready)

Claim ReachedRunning @ Happy:
  eventually:
    process.started

Claim NonNegative @ Happy:
  always:
    scheduler.credits >= 0

Claim OneDispatch @ Happy:
  count Dispatch.ready <= 1

Claim OrderedStart @ Happy:
  eventually(<not process.started, process.started> ~ happens_before)
)DTESSL";
  const dtessl::Program composite_program = dtessl::parse(composite_trace_source);
  const dtessl::TraceSnapshot static_trace =
      dtessl::run_named_trace(composite_program, "Happy");
  require(static_trace.closed && static_trace.rounds.size() == 1U &&
              static_trace.rounds.front().transitions.front().from_state.find("Retrying") !=
                  std::string::npos &&
              static_trace.rounds.front().transitions.front().procedure == "RetryEntry" &&
              static_trace.procedure_history.at("RetryEntry").front().round == 1U &&
              static_trace.procedure_history.at("RetryEntry").front()
                      .procedure_revision == 1U &&
              static_trace.final_state.at("process.started").as_bool(),
          "static typed trace did not execute the composite state rewrite");
  const auto claim_results = dtessl::evaluate_named_trace(composite_program, "Happy");
  require(claim_results.size() == 4U &&
              std::all_of(claim_results.begin(), claim_results.end(),
                          [](const dtessl::ClaimEvaluation& claim) {
                            return claim.status == dtessl::ClaimStatus::Satisfied;
                          }),
          "closed trace claims did not reach satisfied");
  dtessl::Engine procedure_engine =
      dtessl::Engine::from_procedure(composite_program, "RetryEntry");
  require(procedure_engine.initial_context() == "system" &&
              procedure_engine.current_states().at("scheduler") == "Idle" &&
              procedure_engine.current_states().at("process") == "Retrying",
          "procedure did not supply only its declared Engine entry embedding");
  const dtessl::StepResult procedure_step =
      procedure_engine.step_transition(dtessl::TransitionInput{"Dispatch", {}});
  require(procedure_step.transition == "Dispatch.ready" &&
              procedure_engine.current_states().at("scheduler") == "Busy" &&
              procedure_engine.current_states().at("process") == "Running",
          "global transition engine did not advance a procedure entry");
  dtessl::Engine composite_engine(composite_program);
  const dtessl::StepResult composite_step =
      composite_engine.step_transition(dtessl::TransitionInput{"Dispatch", {}});
  require(composite_step.active_states.at("scheduler") == "Busy" &&
              composite_step.active_states.at("process") == "Running" &&
              composite_step.transition == "Dispatch.ready" &&
              composite_step.state.at("scheduler.credits").as_int() == 1,
          "composite state axes were not committed atomically");
  const dtessl::TraceSnapshot projected =
      composite_engine.captured_trace("ProcessView", true);
  require(projected.rounds.size() == 1U && !projected.causal_gaps.empty() &&
              projected.captured_paths.contains("Dispatch.ready") &&
              projected.rounds.front().transitions.front().transition == "Dispatch.ready" &&
              projected.final_state.contains("process.started") &&
              !projected.final_state.contains("scheduler.credits"),
          "projected dynamic trace did not enforce its @ context scope");
  dtessl::Engine rollback_engine(composite_program);
  bool invariant_error = false;
  try {
    static_cast<void>(rollback_engine.step(dtessl::Event{"Break", {}}));
  } catch (const dtessl::Error&) {
    invariant_error = true;
  }
  require(invariant_error && rollback_engine.current_round() == 0U &&
              rollback_engine.current_states().at("scheduler") == "Idle" &&
              rollback_engine.current_states().at("process") == "Ready" &&
              rollback_engine.values().at("scheduler.credits").as_int() == 2,
          "failed composite invariant partially committed another state axis");

  constexpr std::string_view persistent_procedure_replay = R"DTESSL(
state Counter @ counter initial:
  value: int = 0

transition Add @ Increment(delta: int):
  case stay (Counter @ counter) -> (Counter @ counter):
    set:
      value = before.value + delta @ counter

procedure SessionA @ alpha:
  initial (Counter @ counter)

procedure SessionB @ beta:
  initial (Counter @ counter)

trace Interleaved:
  replay:
    inject Add(1) @ SessionA -> Add.stay | inject Add(2) @ SessionB -> Add.stay
    inject Add(3) @ SessionA -> Add.stay
  capture closed:
    procedure (SessionA, SessionB)

Claim Persisted @ Interleaved:
  eventually:
    SessionA.value = 4 and SessionB.value = 2
)DTESSL";
  const dtessl::Program persistent_program =
      dtessl::parse(persistent_procedure_replay);
  const dtessl::LanguageAnalysis procedure_language_analysis =
      dtessl::analyze_source(persistent_procedure_replay, "test://procedure", 4);
  require(procedure_language_analysis.valid() &&
              std::any_of(
                  procedure_language_analysis.highlights.begin(),
                  procedure_language_analysis.highlights.end(),
                  [&](const dtessl::HighlightToken& token) {
                    return token.syntax == dtessl::SyntaxClass::Keyword &&
                           persistent_procedure_replay.substr(
                               token.range.start.offset,
                               token.range.end.offset - token.range.start.offset) == "inject";
                  }),
          "procedure transition replay did not pass production parsing and highlighting");
  const dtessl::TraceSnapshot persistent_trace =
      dtessl::run_named_trace(persistent_program, "Interleaved");
  require(persistent_trace.rounds.size() == 2U &&
              persistent_trace.rounds.front().transitions.size() == 2U &&
              persistent_trace.rounds.front().transitions[0].round == 1U &&
              persistent_trace.rounds.front().transitions[1].round == 1U &&
              persistent_trace.procedure_contexts.at("SessionA") == "alpha" &&
              persistent_trace.procedure_contexts.at("SessionB") == "beta" &&
              persistent_trace.procedure_states.at("SessionA").at("value").as_int() == 4 &&
              persistent_trace.procedure_states.at("SessionB").at("value").as_int() == 2 &&
              persistent_trace.rounds.back().transitions.front().procedure == "SessionA" &&
              persistent_trace.rounds.back().transitions.front().procedure_revision == 2U &&
              persistent_trace.rounds.back().transitions.front()
                      .causal_predecessors.contains("SessionA/r1:0") &&
              persistent_trace.procedure_history.at("SessionA").size() == 2U &&
              persistent_trace.procedure_history.at("SessionB").size() == 2U &&
              persistent_trace.procedure_history.at("SessionB").back().round == 2U &&
              persistent_trace.procedure_history.at("SessionB").back()
                      .procedure_revision == 1U &&
              persistent_trace.procedure_history.at("SessionB").back()
                      .transitions.empty() &&
              persistent_trace.procedure_history.at("SessionB").back()
                      .inputs.empty() &&
              persistent_trace.procedure_history.at("SessionB").back()
                      .before_state ==
                  persistent_trace.procedure_history.at("SessionB").back().state &&
              persistent_trace.replayable &&
              persistent_trace.procedure_artifacts.size() == 2U,
          "replay did not persist independent procedure state across global rounds");
  const dtessl::ParallelStepResult& captured_round_one =
      dtessl::captured_round_at(persistent_trace, 1U);
  const dtessl::ProcedureTraceFrame& captured_session_a =
      dtessl::captured_procedure_at(persistent_trace, "SessionA", 1U);
  require(captured_round_one.transitions.size() == 2U &&
              captured_round_one.transitions.front().input.kind ==
                  dtessl::OccurrenceInputKind::Transition &&
              captured_round_one.transitions.front().input.target_procedure ==
                  "SessionA" &&
              captured_round_one.transitions.front().input.target_context ==
                  "alpha" &&
              captured_round_one.transitions.front().input.fields.at("delta").as_int() == 1 &&
              captured_round_one.transitions.front().before_state.at("value").as_int() == 0 &&
              captured_session_a.inputs.size() == 1U &&
              captured_session_a.before_state.at("value").as_int() == 0 &&
              captured_session_a.state.at("value").as_int() == 1 &&
              captured_session_a.inputs.front() ==
                  captured_round_one.transitions.front().input,
          "closed trace did not place the immutable external input at its RoundId");
  const std::vector<dtessl::ProcedureArtifact> persistent_artifacts{
      persistent_trace.procedure_artifacts.at("SessionA"),
      persistent_trace.procedure_artifacts.at("SessionB")};
  const dtessl::TraceSnapshot golden_replay = dtessl::replay_procedures(
      persistent_program, persistent_artifacts,
      {{1U, "SessionA", "Add.stay"}, {1U, "SessionB", "Add.stay"},
       {2U, "SessionA", "Add.stay"}});
  require(golden_replay.replayable && golden_replay.rounds == persistent_trace.rounds &&
              golden_replay.procedure_states.at("SessionA").at("value").as_int() == 4 &&
              golden_replay.procedure_states.at("SessionB").at("value").as_int() == 2,
          "complete procedure artifact replay did not reconstruct searched decisions");
  bool search_mismatch = false;
  try {
    static_cast<void>(dtessl::replay_procedures(
        persistent_program, persistent_artifacts,
        {{2U, "SessionA", "Add.missing"}}));
  } catch (const dtessl::Error&) {
    search_mismatch = true;
  }
  require(search_mismatch,
          "search replay accepted an expected path that the engine did not derive");

  constexpr std::string_view indexed_transition_injection = R"DTESSL(
state Ready @ lane initial:
  value: int = 0

transition ChooseLeft @ Choose(value: int):
  case stay (Ready @ lane) -> (Ready @ lane):
    set:
      value = value @ lane

transition ChooseRight @ Choose(value: int):
  case stay (Ready @ lane) -> (Ready @ lane):
    set:
      value = value + 100 @ lane

procedure Indexed @ system:
  initial (Ready @ lane)
)DTESSL";
  const dtessl::Program indexed_program = dtessl::parse(indexed_transition_injection);
  bool open_event_ambiguous = false;
  try {
    dtessl::Engine open(indexed_program);
    static_cast<void>(open.step(dtessl::Event{
        "Choose", {{"value", dtessl::Value(std::int64_t{7})}}}));
  } catch (const dtessl::Error&) {
    open_event_ambiguous = true;
  }
  dtessl::RuntimeContext indexed_runtime(indexed_program);
  indexed_runtime.start("Indexed");
  require(indexed_runtime.current_round() == 0U &&
              indexed_runtime.artifact("Indexed").injections.empty(),
          "procedure startup must quiesce without inventing a transition occurrence");
  const dtessl::ParallelStepResult indexed_step = indexed_runtime.inject({
      {"Indexed", dtessl::TransitionInput{
                      "ChooseLeft", {{"value", dtessl::Value(std::int64_t{7})}}}}});
  require(open_event_ambiguous && indexed_step.transitions.size() == 1U &&
              indexed_step.transitions.front().transition == "ChooseLeft.stay" &&
              indexed_step.transitions.front().input.kind ==
                  dtessl::OccurrenceInputKind::Transition &&
              indexed_step.transitions.front().input.symbol == "ChooseLeft" &&
              indexed_step.transitions.front().input.event == "Choose" &&
              indexed_step.transitions.front().input.target_procedure == "Indexed" &&
              indexed_step.transitions.front().input.target_context == "system" &&
              indexed_step.transitions.front().input.fields.at("value").as_int() == 7 &&
              indexed_step.transitions.front().before_state.at("value").as_int() == 0 &&
              indexed_step.state.at("value").as_int() == 7,
          "TransitionId injection did not bypass unrelated event families");

  constexpr std::string_view case_transition_surface = R"DTESSL(
port audit.emit(int)

state A @ lane initial:
  value: int = 0

state B @ lane:
  value: int = 0

transition Move():
  case go (A @ lane) -> (B @ lane):
    set @ lane:
      value = 1
    do:
      emitted: $audit.emit(1) @ lane
)DTESSL";
  constexpr std::string_view relation_transition_surface = R"DTESSL(
port audit.emit(int)

state A @ lane initial:
  value: int = 0

state B @ lane:
  value: int = 0

transition Move:
  <A @ lane, B @ lane> [label=go, do=(emitted: $audit.emit(1) @ lane)]:
    set @ lane:
      value = 1
)DTESSL";
  const dtessl::Program relation_transition_program =
      dtessl::parse(relation_transition_surface);
  const dtessl::FeatureSet relation_transition_features =
      dtessl::required_features(relation_transition_program);
  require(relation_transition_features.contains(
              dtessl::LanguageFeature::TransitionRelations),
          "transition relation surface is absent from backend negotiation");
  const auto relation_transition_plans =
      dtessl::search_plans(relation_transition_program);
  require(std::any_of(
              relation_transition_plans.begin(), relation_transition_plans.end(),
              [](const dtessl::SearchPlanSummary& plan) {
                return plan.operation == "transition-relation-union" &&
                       plan.max_rows == 1U && plan.max_work == 1U &&
                       plan.deterministic;
              }),
          "transition relation union did not expose its bounded search plan");
  dtessl::Engine case_transition_engine(dtessl::parse(case_transition_surface));
  dtessl::Engine relation_transition_engine(relation_transition_program);
  const dtessl::StepResult case_transition_result =
      case_transition_engine.step_transition({"Move", {}});
  const dtessl::StepResult relation_transition_result =
      relation_transition_engine.step_transition({"Move", {}});
  require(case_transition_result == relation_transition_result,
          "relation and case transition surfaces did not lower identically");

  constexpr std::string_view named_relation_transition_surface = R"DTESSL(
port audit.emit(int)

state A @ lane initial:
  value: int = 0

state B @ lane:
  value: int = 0

relation MoveRelation @ lane:
  A [where=(value = 0)]

transition Move:
  MoveRelation -> B @ lane [do=(emitted: $audit.emit(1) @ lane)]:
    set @ lane:
      value = before.value + 1
)DTESSL";
  dtessl::Engine named_relation_transition_engine(
      dtessl::parse(named_relation_transition_surface));
  const dtessl::StepResult named_relation_transition_result =
      named_relation_transition_engine.step_transition({"Move", {}});
  require(named_relation_transition_result.active_states.at("lane") == "B" &&
              named_relation_transition_result.state.at("value").as_int() == 1 &&
              named_relation_transition_result.actions.calls.size() == 1U &&
              named_relation_transition_result.transition == "Move.MoveRelation",
          "named relation union or + do ActionPlan attachment is wrong");

  constexpr std::string_view named_do_surface = R"DTESSL(
name TaskId
name OperationId
port worker.start(TaskId, OperationId)
port audit.accept(TaskId)

do StartTask(task: TaskId, operation: OperationId) @ worker [context=fixed, idempotent_by=(<task, operation>), result=consistent_by(<task, operation>), delivery=at_least_once, ordering=ordered_by(task), retry=safe, replay=suppress]:
  invoke: $worker.start(task, operation)

do RecordAccepted(task: TaskId) @ audit [context=fixed, delivery=at_most_once, retry=forbidden, replay=suppress]:
  append: $audit.accept(task)

state Idle @ scheduler initial:
  accepted: int = 0
state Busy @ scheduler:
  accepted: int = 0

transition Dispatch @ Submit(task: TaskId, operation: OperationId):
  <Idle @ scheduler, Busy @ scheduler> + (StartTask(task, operation), RecordAccepted(task)) [label=accept]:
    set @ scheduler:
      accepted = before.accepted + 1
)DTESSL";
  const dtessl::Program named_do_program = dtessl::parse(named_do_surface);
  const dtessl::FeatureSet named_do_features =
      dtessl::required_features(named_do_program);
  require(named_do_features.contains(dtessl::LanguageFeature::NamedDo) &&
              named_do_features.contains(dtessl::LanguageFeature::EffectContracts),
          "named do contract is absent from backend feature negotiation");
  dtessl::Engine named_do_engine(named_do_program);
  const dtessl::StepResult named_do_result = named_do_engine.step(
      {"Submit", {{"task", dtessl::Value(dtessl::ValueName{"TaskId", "task_a"})},
                  {"operation", dtessl::Value(dtessl::ValueName{
                                    "OperationId", "operation_1"})}}});
  require(named_do_result.transition == "Dispatch.accept" &&
              named_do_result.active_states.at("scheduler") == "Busy" &&
              named_do_result.actions.calls.size() == 2U &&
              named_do_result.actions.dependencies ==
                  std::vector<std::pair<std::size_t, std::size_t>>{{0U, 1U}},
          "named do did not compose with TransitionRelation and ActionPlan DAG");
  const dtessl::ActionCall& start_task = named_do_result.actions.calls[0];
  require(start_task.label == "StartTask.invoke" &&
              start_task.effect == "StartTask" && start_task.context == "worker" &&
              start_task.contract.context == dtessl::EffectContextPolicy::Fixed &&
              start_task.contract.result == dtessl::EffectResultPolicy::Consistent &&
              start_task.contract.delivery ==
                  dtessl::EffectDeliveryPolicy::AtLeastOnce &&
              start_task.contract.ordering ==
                  dtessl::EffectOrderingPolicy::Ordered &&
              start_task.contract.retry == dtessl::EffectRetryPolicy::Safe &&
              start_task.contract.replay == dtessl::EffectReplayPolicy::Suppress &&
              start_task.contract.idempotency_key &&
              start_task.contract.consistency_key &&
              start_task.contract.ordering_key,
          "named do did not materialize its typed effect contract");
  require(start_task.contract.idempotency_key->kind() == dtessl::Value::Kind::Tuple &&
              start_task.contract.idempotency_key->as_tuple().fields.size() == 2U,
          "named do idempotency key did not preserve canonical tuple structure");
  require(dtessl::result_text(named_do_result).find(
              "do StartTask [context=fixed, idempotent_by=") !=
              std::string::npos,
          "named do contract is absent from the deterministic result projection");
  dtessl::Engine named_do_replay(named_do_program);
  const dtessl::StepResult named_do_replayed = named_do_replay.step(
      {"Submit", named_do_result.input.fields});
  require(named_do_replayed == named_do_result,
          "named do contract is not deterministic under DTESSL replay");

  constexpr std::string_view nondeterministic_do_surface = R"DTESSL(
port entropy.draw(int)
do Draw(size: int) @ entropy [context=fixed, result=nondeterministic, retry=forbidden, replay=reinject]:
  invoke: $entropy.draw(size)
state Ready initial:
  value: int = 0
transition DrawNow @ DrawEvent(size: int):
  <Ready, Ready> + Draw(size)
)DTESSL";
  dtessl::Engine nondeterministic_do_engine(
      dtessl::parse(nondeterministic_do_surface));
  const dtessl::StepResult nondeterministic_do_result =
      nondeterministic_do_engine.step(
          {"DrawEvent", {{"size", dtessl::Value(std::int64_t{32})}}});
  require(nondeterministic_do_result.actions.calls.size() == 1U &&
              nondeterministic_do_result.actions.calls.front().contract.result ==
                  dtessl::EffectResultPolicy::Nondeterministic &&
              nondeterministic_do_result.actions.calls.front().contract.replay ==
                  dtessl::EffectReplayPolicy::Reinject &&
              nondeterministic_do_result.state.at("value").as_int() == 0,
          "nondeterministic do did not remain an outbound plan without a result binding");

  bool nondeterministic_replay_error = false;
  try {
    (void)dtessl::parse(R"DTESSL(
port entropy.draw(int)
do Draw(size: int) @ entropy [context=fixed, result=nondeterministic, replay=suppress]:
  invoke: $entropy.draw(size)
state Ready initial:
  value: int = 0
transition DrawNow @ DrawEvent(size: int):
  <Ready, Ready> + Draw(size)
)DTESSL");
  } catch (const dtessl::Error& error) {
    nondeterministic_replay_error =
        std::string(error.what()).find(
            "nondeterministic do result requires replay=reinject") !=
        std::string::npos;
  }
  require(nondeterministic_replay_error,
          "nondeterministic do result admitted replay without reinjection");

  constexpr std::string_view composed_relation_transition_surface = R"DTESSL(
port audit.emit(int)

state A @ lane initial:
  value: int = 0
state B @ lane:
  value: int = 0
state C @ lane:
  value: int = 0

relation FromA @ lane:
  A

relation Zero @ lane:
  A [where=(value = 0)]

relation Reject @ lane:
  A [where=(value = 99)]

transition Chain:
  FromA, Zero -> C @ lane [do=(first: $audit.emit(1) @ lane, second: $audit.emit(2) @ lane)]:
    set @ lane:
      value = before.value + 2

transition Blocked:
  FromA, Reject -> C @ lane
)DTESSL";
  const dtessl::Program composed_relation_program =
      dtessl::parse(composed_relation_transition_surface);
  require(dtessl::required_features(composed_relation_program).contains(
              dtessl::LanguageFeature::StateRelationTemplates),
          "state relation templates are absent from backend negotiation");
  dtessl::Engine composed_relation_engine(composed_relation_program);
  const dtessl::StepResult composed_relation_result =
      composed_relation_engine.step_transition({"Chain", {}});
  require(composed_relation_result.round == 1U &&
              composed_relation_result.active_states.at("lane") == "C" &&
              composed_relation_result.state.at("value").as_int() == 2 &&
              composed_relation_result.transition == "Chain.FromA_and_Zero" &&
              composed_relation_result.actions.calls.size() == 2U &&
              composed_relation_result.actions.dependencies ==
                  std::vector<std::pair<std::size_t, std::size_t>>{{0U, 1U}},
          "named relation conjunction did not filter one transition construction");
  const auto composed_relation_plans =
      dtessl::search_plans(composed_relation_program);
  require(std::any_of(
              composed_relation_plans.begin(), composed_relation_plans.end(),
              [](const dtessl::SearchPlanSummary& plan) {
                return plan.operation == "transition-relation-match" &&
                       plan.max_rows == 2U && plan.max_work == 2U &&
                       plan.deterministic;
              }),
          "relation template matching did not expose a bounded Solver plan");
  dtessl::Engine blocked_composition_engine(composed_relation_program);
  bool blocked_composition_rejected = false;
  try {
    static_cast<void>(
        blocked_composition_engine.step_transition({"Blocked", {}}));
  } catch (const dtessl::Error&) {
    blocked_composition_rejected = true;
  }
  require(blocked_composition_rejected &&
              blocked_composition_engine.current_round() == 0U &&
              blocked_composition_engine.current_states().at("lane") == "A" &&
              blocked_composition_engine.values().at("value").as_int() == 0,
          "failed relation predicate changed transition state");

  constexpr std::string_view contextual_compact_relation_surface = R"DTESSL(
state Model @ model initial:
  values: set<int> = {1, 2}
  accepted: bool = false

state Noise @ noise initial:
  values: set<int> = {9}

relation Positive<x: int> @ model: x in values, x > 0;
relation <x: int> ~ PositiveTuple @ model: x in values, x > 0;

transition Check:
  <Model @ model, Model @ model>:
    set @ model:
      accepted = 1 ~ Positive and 1 ~ PositiveTuple
)DTESSL";
  dtessl::Engine contextual_relation_engine(
      dtessl::parse(contextual_compact_relation_surface));
  const dtessl::StepResult contextual_relation_result =
      contextual_relation_engine.step_transition({"Check", {}});
  require(contextual_relation_result.state.at("model.accepted").as_bool(),
          "name-first compact relation did not retain its @context scope");

  bool effectful_named_relation_rejected = false;
  try {
    static_cast<void>(dtessl::parse(R"DTESSL(
port audit.emit(int)
state A @ lane initial:
  value: int = 0
state B @ lane:
  value: int = 0
relation Impure:
  A @ lane [do=(emitted: $audit.emit(1) @ lane)]
)DTESSL"));
  } catch (const dtessl::Error&) {
    effectful_named_relation_rejected = true;
  }
  require(effectful_named_relation_rejected,
          "a pure named relation accepted ActionPlan ownership");

  bool named_relation_after_rejected = false;
  try {
    static_cast<void>(dtessl::parse(R"DTESSL(
state A @ lane initial:
  value: int = 0
state B @ lane:
  value: int = 0
relation OldBinary @ lane:
  <A, B>
)DTESSL"));
  } catch (const dtessl::Error&) {
    named_relation_after_rejected = true;
  }
  require(named_relation_after_rejected,
          "a named relation retained an after-Embedding");

  bool named_relation_set_rejected = false;
  try {
    static_cast<void>(dtessl::parse(R"DTESSL(
state A @ lane initial:
  value: int = 0
relation ImpureUpdate @ lane:
  A:
    set:
      value = 1
)DTESSL"));
  } catch (const dtessl::Error&) {
    named_relation_set_rejected = true;
  }
  require(named_relation_set_rejected,
          "a named relation retained a state update");

  bool unused_invalid_named_relation_rejected = false;
  try {
    static_cast<void>(dtessl::parse(R"DTESSL(
state A @ lane initial:
  value: int = 0
relation Invalid @ lane:
  Missing @ lane
)DTESSL"));
  } catch (const dtessl::Error&) {
    unused_invalid_named_relation_rejected = true;
  }
  require(unused_invalid_named_relation_rejected,
          "an unused named relation bypassed state verification");
  bool duplicate_relation_label_rejected = false;
  try {
    static_cast<void>(dtessl::parse(R"DTESSL(
state A @ lane initial:
  value: int = 0
state B @ lane:
  value: int = 0

transition Bad:
  <A @ lane, B @ lane> [label=same] | <A @ lane, B @ lane> [label=same]
)DTESSL"));
  } catch (const dtessl::Error&) {
    duplicate_relation_label_rejected = true;
  }
  require(duplicate_relation_label_rejected,
          "transition relation union accepted duplicate branch labels");

  bool duplicate_relation_action_rejected = false;
  try {
    static_cast<void>(dtessl::parse(R"DTESSL(
state A @ lane initial:
  value: int = 0
state B @ lane:
  value: int = 0

transition Bad:
  <A @ lane, B @ lane> [do=(inline: $audit.emit() @ lane)]:
    do:
      block: $audit.emit() @ lane
)DTESSL"));
  } catch (const dtessl::Error&) {
    duplicate_relation_action_rejected = true;
  }
  require(duplicate_relation_action_rejected,
          "transition relation branch accepted two ActionPlan lowerings");

  constexpr std::string_view optimized_transition = R"DTESSL(
function utility(value: int) -> int:
  value

state Idle @ scheduler initial:
  score: int = 0

transition Choose @ scheduler [optimized_score = utility(scheduler.score - before.scheduler.score)]():
  case low (Idle @ scheduler) -> (Idle @ scheduler):
    set:
      score = 2 @ scheduler
  case high (Idle @ scheduler) -> (Idle @ scheduler):
    set:
      score = 9 @ scheduler
)DTESSL";
  const dtessl::Program optimized_program = dtessl::parse(optimized_transition);
  require(dtessl::required_features(optimized_program).contains(
              dtessl::LanguageFeature::OptimizedTransition),
          "optimized transition feature discovery is missing");
  const auto optimized_plans = dtessl::search_plans(optimized_program);
  require(std::any_of(optimized_plans.begin(), optimized_plans.end(),
                      [](const dtessl::SearchPlanSummary& plan) {
                        return plan.operation == "optimized-transition-max" &&
                               plan.max_rows == 2U && plan.max_work == 2U &&
                               plan.deterministic && plan.rejects_ambiguous_score;
                      }),
          "optimized transition search plan lost its bound or tie policy");
  dtessl::Engine optimized_engine(optimized_program);
  const dtessl::StepResult optimized_result = optimized_engine.step_transition(
      dtessl::TransitionInput{"Choose", {}});
  require(optimized_result.transition == "Choose.high" &&
              optimized_result.state.at("score").as_int() == 9 &&
              optimized_result.optimization_scope == "scheduler" &&
              optimized_result.optimized_score.has_value() &&
              optimized_result.optimized_score->as_int() == 9,
          "optimized transition did not score candidate after-states or select the maximum");

  constexpr std::string_view tied_transition = R"DTESSL(
state Idle @ scheduler initial:
  score: int = 0

transition Choose() @ scheduler [optimized_score = scheduler.score]:
  case left (Idle @ scheduler) -> (Idle @ scheduler):
    set:
      score = 4 @ scheduler
  case right (Idle @ scheduler) -> (Idle @ scheduler):
    set:
      score = 4 @ scheduler
)DTESSL";
  dtessl::Engine tied_engine(dtessl::parse(tied_transition));
  bool tied_score_rejected = false;
  try {
    static_cast<void>(tied_engine.step_transition(
        dtessl::TransitionInput{"Choose", {}}));
  } catch (const dtessl::Error&) {
    tied_score_rejected = true;
  }
  require(tied_score_rejected && tied_engine.current_round() == 0U &&
              tied_engine.values().at("score").as_int() == 0,
          "equal optimized scores must reject without committing the round");

  constexpr std::string_view non_numeric_score = R"DTESSL(
state Idle @ scheduler initial:
  ready: bool = true

transition Bad() @ scheduler [optimized_score = scheduler.ready]:
  case stay (Idle @ scheduler) -> (Idle @ scheduler):
    where:
      true
)DTESSL";
  bool non_numeric_score_rejected = false;
  try {
    static_cast<void>(dtessl::parse(non_numeric_score));
  } catch (const dtessl::Error&) {
    non_numeric_score_rejected = true;
  }
  require(non_numeric_score_rejected,
          "optimized_score must reject non-exact-numeric expressions");

  const auto persistent_claims =
      dtessl::evaluate_named_trace(persistent_program, "Interleaved");
  require(persistent_claims.size() == 1U &&
              persistent_claims.front().status == dtessl::ClaimStatus::Satisfied,
          "procedure-qualified Claim did not observe persistent replay state");
  const dtessl::ProcedureTraceFrame& session_b_at_two =
      dtessl::captured_procedure_at(persistent_trace, "SessionB", 2U);
  require(session_b_at_two.state.at("value").as_int() == 2 &&
              session_b_at_two.procedure_revision == 1U &&
              session_b_at_two.transitions.empty(),
          "RoundId lookup did not retain an idle captured procedure frame");

  constexpr std::string_view missing_set_context = R"DTESSL(
state Idle @ local initial:
  value: int = 0

transition Bad @ Go():
  case (Idle @ local) -> (Idle @ local):
    set:
      value = 1
)DTESSL";
  bool missing_set_context_error = false;
  try {
    static_cast<void>(dtessl::parse(missing_set_context));
  } catch (const dtessl::Error&) {
    missing_set_context_error = true;
  }
  require(missing_set_context_error,
          "compact set accepted an assignment without @ context");

  constexpr std::string_view ambiguous_cases = R"DTESSL(
state Idle @ scheduler initial:
  value: int = 0

state Busy @ scheduler:
  value: int = 0

transition Ambiguous @ Go():
  case wildcard (_ @ scheduler) -> (Busy @ scheduler):
    set @ scheduler:
      value = 1
  case exact (Idle @ scheduler) -> (Busy @ scheduler):
    set @ scheduler:
      value = 2
)DTESSL";
  bool ambiguous_error = false;
  try {
    dtessl::Engine ambiguous(dtessl::parse(ambiguous_cases));
    static_cast<void>(ambiguous.step(dtessl::Event{"Go", {}}));
  } catch (const dtessl::Error&) {
    ambiguous_error = true;
  }
  require(ambiguous_error, "overlapping executable case paths were not rejected as ambiguous");

  constexpr std::string_view unknown_trace_path = R"DTESSL(
state Idle @ scheduler initial:
  value: int = 0

transition Move @ Go():
  case known (Idle @ scheduler) -> (Idle @ scheduler):
    set @ scheduler:
      value = 1

trace Bad @ system:
  capture projected:
    Move.missing
)DTESSL";
  bool unknown_path_error = false;
  try {
    static_cast<void>(dtessl::parse(unknown_trace_path));
  } catch (const dtessl::Error&) {
    unknown_path_error = true;
  }
  require(unknown_path_error, "trace accepted an unknown qualified case path");

  constexpr std::string_view impure_function = R"DTESSL(
function bad() -> int:
  round

state Idle initial:
  value: int = 0
)DTESSL";
  bool impure_function_error = false;
  try {
    static_cast<void>(dtessl::parse(impure_function));
  } catch (const dtessl::Error&) {
    impure_function_error = true;
  }
  require(impure_function_error, "pure function observed the automaton round");

  constexpr std::string_view relation_capturing_function = R"DTESSL(
function bad() -> relation int:
  Values

state Model initial:
  values: relation int = relation{1}

relation Values: relation int = values
)DTESSL";
  bool relation_capture_error = false;
  try {
    static_cast<void>(dtessl::parse(relation_capturing_function));
  } catch (const dtessl::Error&) {
    relation_capture_error = true;
  }
  require(relation_capture_error,
          "pure function captured an Embedding-dependent RelationPlan");

  constexpr std::string_view recursive_function = R"DTESSL(
function loop(value: int) -> int:
  loop(value)

state Idle initial:
  value: int = 0
)DTESSL";
  bool recursive_function_error = false;
  try {
    static_cast<void>(dtessl::parse(recursive_function));
  } catch (const dtessl::Error&) {
    recursive_function_error = true;
  }
  require(recursive_function_error, "recursive pure function cycle was accepted");

  constexpr std::string_view reversed_trace_sections = R"DTESSL(
state Idle initial:
  value: int = 0

transition Stay @ Go():
  from Idle
  to Idle:
    value = 1

trace Bad @ system:
  capture projected:
    Idle @ system
  replay:
    Stay() @ Entry
)DTESSL";
  bool trace_order_error = false;
  try {
    static_cast<void>(dtessl::parse(reversed_trace_sections));
  } catch (const dtessl::Error&) {
    trace_order_error = true;
  }
  require(trace_order_error, "trace accepted capture before replay");

  constexpr std::string_view empty_option_without_context = R"DTESSL(
state Broken initial:
  value: int = 0

transition Break @ Go():
  from Broken
  to Broken:
    value = []
)DTESSL";
  bool empty_option_type_error = false;
  try {
    static_cast<void>(dtessl::parse(empty_option_without_context));
  } catch (const dtessl::Error&) {
    empty_option_type_error = true;
  }
  require(empty_option_type_error, "[] was admitted without an expected [T] type");

  constexpr std::string_view invalid_relation = R"DTESSL(
state Broken initial:
  edges: relation<string, int> = relation{("a", 1)}
  projected: relation<string> = relation{}

transition Break @ Go():
  from Broken
  to Broken:
    projected = project(before.edges, 0, 0)
)DTESSL";
  bool relation_type_error = false;
  try {
    static_cast<void>(dtessl::parse(invalid_relation));
  } catch (const dtessl::Error&) {
    relation_type_error = true;
  }
  require(relation_type_error, "relation verifier accepted duplicate project columns");

  constexpr std::string_view compact_automaton = R"DTESSL(
state a, b, c;
trans ctodo: a -> b when b.val == 0;
trans ctodo: a -> c when b.val != 0;
procedure p1 a, b.val = 0, c & inject ctodo;
trace @procedure;
)DTESSL";
  const dtessl::Program compact_program = dtessl::parse(compact_automaton);
  const dtessl::TraceSnapshot compact_trace =
      dtessl::run_named_trace(compact_program, "p1");
  require(compact_trace.replayable && compact_trace.rounds.size() == 1U &&
              compact_trace.rounds.front().transitions.size() == 1U &&
              compact_trace.rounds.front().transitions.front().transition ==
                  "ctodo.a_to_b" &&
              compact_trace.procedure_states.at("p1").at("val").as_int() == 0,
          "compact AST did not lower to deterministic procedure replay semantics");
  const dtessl::LanguageAnalysis compact_language =
      dtessl::analyze_source(compact_automaton, "test://compact", 1);
  require(compact_language.valid() &&
              std::any_of(compact_language.highlights.begin(),
                          compact_language.highlights.end(), [&](const auto& token) {
                            return token.syntax == dtessl::SyntaxClass::Keyword &&
                                   compact_automaton.substr(
                                       token.range.start.offset,
                                       token.range.end.offset - token.range.start.offset) ==
                                       "trans";
                          }),
          "compact syntax did not reuse the production language service");

  constexpr std::string_view multiline_compact = R"DTESSL(
state a,
  b;
)DTESSL";
  bool multiline_compact_rejected = false;
  try {
    static_cast<void>(dtessl::parse(multiline_compact));
  } catch (const dtessl::Error&) {
    multiline_compact_rejected = true;
  }
  require(multiline_compact_rejected,
          "compact syntax accepted a declaration split across physical lines");

  constexpr std::string_view compact_inject_without_transition = R"DTESSL(
state idle, done;
trans idle -> done;
procedure broken idle & inject go;
)DTESSL";
  bool empty_compact_transition_rejected = false;
  try {
    static_cast<void>(dtessl::parse(compact_inject_without_transition));
  } catch (const dtessl::Error&) {
    empty_compact_transition_rejected = true;
  }
  require(empty_compact_transition_rejected,
          "compact injection resolved an undefined transition name");

  const dtessl::Embedding embedding{
      {{"scheduler", "Ready"}},
      {{"scheduler.credits", dtessl::Value(std::int64_t{2})}}};
  const auto encoded_embedding = dtessl::encode_embedding(embedding);
  require(dtessl::decode_embedding(encoded_embedding) == embedding,
          "canonical Embedding codec did not round-trip");
  dtessl::EmbeddingStore embedding_store;
  const auto embedding_root = embedding_store.insert(embedding);
  const auto embedding_duplicate = embedding_store.insert(embedding);
  require(embedding_root.inserted && !embedding_duplicate.inserted &&
              embedding_root.index == embedding_duplicate.index &&
              embedding_store.path_to(embedding_root.index).size() == 1U,
          "EmbeddingStore did not deduplicate an exact Embedding");
  dtessl::Embedding child_embedding = embedding;
  child_embedding.values.insert_or_assign("scheduler.credits",
                                          dtessl::Value(std::int64_t{3}));
  const auto embedding_child = embedding_store.insert(
      child_embedding, embedding_root.index, "RaiseCredits");
  require(embedding_child.inserted &&
              !embedding_store.at(embedding_child.index).witness_delta.empty() &&
              dtessl::apply_embedding_delta(
                  embedding_store.at(embedding_root.index).embedding,
                  embedding_store.at(embedding_child.index).witness_delta) ==
                  child_embedding,
          "EmbeddingStore did not derive a reconstructable witness delta");

  constexpr std::string_view recursive_embedding_source = R"DTESSL(
name WorkerId

state Scheduler @ session initial:
  Phase(Idle | Running(Stage(Reserving | Committing), attempts: int = 0, permitted: relation WorkerId = {WorkerId(a)}, invariant(WorkerId(a) ~ permitted)))
  Health(Healthy | Degraded)
  credits: int = 2
  workers: relation WorkerId = {WorkerId(a), WorkerId(b)}
  primary: relation WorkerId = {WorkerId(a)}
  fallback: relation WorkerId = {WorkerId(b)}
  invariant:
    WorkerId(a) ~ workers and (WorkerId(a) ~ primary or WorkerId(a) ~ fallback)

transition Start():
  case reserve (Scheduler(Phase.Idle, Health.Healthy) @ session) -> (Scheduler(Phase.Running.Stage.Reserving) @ session):
    set @ session:
      credits = before.session.credits - 1

transition Commit():
  case commit (Scheduler(Phase.Running.Stage.Reserving) @ session) -> (Scheduler(Phase.Running.Stage.Committing) @ session):
    where:
      <WorkerId(a), WorkerId(b)> ~ before.session.links
      and (<WorkerId(a), WorkerId(b)> ~ before.session.links or
           <WorkerId(a), WorkerId(b)> ~ before.session.backup)
      and <<WorkerId(a), WorkerId(b)>, WorkerId(a)> ~ before.session.nested
      and (<<WorkerId(a), WorkerId(b)>, WorkerId(a)> ~ before.session.nested or
           <<WorkerId(a), WorkerId(b)>, WorkerId(a)> ~ before.session.nested_backup)
)DTESSL";
  // Add binary relations separately so the recursive relation-expression also
  // checks tuple subjects and nested AND/OR.
  std::string recursive_source(recursive_embedding_source);
  const std::string relation_fields =
      "  links: relation (WorkerId, WorkerId) = {<WorkerId(a), WorkerId(b)>}\n"
      "  backup: relation (WorkerId, WorkerId) = {}\n"
      "  nested: relation (tuple<WorkerId, WorkerId>, WorkerId) = "
      "{<<WorkerId(a), WorkerId(b)>, WorkerId(a)>}\n"
      "  nested_backup: relation (tuple<WorkerId, WorkerId>, WorkerId) = {}\n";
  recursive_source.insert(recursive_source.find("  invariant:"), relation_fields);
  const dtessl::Program recursive_program = dtessl::parse(recursive_source);
  const dtessl::FeatureSet recursive_features =
      dtessl::required_features(recursive_program);
  require(recursive_features.contains(dtessl::LanguageFeature::RecursiveStateSchema) &&
              recursive_features.contains(dtessl::LanguageFeature::RelationExpression),
          "recursive state/relation features were not preserved in the typed AST");
  const dtessl::Solver recursive_solver(recursive_program);
  const dtessl::RawKeyMap& raw_keys = recursive_solver.raw_key_map();
  require(raw_keys.offset_of(dtessl::RawSlotKind::Control,
                             "session::Scheduler.Phase") &&
              raw_keys.offset_of(dtessl::RawSlotKind::Control,
                                 "session::Scheduler.Phase.Running.Stage") &&
              raw_keys.offset_of(dtessl::RawSlotKind::Value,
                                 "session.credits"),
          "RawKeyMap did not map recursive semantic paths to raw vector offsets");
  dtessl::Engine recursive_engine(recursive_program);
  static_cast<void>(recursive_engine.step_transition({"Start", {}}));
  require(recursive_engine.current_states().at("session::Scheduler.Phase") ==
              "Scheduler.Phase.Running" &&
              recursive_engine.current_states().at(
                  "session::Scheduler.Phase.Running.Stage") ==
                  "Scheduler.Phase.Running.Stage.Reserving",
          "recursive structural transition did not rewrite the nested embedding");
  static_cast<void>(recursive_engine.step_transition({"Commit", {}}));
  require(recursive_engine.current_states().at(
              "session::Scheduler.Phase.Running.Stage") ==
              "Scheduler.Phase.Running.Stage.Committing",
          "recursive relation guard or descendant rewrite failed");

  constexpr std::string_view compact_recursive_source = R"DTESSL(
state Switch @ local = Mode(Off | On(Level(Low | High), stable: bool = true, invariant(stable))), Health(Good | Failed), changes: int = 0;

transition TurnOn():
  case (Switch(Mode.Off, Health.Good) @ local) -> (Switch(Mode.On.Level.Low) @ local):
    set @ local:
      changes = before.local.changes + 1
)DTESSL";
  const dtessl::Program compact_recursive_program =
      dtessl::parse(compact_recursive_source);
  dtessl::Engine compact_recursive_engine(compact_recursive_program);
  const dtessl::StepResult compact_recursive_step =
      compact_recursive_engine.step_transition({"TurnOn", {}});
  require(compact_recursive_step.state.at("local.changes").as_int() == 1,
          "compact recursive state did not lower through the formal runtime");

  constexpr std::string_view case_axis_source = R"DTESSL(
name WorkerId

state CaseScheduler @ session initial:
  retries: int32[0:1:2] = 0
  case Phase:
    Idle
    | Running(worker: WorkerId{WorkerId(a), WorkerId(b)} = WorkerId(a), case Step: Prepare | Execute)
    | Done

transition Start():
  case begin (CaseScheduler(Phase.Idle) @ session) -> (CaseScheduler(Phase.Running.Step.Prepare) @ session):
    set @ session:
      retries = before.session.retries + 1

transition Advance():
  case execute (CaseScheduler(Phase.Running.Step.Prepare) @ session) -> (CaseScheduler(Phase.Running.Step.Execute) @ session):
    where:
      true

transition SetRetry(next: int32[0:1:2]):
  case update (CaseScheduler @ session) -> (CaseScheduler @ session):
    set @ session:
      retries = next

procedure CaseScheduling @ session:
  initial (CaseScheduler @ session)

Claim RetryBound @ procedure CaseScheduling @ (session):
  always(session.retries <= 2)
)DTESSL";
  const dtessl::Program case_axis_program = dtessl::parse(case_axis_source);
  require(dtessl::required_features(case_axis_program).contains(
              dtessl::LanguageFeature::FiniteDomains),
          "finite domain feature was not negotiated with backends");
  dtessl::Engine case_axis_engine(case_axis_program);
  const dtessl::StepResult case_axis_step =
      case_axis_engine.step_transition({"Start", {}});
  const dtessl::Embedding case_axis_before{
      case_axis_step.before_active_states, case_axis_step.before_state};
  const dtessl::Embedding case_axis_after{
      case_axis_step.active_states, case_axis_step.state};
  require(case_axis_engine.current_states().at(
              "session::CaseScheduler.Phase") ==
              "CaseScheduler.Phase.Running" &&
              case_axis_engine.current_states().at(
                  "session::CaseScheduler.Phase.Running.Step") ==
                  "CaseScheduler.Phase.Running.Step.Prepare",
          "state case did not lower to explicit nested choice axes");
  require(!case_axis_step.delta.empty() &&
              dtessl::apply_embedding_delta(case_axis_before,
                                            case_axis_step.delta) ==
                  case_axis_after &&
              case_axis_step.before_embedding_digest ==
                  dtessl::embedding_digest(case_axis_before) &&
              case_axis_step.embedding_digest ==
                  dtessl::embedding_digest(case_axis_after),
          "runtime trace delta did not reconstruct and authenticate its embedding");
  const dtessl::StepResult nested_axis_step =
      case_axis_engine.step_transition({"Advance", {}});
  require(nested_axis_step.delta.controls.size() == 1U &&
              nested_axis_step.delta.controls.front().path ==
                  "session::CaseScheduler.Phase.Running.Step",
          "nested case-axis rewrite changed more than its explicit axis");
  const dtessl::ClaimSolveResult finite_solver_result =
      dtessl::Solver(case_axis_program).verify_claim("RetryBound");
  require(finite_solver_result.status != dtessl::ClaimSolveStatus::Inconclusive,
          "finite transition domain was not exhaustively generated by Solver");

  constexpr std::string_view compact_case_axis_source = R"DTESSL(
state CompactCase @ local = changes: int8[0:3] = 0, case Mode: Off | On(case Level: Low | High);
)DTESSL";
  static_cast<void>(dtessl::parse(compact_case_axis_source));

  constexpr std::string_view product_case_source = R"DTESSL(
state ProductCase @ local initial:
  case Shape:
    (A, B)
    | C

transition Fold():
  case both (ProductCase(Shape.A, Shape.B) @ local) -> (ProductCase(Shape.C) @ local):
    where:
      true
)DTESSL";
  dtessl::Engine product_case_engine(dtessl::parse(product_case_source));
  const dtessl::StepResult product_case_step =
      product_case_engine.step_transition({"Fold", {}});
  require(product_case_step.delta.controls.size() == 1U &&
              product_case_step.delta.controls.front().path ==
                  "local::ProductCase.Shape" &&
              product_case_engine.current_states().at(
                  "local::ProductCase.Shape") == "ProductCase.Shape.C",
          "parenthesized product branch did not match as one case alternative");

  constexpr std::string_view invalid_finite_initial = R"DTESSL(
state InvalidFinite initial:
  value: int32[0:2:4] = 3
)DTESSL";
  bool invalid_finite_initial_rejected = false;
  try {
    static_cast<void>(dtessl::parse(invalid_finite_initial));
  } catch (const dtessl::Error&) {
    invalid_finite_initial_rejected = true;
  }
  require(invalid_finite_initial_rejected,
          "state value outside its finite static constraint was accepted");

  constexpr std::string_view duplicate_recursive_state = R"DTESSL(
state Invalid initial:
  Phase(Idle | Idle)
)DTESSL";
  bool duplicate_recursive_rejected = false;
  try {
    static_cast<void>(dtessl::parse(duplicate_recursive_state));
  } catch (const dtessl::Error&) {
    duplicate_recursive_rejected = true;
  }
  require(duplicate_recursive_rejected,
          "recursive schema verification accepted duplicate alternatives");

  constexpr std::string_view false_recursive_relation = R"DTESSL(
name Item
state Invalid initial:
  present: relation Item = {Item(a)}
  absent: relation Item = {}
  invariant:
    Item(a) ~ present and Item(a) ~ absent
)DTESSL";
  bool recursive_relation_rejected = false;
  try {
    static_cast<void>(dtessl::parse(false_recursive_relation));
  } catch (const dtessl::Error&) {
    recursive_relation_rejected = true;
  }
  require(recursive_relation_rejected,
          "recursive relation conjunction did not reject a false invariant");

  std::string excessive_relation_depth =
      "name Item\nstate Deep initial:\n  members: relation Item = {Item(a)}\n"
      "  invariant:\n    Item(a) ~ ";
  excessive_relation_depth.append(70U, '(');
  excessive_relation_depth += "members";
  excessive_relation_depth.append(70U, ')');
  excessive_relation_depth += "\n";
  bool recursive_depth_rejected = false;
  try {
    static_cast<void>(dtessl::parse(excessive_relation_depth));
  } catch (const dtessl::Error&) {
    recursive_depth_rejected = true;
  }
  require(recursive_depth_rejected,
          "recursive relation parser accepted an expression beyond its depth limit");

  constexpr std::string_view ancestor_guard_source = R"DTESSL(
state Hierarchy initial:
  Phase(Idle | Running(Stage(A | B)))

transition Sneak():
  case (Hierarchy.Phase.Running.Stage.A) -> (Hierarchy.Phase.Running.Stage.B):
    where:
      true
)DTESSL";
  dtessl::Engine ancestor_guard(dtessl::parse(ancestor_guard_source));
  bool inactive_descendant_rejected = false;
  try {
    static_cast<void>(ancestor_guard.step_transition({"Sneak", {}}));
  } catch (const dtessl::Error&) {
    inactive_descendant_rejected = true;
  }
  require(inactive_descendant_rejected,
          "recursive matching ignored an inactive ancestor state");

  constexpr std::string_view round_dependent_claim = R"DTESSL(
state Idle initial:
  value: int = 0

transition Stay():
  case same (Idle) -> (Idle):
    where:
      true

trace Model:
  capture closed:
    transition (Stay.same)

Claim Later @ Model:
  eventually:
    round > 2
)DTESSL";
  const auto round_result =
      dtessl::Solver(dtessl::parse(round_dependent_claim)).verify_claim("Later");
  require(round_result.status == dtessl::ClaimSolveStatus::Inconclusive,
          "Solver treated an unmodeled logical round as Embedding state");

  constexpr std::string_view temporal_procedure = R"DTESSL(
state Idle @ scheduler initial:
  credits: int = 1
  done: bool = false

state Running @ scheduler:
  credits: int = 1
  done: bool = true

state Cold @ process initial:
  started: bool = false

state Started @ process:
  started: bool = true

procedure Work @ system:
  initial (Idle @ scheduler, Cold @ process)
  (Idle @ scheduler, Cold @ process) -> (Running @ scheduler, Started @ process):
    set @ scheduler:
      done = true
    set @ process:
      started = true
    ensure:
      within(1, process.started)
  (Running @ scheduler, Started @ process) -> (Running @ scheduler, Started @ process):
    where:
      true

Claim Safe @ procedure Work @ (scheduler, process):
  always(scheduler.credits >= 0)

Claim Starts @ procedure Work @ (scheduler, process):
  eventually(process.started)

Claim HoldsUntilStart @ procedure Work:
  until(not process.started, process.started)

Claim StartsPromptly @ procedure Work:
  within(1, process.started)

Claim StartedSinceDone @ procedure Work:
  eventually(since(process.started, scheduler.done))

Claim NeverNegative @ procedure Work:
  never(scheduler.credits < 0)

Claim StartBeforeDone @ procedure Work:
  <not process.started, scheduler.done> ~ happens_before

Claim LegacyBefore @ procedure Work:
  before(not process.started, scheduler.done)

Claim NestedTraceRelation @ procedure Work:
  eventually(<not process.started, scheduler.done> ~ happens_before)

Claim WaitWeakly @ procedure Work:
  weak_until(not process.started, process.started)

Claim ImpossibleWithinOne @ procedure Work:
  within(1, scheduler.credits = 99)

Claim ImpossibleUntil @ procedure Work:
  until(scheduler.credits >= 0, scheduler.credits = 99)

Claim InvalidHistory @ procedure Work:
  always(since(process.started, scheduler.done))

Claim SimultaneousIsNotBefore @ procedure Work:
  before(process.started, scheduler.done)

Claim RunningDone @ state Running @ (scheduler):
  always(scheduler.done)
)DTESSL";
  const dtessl::Program temporal_program = dtessl::parse(temporal_procedure);
  for (const std::string_view claim :
       {"Safe", "Starts", "HoldsUntilStart", "StartsPromptly",
        "StartedSinceDone", "NeverNegative", "StartBeforeDone", "LegacyBefore",
        "WaitWeakly"}) {
    const dtessl::ClaimSolveResult solved =
        dtessl::Solver(temporal_program).verify_claim(claim);
    require(solved.status == dtessl::ClaimSolveStatus::Verified &&
                solved.explored_product_states != 0U &&
                solved.claim_monitor_states != 0U &&
                solved.executable_embedding_snapshots ==
                    solved.explored_embeddings,
            std::string("temporal Claim did not verify: ") + std::string(claim));
  }
  const dtessl::ClaimSolveResult history_product =
      dtessl::Solver(temporal_program).verify_claim("StartedSinceDone");
  require(history_product.explored_product_states >
              history_product.explored_embeddings &&
              history_product.executable_embedding_snapshots ==
                  history_product.explored_embeddings,
          "Product history duplicated an executable Embedding snapshot");
  const dtessl::ClaimSolveResult state_claim =
      dtessl::Solver(temporal_program).verify_claim("RunningDone");
  require(state_claim.status == dtessl::ClaimSolveStatus::Verified,
          "typed @ state Claim did not evaluate its selected context");
  const dtessl::ClaimSolveResult nested_trace_relation =
      dtessl::Solver(temporal_program).verify_claim("NestedTraceRelation");
  require(nested_trace_relation.status == dtessl::ClaimSolveStatus::Inconclusive,
          "unsupported future-under-future trace relation was proved optimistically");
  for (const std::string_view claim :
       {"ImpossibleWithinOne", "ImpossibleUntil", "InvalidHistory",
        "SimultaneousIsNotBefore"}) {
    const dtessl::ClaimSolveResult solved =
        dtessl::Solver(temporal_program).verify_claim(claim);
    require(solved.status == dtessl::ClaimSolveStatus::Counterexample &&
                !solved.counterexample.empty(),
            std::string("temporal counterexample was not found: ") +
                std::string(claim));
  }

  constexpr std::string_view failing_transition_obligation = R"DTESSL(
state Idle initial:
  done: bool = false

state Waiting:
  done: bool = false

transition Begin():
  case start (Idle) -> (Waiting):
    ensure:
      eventually(done)
  case wait (Waiting) -> (Waiting):
    where:
      true

procedure Run @ local:
  initial (Idle)

trace Model:
  replay:
    inject Begin() @ Run -> Begin.start
    inject Begin() @ Run -> Begin.wait
  capture closed:
    transition (Begin.start, Begin.wait)

Claim ModelIsTyped @ Model:
  always:
    true
)DTESSL";
  const dtessl::ClaimSolveResult obligation_result = dtessl::Solver(
      dtessl::parse(failing_transition_obligation)).verify_claim("ModelIsTyped");
  require(obligation_result.status == dtessl::ClaimSolveStatus::Counterexample &&
              !obligation_result.counterexample.empty(),
          "transition temporal obligation did not participate in the Product automaton");
  const auto obligation_trace = dtessl::evaluate_named_trace(
      dtessl::parse(failing_transition_obligation), "Model");
  require(std::any_of(obligation_trace.begin(), obligation_trace.end(),
                      [](const dtessl::ClaimEvaluation& evaluation) {
                        return evaluation.name == "Begin.start.ensure" &&
                               evaluation.status == dtessl::ClaimStatus::Violated;
                      }),
          "finite Trace monitor did not evaluate a transition ensure obligation");

  constexpr std::string_view distributed_capture_extensions = R"DTESSL(
state Idle @ lane initial:
  value: int = 0

state Active @ lane [capture=Audit/default | StateOnly/default]:
  value: int = 1

state Done @ lane:
  value: int = 2

state Never @ lane [capture=Missing/default]:
  value: int = 3

transition Enter() @ lane [capture=Transitions/sessionB]:
  case go (Idle @ lane) -> (Active @ lane):
    set:
      value = 1 @ lane

transition Leave():
  case go (Active @ lane) -> (Done @ lane):
    set:
      value = 2 @ lane

procedure Work @ system [capture=Procedure/sessionC]:
  initial (Idle @ lane)

trace Audit:
  capture closed:
    state()

trace Procedure:
  capture closed:
    procedure()

trace Missing:
  capture closed:
    state()

trace EnterOnly:
  capture closed:
    transition(Enter)

trace LeaveOnly:
  capture closed:
    transition(Leave)
)DTESSL";
  const dtessl::Program capture_program =
      dtessl::parse(distributed_capture_extensions);
  const std::vector<std::string> capture_traces =
      dtessl::declared_traces(capture_program);
  require(std::find(capture_traces.begin(), capture_traces.end(), "Audit") !=
                  capture_traces.end() &&
              std::find(capture_traces.begin(), capture_traces.end(), "StateOnly") !=
                  capture_traces.end() &&
              std::find(capture_traces.begin(), capture_traces.end(),
                        "Transitions/sessionB") != capture_traces.end() &&
              std::find(capture_traces.begin(), capture_traces.end(),
                        "Procedure/sessionC") != capture_traces.end(),
          "distributed capture extensions did not aggregate by Trace/Session");

  dtessl::Engine capture_engine =
      dtessl::Engine::from_procedure(capture_program, "Work");
  static_cast<void>(capture_engine.step_transition({"Enter", {}}));
  static_cast<void>(capture_engine.step_transition({"Leave", {}}));
  const dtessl::TraceSnapshot state_capture =
      capture_engine.captured_trace("StateOnly", true);
  require(state_capture.mode == dtessl::TraceCaptureMode::Projected &&
              state_capture.rounds.size() == 2U &&
              state_capture.captured_states.contains({"lane", "Active"}) &&
              state_capture.rounds.front().transitions.front().transition ==
                  "Enter.go" &&
              state_capture.rounds.back().transitions.front().transition ==
                  "Leave.go",
          "state capture relation did not observe both entry and exit");
  const dtessl::TraceSnapshot transition_capture =
      capture_engine.captured_trace("Transitions/sessionB", true);
  require(transition_capture.mode == dtessl::TraceCaptureMode::Projected &&
              transition_capture.rounds.size() == 1U &&
              transition_capture.captured_paths.contains("Enter") &&
              transition_capture.rounds.front().transitions.front().transition ==
                  "Enter.go" &&
              !transition_capture.replayable,
          "transition-family capture relation did not select named cases");

  dtessl::RuntimeContext capture_runtime(capture_program);
  capture_runtime.start("Work");
  static_cast<void>(capture_runtime.inject({{"Work", {"Enter", {}}}}));
  static_cast<void>(capture_runtime.inject({{"Work", {"Leave", {}}}}));
  const dtessl::TraceSnapshot audit_capture = capture_runtime.snapshot("Audit");
  require(audit_capture.mode == dtessl::TraceCaptureMode::Closed &&
              audit_capture.rounds.size() == 2U && audit_capture.replayable &&
              audit_capture.procedure_artifacts.at("Work").injections.size() == 2U,
          "closed state seed did not retain its selected occurrence evidence");
  const dtessl::TraceSnapshot procedure_capture =
      capture_runtime.snapshot("Procedure/sessionC");
  require(procedure_capture.mode == dtessl::TraceCaptureMode::Closed &&
              procedure_capture.rounds.size() == 2U &&
              procedure_capture.captured_procedures.contains("Work") &&
              procedure_capture.procedure_history.at("Work").size() == 2U &&
              procedure_capture.replayable,
          "procedure capture extension did not retain a replayable closure");
  const dtessl::TraceSnapshot missing_capture =
      capture_runtime.snapshot("Missing");
  require(missing_capture.mode == dtessl::TraceCaptureMode::Closed &&
              missing_capture.rounds.empty() &&
              missing_capture.captured_procedures.empty() &&
              missing_capture.procedure_artifacts.empty() &&
              !missing_capture.replayable,
          "a closed capture with no relation match leaked unrelated procedures");
  const dtessl::TraceSnapshot enter_only_capture =
      capture_runtime.snapshot("EnterOnly");
  require(enter_only_capture.rounds.size() == 1U &&
              enter_only_capture.rounds.front().round == 1U &&
              enter_only_capture.rounds.front().transitions.front().transition ==
                  "Enter.go" &&
              enter_only_capture.final_state.at("value").as_int() == 1 &&
              enter_only_capture.procedure_artifacts.at("Work").injections.size() == 1U &&
              enter_only_capture.procedure_history.at("Work").size() == 1U &&
              enter_only_capture.trace_artifact.has_value() &&
              enter_only_capture.replayable &&
              dtessl::replay_trace_artifact(
                  capture_program, *enter_only_capture.trace_artifact).rounds ==
                  enter_only_capture.rounds,
          "closed transition capture incorrectly expanded to a whole procedure");
  const dtessl::TraceSnapshot leave_only_capture =
      capture_runtime.snapshot("LeaveOnly");
  require(leave_only_capture.rounds.size() == 2U &&
              leave_only_capture.rounds.front().transitions.front().transition ==
                  "Enter.go" &&
              leave_only_capture.rounds.back().transitions.front().transition ==
                  "Leave.go" &&
              leave_only_capture.trace_artifact.has_value() &&
              leave_only_capture.trace_artifact->rounds.size() == 2U,
          "closed capture did not follow the selected occurrence's predecessor IDs");

  constexpr std::string_view free_temporal_capture = R"DTESSL(
state Idle @ flow initial:
  value: int = 0

state Waiting @ flow:
  value: int = 0

state Done @ flow:
  value: int = 0

transition Start():
  case go (Idle @ flow) -> (Waiting @ flow):
    set:
      value = 1 @ flow

transition Progress():
  case stay (Waiting @ flow) -> (Waiting @ flow):
    set:
      value = before.value + 1 @ flow

transition Finish():
  case done (Waiting @ flow) -> (Done @ flow):
    set:
      value = before.value + 1 @ flow

transition After():
  case stay (Done @ flow) -> (Done @ flow):
    set:
      value = before.value + 1 @ flow

trace FreeTemporal:
  capture closed:
    transition(Start)
    eventually state(Done @ flow)

trace FreeTemporalPath:
  capture closed:
    transition(Start)
    eventually transition(Finish.done)
)DTESSL";
  const dtessl::Program free_temporal_program =
      dtessl::parse(free_temporal_capture);
  require(dtessl::required_features(free_temporal_program).contains(
              dtessl::LanguageFeature::TemporalLogic),
          "temporal capture did not negotiate its semantic feature");
  dtessl::Engine free_temporal_engine(free_temporal_program);
  static_cast<void>(free_temporal_engine.step_transition({"Start", {}}));
  static_cast<void>(free_temporal_engine.step_transition({"Progress", {}}));
  const dtessl::TraceSnapshot open_temporal =
      free_temporal_engine.captured_trace("FreeTemporal", false);
  require(open_temporal.rounds.size() == 2U &&
              open_temporal.temporal_intervals.size() == 1U &&
              open_temporal.temporal_intervals.front().status ==
                  dtessl::CaptureIntervalStatus::Pending,
          "open eventually capture did not retain its pending suffix");
  const dtessl::TraceSnapshot unresolved_temporal =
      free_temporal_engine.captured_trace("FreeTemporal", true);
  require(unresolved_temporal.temporal_intervals.front().status ==
              dtessl::CaptureIntervalStatus::Unresolved,
          "closing an unwitnessed eventually interval did not mark the gap");
  static_cast<void>(free_temporal_engine.step_transition({"Finish", {}}));
  static_cast<void>(free_temporal_engine.step_transition({"After", {}}));
  const dtessl::TraceSnapshot witnessed_temporal =
      free_temporal_engine.captured_trace("FreeTemporal", true);
  require(witnessed_temporal.rounds.size() == 3U &&
              witnessed_temporal.rounds.back().round == 3U &&
              witnessed_temporal.rounds.back().transitions.front().transition ==
                  "Finish.done" &&
              witnessed_temporal.final_state.at("value").as_int() == 3 &&
              witnessed_temporal.captured_procedures.empty() &&
              witnessed_temporal.temporal_intervals.front().status ==
                  dtessl::CaptureIntervalStatus::Witnessed &&
              witnessed_temporal.temporal_intervals.front().witness_round == 3U &&
              dtessl::captured_round_at(witnessed_temporal, 3U).round == 3U &&
              witnessed_temporal.trace_artifact.has_value() &&
              witnessed_temporal.replayable &&
              dtessl::replay_trace_artifact(
                  free_temporal_program,
                  *witnessed_temporal.trace_artifact).rounds ==
                  witnessed_temporal.rounds,
          "free transition eventually closure did not stop at its witness round");
  const dtessl::TraceSnapshot path_witnessed_temporal =
      free_temporal_engine.captured_trace("FreeTemporalPath", true);
  require(path_witnessed_temporal.rounds == witnessed_temporal.rounds &&
              path_witnessed_temporal.temporal_intervals.size() == 1U &&
              path_witnessed_temporal.temporal_intervals.front().status ==
                  dtessl::CaptureIntervalStatus::Witnessed &&
              path_witnessed_temporal.temporal_intervals.front().witness_occurrence ==
                  witnessed_temporal.rounds.back().transitions.front().id,
          "eventually transition target did not use the occurrence identity");
  dtessl::TraceArtifact tampered_artifact =
      *witnessed_temporal.trace_artifact;
  tampered_artifact.rounds.front().expected.transitions.front().transition =
      "Start.tampered";
  bool tampered_decision_rejected = false;
  try {
    static_cast<void>(dtessl::replay_trace_artifact(
        free_temporal_program, tampered_artifact));
  } catch (const dtessl::Error&) {
    tampered_decision_rejected = true;
  }
  require(tampered_decision_rejected,
          "trace artifact replay did not compare the complete logical result");

  bool seedless_temporal_rejected = false;
  try {
    static_cast<void>(dtessl::parse(R"DTESSL(
state Idle initial:
  value: int = 0

trace Invalid:
  capture closed:
    eventually state(Idle)
)DTESSL"));
  } catch (const dtessl::Error&) {
    seedless_temporal_rejected = true;
  }
  require(seedless_temporal_rejected,
          "eventually capture without an anchor seed was accepted");
  return 0;
}
