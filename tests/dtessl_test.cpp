#include "dtessl/dtessl.hpp"
#include "dtessl/scratch_pool.hpp"
#include "dtessl/version.hpp"

#include <cstdlib>
#include <iostream>
#include <set>
#include <string>

namespace {

constexpr std::string_view source = R"DTESSL(
state Scheduler @ local initial:
  mode: string = "Idle"
  credits: int = 2
  workers: set<string> = {"worker-a", "worker-b"}
  busy: set<string> = {}
  last_round: int = 0
  note: string = ""
  tags: set<string> = {} merge union
  tag_count: int = 0
  invariant:
    credits >= 0 and count(workers) = 2 and count(busy) <= count(workers)

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
  require(dtessl::version == "0.0.2", "compiled version must be v0.0.2");
  const dtessl::Program program = dtessl::parse(source);
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
  return 0;
}
