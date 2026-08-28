#include "dtessl/dtessl.hpp"
#include "dtessl/backend.hpp"
#include "dtessl/scratch_pool.hpp"
#include "dtessl/value_codec.hpp"
#include "dtessl/version.hpp"

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
)DTESSL";

[[noreturn]] void fail(const std::string& message) {
  std::cerr << "dtessl test failed: " << message << '\n';
  std::exit(1);
}

void require(bool condition, const std::string& message) {
  if (!condition) fail(message);
}

}  // namespace

int main() {
  require(dtessl::version == "0.1.2", "compiled version must be v0.1.2");
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
  return 0;
}
