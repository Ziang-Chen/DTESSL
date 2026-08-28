#include "dtessl/dtessl.hpp"

#include <cstdlib>
#include <iostream>
#include <string>

namespace {

constexpr std::string_view source = R"DTESSL(
state Scheduler @ local initial:
  mode: string = "Idle"
  credits: int = 2
  workers: set<string> = {"worker-a", "worker-b"}
  busy: set<string> = {}
  last_event_at: int = 0
  invariant:
    credits >= 0 and count(workers) = 2 and count(busy) <= count(workers)

transition Schedule @ Submit(task: string, worker: string):
  from Scheduler
  to Scheduler:
    mode = "Waiting"
    credits = before.credits - 1
    busy = insert(before.busy, worker)
    last_event_at = now
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
    last_event_at = now
  where:
    before.mode = "Waiting"
  do:
    completed: $ipc.complete(task)
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
  const dtessl::Program program = dtessl::parse(source);
  dtessl::Event submit{"Submit", {{"task", dtessl::Value("task-1")},
                                    {"worker", dtessl::Value("worker-a")}}};

  dtessl::Engine engine(program);
  const dtessl::StepResult first = engine.step(submit);
  require(first.tick == 1, "the first accepted event must advance logical time to one");
  require(first.transition == "Schedule", "wrong transition selected");
  require(first.state.at("mode").as_string() == "Waiting", "state update was not committed");
  require(first.state.at("credits").as_int() == 1, "integer update is wrong");
  require(first.state.at("busy").as_string_set().values.contains("worker-a"),
          "set resource update is wrong");
  require(first.state.at("last_event_at").as_int() == 0, "now must expose the pre-step tick");
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
  require(second.tick == 2 && second.state.at("mode").as_string() == "Idle",
          "second event did not evolve the state");
  require(second.state.at("busy").as_string_set().values.empty(),
          "set resource release is wrong");
  require(second.state.at("last_event_at").as_int() == 1, "logical time did not advance");

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
