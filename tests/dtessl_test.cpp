#include "dtessl/dtessl.hpp"
#include "dtessl/backend.hpp"
#include "dtessl/scratch_pool.hpp"
#include "dtessl/semantic_descriptor.hpp"
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
                      "owned by chenRT runtime journal and excluded from this DTESSL model"}};
  return descriptor;
}

}  // namespace

int main() {
  require(dtessl::version == "0.2.1", "compiled version must be v0.2.1");
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
      {"chen", "vm", 1},
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
  return 0;
}
