// Execution half of the implementation. It is included once by frontend.cpp
// so the private typed AST remains hidden without creating a second public IR.
// All Engine/RuntimeContext round, dispatch, replay and capture behavior lives
// here; parsing and verification remain in frontend.cpp.

Engine::Engine(Program program) : Engine(std::move(program), {}) {}

Engine::Engine(Program program,
               std::map<std::string, std::string, std::less<>> initial_states)
    : program_(std::move(program)) {
  if (program_.empty()) throw Error("cannot construct an engine from an empty program");
  const Program::Impl& implementation = *program_.implementation();
  FunctionScope function_scope(implementation.functions);
  const std::size_t initial_count = static_cast<std::size_t>(std::count_if(
      implementation.states.begin(), implementation.states.end(),
      [](const State& state) { return state.initial; }));
  for (const State& state : implementation.states) {
    if (!state.initial) continue;
    active_states_.emplace(state.context, state.name);
    for (const auto& [field, value] : initial_values(state)) {
      values_.emplace(initial_count == 1U ? field : state_key(state.context, field), value);
    }
  }
  for (const auto& [context, state_name] : initial_states) {
    const State& state = find_state(implementation, state_name);
    if (state.context != context) {
      throw Error("initial override '" + state_name + "' does not belong to @" + context);
    }
    const State& previous = find_state(implementation, active_states_.at(context));
    for (const Field& field : previous.fields) {
      values_.erase(initial_count == 1U ? field.name : state_key(context, field.name));
    }
    active_states_.insert_or_assign(context, state_name);
    for (const auto& [field, value] : initial_values(state)) {
      values_.insert_or_assign(initial_count == 1U ? field : state_key(context, field), value);
    }
    verify_invariants(state, initial_values(state), 0);
  }
  for (const TraceDeclaration& trace : implementation.traces) {
    if (!trace.has_capture) continue;
    TraceSnapshot snapshot;
    snapshot.name = trace.name;
    snapshot.root_context = trace.root_context;
    snapshot.mode = trace.capture_mode;
    for (const StateBinding& binding : trace.capture) {
      snapshot.captured_contexts.insert(binding.context);
    }
    snapshot.captured_paths = trace.paths;
    if (trace.capture_mode == TraceCaptureMode::Projected) {
      snapshot.causal_gaps.push_back(
          "projected capture omits decisions outside selected @ contexts");
    }
    captured_traces_.emplace(trace.name, std::move(snapshot));
  }
}

Engine Engine::from_procedure(Program program, std::string_view procedure_name) {
  if (program.empty()) throw Error("cannot start a procedure from an empty program");
  const auto& procedures = program.implementation()->procedures;
  const auto found = procedures.find(procedure_name);
  if (found == procedures.end()) {
    throw Error("unknown procedure '" + std::string(procedure_name) + "'");
  }
  std::map<std::string, std::string, std::less<>> initial_states;
  for (const StateBinding& binding : found->second.initial_states) {
    initial_states.emplace(binding.context, binding.state);
  }
  const std::string initial_context = found->second.initial_context;
  Engine result(std::move(program), std::move(initial_states));
  result.initial_context_ = initial_context;
  return result;
}

ParallelStepResult Engine::step_parallel(const std::vector<Event>& events) {
  if (round_ == std::numeric_limits<std::uint64_t>::max()) {
    throw Error("simulation round overflow");
  }
  return step_parallel_at(events, round_ + 1U);
}

ParallelStepResult Engine::step_parallel_at(const std::vector<Event>& events,
                                            std::uint64_t round_id) {
  std::vector<std::pair<Event, std::string>> inputs;
  inputs.reserve(events.size());
  for (const Event& event : events) inputs.emplace_back(event, std::string{});
  return step_inputs_at(inputs, round_id);
}

StepResult Engine::step_transition(const TransitionInput& transition) {
  ParallelStepResult result = step_transitions({transition});
  return std::move(result.transitions.front());
}

ParallelStepResult Engine::step_transitions(
    const std::vector<TransitionInput>& transitions) {
  if (round_ == std::numeric_limits<std::uint64_t>::max()) {
    throw Error("simulation round overflow");
  }
  return step_transitions_at(transitions, round_ + 1U);
}

ParallelStepResult Engine::step_transitions_at(
    const std::vector<TransitionInput>& transitions, std::uint64_t round_id) {
  const Program::Impl& program = *program_.implementation();
  std::vector<std::pair<Event, std::string>> inputs;
  inputs.reserve(transitions.size());
  for (const TransitionInput& input : transitions) {
    const auto indexed = program.transition_index.find(input.transition);
    if (indexed == program.transition_index.end()) {
      throw Error("unknown injected transition '" + input.transition + "'");
    }
    const Transition& transition = program.transitions[indexed->second];
    inputs.emplace_back(Event{transition.event, input.fields}, input.transition);
  }
  return step_inputs_at(inputs, round_id);
}

ParallelStepResult Engine::step_inputs_at(
    const std::vector<std::pair<Event, std::string>>& inputs,
    std::uint64_t round_id) {
  if (inputs.empty()) throw Error("a parallel step needs at least one transition");
  if (round_id == 0U || round_id <= round_) {
    throw Error("causal RoundId must advance monotonically for an Engine");
  }
  const Program::Impl& program = *program_.implementation();
  FunctionScope function_scope(program.functions);
  struct Prepared {
    const Transition* transition;
    const std::string* case_name;
    const std::vector<StateBinding>* from;
    const std::vector<TransitionTarget>* to;
    const std::set<std::string, std::less<>>* read_set;
    const std::set<std::string, std::less<>>* write_set;
    std::map<std::string, Value, std::less<>> writes;
    std::map<std::string, std::string, std::less<>> targets;
    ActionPlan actions;
    std::set<std::string, std::less<>> causal_predecessors;
    std::string optimization_scope;
    std::optional<Value> optimized_score;
  };
  std::vector<Prepared> prepared;
  prepared.reserve(inputs.size());

  std::vector<const std::pair<Event, std::string>*> ordered_inputs;
  ordered_inputs.reserve(inputs.size());
  for (const auto& input : inputs) ordered_inputs.push_back(&input);
  std::stable_sort(
      ordered_inputs.begin(), ordered_inputs.end(),
      [](const auto* left, const auto* right) {
        if (event_less(left->first, right->first)) return true;
        if (event_less(right->first, left->first)) return false;
        return left->second < right->second;
      });

  for (const auto* input : ordered_inputs) {
    const Event& event = input->first;
    const std::string& injected_transition = input->second;
    struct Enabled {
      const Transition* transition;
      const std::string* case_name;
      const std::vector<StateBinding>* from;
      const std::vector<TransitionTarget>* to;
      const ExprPtr* condition;
      const std::shared_ptr<ActionExpr>* action;
      const std::set<std::string, std::less<>>* reads;
      const std::set<std::string, std::less<>>* writes;
    };
    std::vector<Enabled> enabled;
    std::vector<std::size_t> candidate_indices;
    if (!injected_transition.empty()) {
      candidate_indices.push_back(program.transition_index.at(injected_transition));
    } else {
      const auto candidates = program.event_index.find(event.name);
      if (candidates != program.event_index.end()) candidate_indices = candidates->second;
    }
    for (const std::size_t candidate_index : candidate_indices) {
      const Transition& transition = program.transitions[candidate_index];
      struct Route {
        const std::vector<StateBinding>* from;
        const std::string* case_name;
        const std::vector<TransitionTarget>* to;
        const ExprPtr* condition;
        const std::shared_ptr<ActionExpr>* action;
        const std::set<std::string, std::less<>>* reads;
        const std::set<std::string, std::less<>>* writes;
      };
      std::vector<Route> routes;
      routes.push_back(Route{&transition.from, &transition.case_name, &transition.to,
                             &transition.condition, &transition.action,
                             &transition.reads, &transition.writes});
      for (const TransitionAlternative& alternative : transition.alternatives) {
        routes.push_back(Route{&alternative.from, &alternative.name, &alternative.to,
                               &alternative.condition, &alternative.action,
                               &alternative.reads, &alternative.writes});
      }
      validate_event(transition, event, program.types);
      std::vector<std::size_t> candidate_routes;
      for (const RouteStateIndexBucket& bucket :
           program.route_state_index.at(transition.name)) {
        std::vector<std::string> signature;
        signature.reserve(bucket.contexts.size());
        bool complete = true;
        for (const std::string& context : bucket.contexts) {
          const auto active = active_states_.find(context);
          if (active == active_states_.end()) {
            complete = false;
            break;
          }
          signature.push_back(active->second);
        }
        if (!complete) continue;
        const auto indexed_routes = bucket.routes.find(signature);
        if (indexed_routes != bucket.routes.end()) {
          candidate_routes.insert(candidate_routes.end(),
                                  indexed_routes->second.begin(),
                                  indexed_routes->second.end());
        }
      }
      for (const std::size_t route_index : candidate_routes) {
        const Route& route = routes.at(route_index);
        Environment environment{values_, &event, {}, round_id - 1U};
        if (evaluate(*route.condition, environment).as_bool()) {
          enabled.push_back(Enabled{&transition, route.case_name, route.from, route.to,
                                    route.condition, route.action,
                                    route.reads, route.writes});
        }
      }
    }
    if (enabled.empty()) {
      throw Error("no transition accepts event '" + event.name + "' from active state set " +
                  current_state());
    }
    const auto prepare = [&](const Enabled& candidate) {
      const Transition& transition = *candidate.transition;
      const State& source = find_state(program, candidate.from->front().state);
      Environment environment{values_, &event, {}, round_id - 1U};
      Prepared decision{&transition, candidate.case_name, candidate.from, candidate.to,
                        candidate.reads, candidate.writes, {}, {}, {}, {}, {}, {}};
      for (const std::string& field : *candidate.reads) {
        const auto writers = last_writers_.find(field);
        if (writers != last_writers_.end()) {
          decision.causal_predecessors.insert(writers->second.begin(),
                                              writers->second.end());
        }
      }
      for (const TransitionTarget& target : *candidate.to) {
        const State& target_state = find_state(program, target.binding.state);
        const bool single_context = active_states_.size() == 1U;
        decision.targets.emplace(target.binding.context, target.binding.state);
        const auto active = active_states_.find(target.binding.context);
        if (active == active_states_.end() || active->second != target.binding.state) {
          for (const auto& [field, value] : initial_values(target_state)) {
            decision.writes.insert_or_assign(
                single_context ? field : state_key(target.binding.context, field), value);
          }
        }
        for (const Assignment& assignment : target.assignments) {
          Value value = evaluate(assignment.value, environment);
          const Field& field = find_field(target_state, assignment.field);
          if (!value_matches_type(value, field.type, program.types)) {
            throw Error("assignment to '" + assignment.field + "' has the wrong type");
          }
          decision.writes.insert_or_assign(
              single_context ? assignment.field
                             : state_key(target.binding.context, assignment.field),
              std::move(value));
        }
      }
      if (*candidate.action) {
        std::unordered_set<std::string> labels;
        build_plan(*candidate.action, environment, source, decision.actions, labels);
        std::sort(decision.actions.dependencies.begin(), decision.actions.dependencies.end());
        decision.actions.dependencies.erase(
            std::unique(decision.actions.dependencies.begin(),
                        decision.actions.dependencies.end()),
            decision.actions.dependencies.end());
      }
      if (transition.optimized_score) {
        std::map<std::string, std::string, std::less<>> candidate_active = active_states_;
        for (const auto& [context, target] : decision.targets) {
          candidate_active.insert_or_assign(context, target);
        }
        std::map<std::string, Value, std::less<>> candidate_state = values_;
        for (const auto& [context, target_state] : candidate_active) {
          const auto previous = active_states_.find(context);
          if (previous == active_states_.end() || previous->second == target_state) continue;
          const State& old_state = find_state(program, previous->second);
          for (const Field& field : old_state.fields) {
            candidate_state.erase(active_states_.size() == 1U
                                      ? field.name
                                      : state_key(context, field.name));
          }
        }
        for (const auto& [field, value] : decision.writes) {
          candidate_state.insert_or_assign(field, value);
        }
        std::map<std::string, Value, std::less<>> score_state = candidate_state;
        std::map<std::string, Value, std::less<>> score_before = values_;
        if (active_states_.size() == 1U) {
          const State& target_state = find_state(
              program, candidate_active.at(transition.optimization_scope));
          for (const Field& field : target_state.fields) {
            const auto value = candidate_state.find(field.name);
            if (value != candidate_state.end()) {
              score_state.insert_or_assign(
                  state_key(transition.optimization_scope, field.name), value->second);
            }
          }
          const State& before_scope_state = find_state(
              program, active_states_.at(transition.optimization_scope));
          for (const Field& field : before_scope_state.fields) {
            const auto value = values_.find(field.name);
            if (value != values_.end()) {
              score_before.insert_or_assign(
                  state_key(transition.optimization_scope, field.name), value->second);
            }
          }
        }
        Environment score_environment{score_state, &event, {}, round_id - 1U,
                                      &score_before};
        Value score = evaluate(transition.optimized_score, score_environment);
        if (!is_exact_numeric(score)) {
          throw Error("optimized_score for transition '" + transition.name +
                      "' did not evaluate to an exact number");
        }
        decision.optimization_scope = transition.optimization_scope;
        decision.optimized_score = std::move(score);
      }
      return decision;
    };

    std::vector<Prepared> candidates;
    candidates.reserve(enabled.size());
    for (const Enabled& candidate : enabled) candidates.push_back(prepare(candidate));
    std::size_t selected_index = 0;
    if (candidates.size() > 1U) {
      const std::string& scope = candidates.front().optimization_scope;
      if (scope.empty() ||
          std::any_of(candidates.begin(), candidates.end(), [&](const Prepared& candidate) {
            return !candidate.optimized_score || candidate.optimization_scope != scope;
          })) {
        throw Error("event '" + event.name +
                    "' enables multiple transitions without one common optimized_score scope");
      }
      bool tied = false;
      for (std::size_t index = 1; index < candidates.size(); ++index) {
        const int order = compare_values(*candidates[index].optimized_score,
                                         *candidates[selected_index].optimized_score);
        if (order > 0) {
          selected_index = index;
          tied = false;
        } else if (order == 0) {
          tied = true;
        }
      }
      if (tied) {
        throw Error("optimized_score ties multiple transitions for event '" +
                    event.name + "' in @" + scope);
      }
    }
    prepared.push_back(std::move(candidates[selected_index]));
  }

  std::map<std::string, std::string, std::less<>> next_active = active_states_;
  for (const Prepared& decision : prepared) {
    for (const auto& [context, target] : decision.targets) {
      const auto [found, inserted] = next_active.insert_or_assign(context, target);
      static_cast<void>(found);
      static_cast<void>(inserted);
      for (const Prepared& other : prepared) {
        const auto conflicting = other.targets.find(context);
        if (conflicting != other.targets.end() && conflicting->second != target) {
          throw Error("parallel transitions choose different states for @" + context);
        }
      }
    }
  }
  std::map<std::string, Value, std::less<>> next = values_;
  for (const auto& [context, target_state] : next_active) {
    const auto previous = active_states_.find(context);
    if (previous == active_states_.end() || previous->second == target_state) continue;
    const State& old_state = find_state(program, previous->second);
    for (const Field& field : old_state.fields) {
      next.erase(active_states_.size() == 1U ? field.name : state_key(context, field.name));
    }
  }
  std::map<std::string, std::vector<Value>, std::less<>> candidates;
  for (const Prepared& decision : prepared) {
    for (const auto& [field, value] : decision.writes) {
      candidates[field].push_back(value);
    }
  }
  for (const auto& [name, values] : candidates) {
    if (values.size() == 1) {
      next.insert_or_assign(name, values.front());
      continue;
    }
    const std::size_t dot = name.find('.');
    const std::string context =
        dot == std::string::npos && next_active.size() == 1U
            ? next_active.begin()->first
            : (dot == std::string::npos ? "" : name.substr(0, dot));
    const std::string field_name = dot == std::string::npos ? name : name.substr(dot + 1U);
    const Field& field = find_field(find_state(program, next_active.at(context)), field_name);
    if (field.merge == Field::Merge::Reject) {
      throw Error("parallel transitions write the same field '" + name + "'");
    }
    if (field.merge == Field::Merge::Equal) {
      if (!std::all_of(values.begin() + 1, values.end(),
                       [&](const Value& value) { return value == values.front(); })) {
        throw Error("equal merge disagrees for field '" + name + "'");
      }
      next.insert_or_assign(name, values.front());
      continue;
    }
    if (next.at(name).kind() == Value::Kind::StringSet) {
      StringSet merged = next.at(name).as_string_set();
      for (const Value& value : values) {
        const StringSet& candidate = value.as_string_set();
        merged.values.insert(candidate.values.begin(), candidate.values.end());
      }
      next.insert_or_assign(name, Value(std::move(merged)));
      continue;
    }
    ValueSet merged = next.at(name).as_set();
    for (const Value& value : values) {
      const auto& candidate = value.as_set().values;
      merged.values.insert(merged.values.end(), candidate.begin(), candidate.end());
    }
    next.insert_or_assign(name, Value(std::move(merged)));
  }
  for (const auto& [context, state_name] : next_active) {
    const State& state = find_state(program, state_name);
    verify_invariants(state, local_state_values(next, context, state), round_id);
  }

  ParallelStepResult result;
  result.round = round_id;
  result.state = next;
  result.transitions.reserve(prepared.size());
  for (std::size_t index = 0; index < prepared.size(); ++index) {
    Prepared& decision = prepared[index];
    StepResult step_result;
    step_result.round = result.round;
    step_result.id = "r" + std::to_string(result.round) + ":" + std::to_string(index);
    step_result.case_name = *decision.case_name;
    step_result.transition = decision.transition->name;
    if (!step_result.case_name.empty()) step_result.transition += "." + step_result.case_name;
    step_result.optimization_scope = std::move(decision.optimization_scope);
    step_result.optimized_score = std::move(decision.optimized_score);
    step_result.from_state = binding_set_text(*decision.from);
    step_result.to_state = target_set_text(*decision.to);
    step_result.active_states = next_active;
    step_result.state = next;
    step_result.actions = std::move(decision.actions);
    step_result.reads = *decision.read_set;
    step_result.writes = *decision.write_set;
    step_result.causal_predecessors = std::move(decision.causal_predecessors);
    result.transitions.push_back(std::move(step_result));
  }

  for (const auto& [field, values] : candidates) {
    static_cast<void>(values);
    last_writers_[field].clear();
  }
  for (const StepResult& decision : result.transitions) {
    for (const std::string& field : decision.writes) {
      last_writers_[field].insert(decision.id);
    }
  }

  round_ = round_id;
  active_states_ = std::move(next_active);
  values_ = std::move(next);

  for (auto& [name, trace] : captured_traces_) {
    static_cast<void>(name);
    auto belongs = [&](std::string_view field) {
      if (trace.captured_contexts.empty()) return true;
      return std::any_of(trace.captured_contexts.begin(), trace.captured_contexts.end(),
                         [&](const std::string& context) {
                           return context.empty() || field.starts_with(context + ".") ||
                                  (active_states_.size() == 1U &&
                                   active_states_.begin()->first == context &&
                                   field.find('.') == std::string_view::npos);
                         });
    };
    const auto path_selected = [&](const StepResult& step_result) {
      return trace.captured_paths.empty() ||
             trace.captured_paths.contains(step_result.transition);
    };
    if (trace.mode == TraceCaptureMode::Projected && !trace.captured_paths.empty() &&
        std::none_of(result.transitions.begin(), result.transitions.end(), path_selected)) {
      continue;
    }
    ParallelStepResult captured = result;
    if (trace.mode == TraceCaptureMode::Projected) {
      captured.transitions.erase(
          std::remove_if(captured.transitions.begin(), captured.transitions.end(),
                         [&](const StepResult& step_result) {
                           const bool touches_context = trace.captured_contexts.empty() ||
                               std::any_of(step_result.reads.begin(), step_result.reads.end(), belongs) ||
                               std::any_of(step_result.writes.begin(), step_result.writes.end(), belongs);
                           return !path_selected(step_result) || !touches_context;
                         }),
          captured.transitions.end());
      if (captured.transitions.empty()) continue;
    }
    for (auto it = captured.state.begin(); it != captured.state.end();) {
      if (!belongs(it->first)) it = captured.state.erase(it);
      else ++it;
    }
    for (StepResult& step_result : captured.transitions) {
      for (auto it = step_result.state.begin(); it != step_result.state.end();) {
        if (!belongs(it->first)) it = step_result.state.erase(it);
        else ++it;
      }
    }
    trace.rounds.push_back(std::move(captured));
    trace.final_state = trace.rounds.back().state;
  }
  return result;
}

StepResult Engine::step(const Event& event) {
  ParallelStepResult result = step_parallel(std::vector<Event>{event});
  return std::move(result.transitions.front());
}

std::string Engine::current_state() const {
  if (active_states_.size() == 1U && active_states_.begin()->first.empty()) {
    return active_states_.begin()->second;
  }
  std::string result = "{";
  bool first = true;
  for (const auto& [context, state] : active_states_) {
    if (!first) result += ", ";
    first = false;
    result += state;
    if (!context.empty()) result += "@" + context;
  }
  return result + "}";
}
const std::map<std::string, std::string, std::less<>>& Engine::current_states() const noexcept {
  return active_states_;
}

const std::string& Engine::initial_context() const noexcept { return initial_context_; }
std::uint64_t Engine::current_round() const noexcept { return round_; }
const std::map<std::string, Value, std::less<>>& Engine::values() const { return values_; }

struct RuntimeContext::Impl {
  explicit Impl(Program source) : program(std::move(source)) {}

  Program program;
  std::uint64_t round{0};
  std::map<std::string, std::unique_ptr<Engine>, std::less<>> engines;
  std::map<std::string, std::uint64_t, std::less<>> revisions;
  std::map<std::string, ProcedureArtifact, std::less<>> artifacts;
  std::vector<ParallelStepResult> rounds;
  std::map<std::string, std::vector<ProcedureTraceFrame>, std::less<>> history;
};

RuntimeContext::RuntimeContext(Program program)
    : impl_(std::make_unique<Impl>(std::move(program))) {
  if (impl_->program.empty()) throw Error("cannot construct an empty RuntimeContext");
}

RuntimeContext::~RuntimeContext() = default;
RuntimeContext::RuntimeContext(RuntimeContext&&) noexcept = default;
RuntimeContext& RuntimeContext::operator=(RuntimeContext&&) noexcept = default;

void RuntimeContext::start(std::string_view procedure_name) {
  const std::string name(procedure_name);
  if (impl_->engines.contains(name)) {
    throw Error("procedure '" + name + "' is already started");
  }
  const auto declaration = impl_->program.implementation()->procedures.find(name);
  if (declaration == impl_->program.implementation()->procedures.end()) {
    throw Error("unknown procedure '" + name + "'");
  }
  auto engine = std::make_unique<Engine>(
      Engine::from_procedure(impl_->program, name));
  ProcedureArtifact artifact;
  artifact.procedure = name;
  artifact.initial_context = declaration->second.initial_context;
  for (const StateBinding& binding : declaration->second.initial_states) {
    artifact.initial_states.emplace(binding.context, binding.state);
  }
  impl_->engines.emplace(name, std::move(engine));
  impl_->revisions.emplace(name, 0U);
  impl_->artifacts.emplace(name, std::move(artifact));
}

ParallelStepResult RuntimeContext::inject(
    const std::vector<std::pair<std::string, TransitionInput>>& transitions) {
  if (impl_->round == std::numeric_limits<std::uint64_t>::max()) {
    throw Error("RuntimeContext round overflow");
  }
  return inject_at(transitions, impl_->round + 1U);
}

ParallelStepResult RuntimeContext::inject_at(
    const std::vector<std::pair<std::string, TransitionInput>>& transitions,
    std::uint64_t round_id) {
  if (transitions.empty()) throw Error("a RuntimeContext round needs an injected transition");
  if (transitions.size() > event_batch_size_limit) {
    throw Error("RuntimeContext round exceeds transition limit");
  }
  if (round_id == 0U || round_id <= impl_->round) {
    throw Error("RuntimeContext RoundId must advance monotonically");
  }
  std::map<std::string, std::vector<TransitionInput>, std::less<>> grouped;
  for (const auto& [procedure, transition] : transitions) {
    const auto engine = impl_->engines.find(procedure);
    if (engine == impl_->engines.end()) {
      throw Error("procedure '" + procedure + "' is not started");
    }
    grouped[procedure].push_back(transition);
  }
  for (auto& [procedure, pending] : grouped) {
    static_cast<void>(procedure);
    std::stable_sort(pending.begin(), pending.end(), transition_input_less);
  }

  ParallelStepResult aggregate;
  aggregate.round = round_id;
  std::map<std::string, Engine, std::less<>> candidates;
  for (const auto& [procedure, pending] : grouped) {
    auto [candidate, inserted] = candidates.emplace(
        procedure, *impl_->engines.at(procedure));
    static_cast<void>(inserted);
    ParallelStepResult local = candidate->second.step_transitions_at(pending, round_id);
    const std::uint64_t revision = impl_->revisions.at(procedure) + 1U;
    for (StepResult& step : local.transitions) {
      step.procedure = procedure;
      step.procedure_revision = revision;
      step.id = procedure + "/" + step.id;
      std::set<std::string, std::less<>> predecessors;
      for (const std::string& predecessor : step.causal_predecessors) {
        predecessors.insert(procedure + "/" + predecessor);
      }
      step.causal_predecessors = std::move(predecessors);
      aggregate.transitions.push_back(std::move(step));
    }
  }
  for (auto& [procedure, candidate] : candidates) {
    *impl_->engines.at(procedure) = std::move(candidate);
    ++impl_->revisions.at(procedure);
  }
  std::sort(aggregate.transitions.begin(), aggregate.transitions.end(),
            [](const StepResult& left, const StepResult& right) {
              return std::tie(left.procedure, left.transition, left.id) <
                     std::tie(right.procedure, right.transition, right.id);
            });
  const bool single = impl_->engines.size() == 1U;
  for (const auto& [procedure, engine] : impl_->engines) {
    for (const auto& [field, value] : engine->values()) {
      aggregate.state.emplace(procedure + "." + field, value);
      if (single) aggregate.state.emplace(field, value);
    }
  }
  for (const auto& [procedure, pending] : grouped) {
    for (const TransitionInput& transition : pending) {
      impl_->artifacts.at(procedure).injections.push_back(
          ProcedureInjection{round_id, transition});
    }
  }
  for (const auto& [procedure, engine] : impl_->engines) {
    ProcedureTraceFrame frame;
    frame.round = round_id;
    frame.procedure_revision = impl_->revisions.at(procedure);
    frame.context = engine->initial_context();
    frame.active_states = engine->current_states();
    frame.state = engine->values();
    for (const StepResult& step : aggregate.transitions) {
      if (step.procedure == procedure) frame.transitions.push_back(step.transition);
    }
    impl_->history[procedure].push_back(std::move(frame));
  }
  impl_->round = round_id;
  impl_->rounds.push_back(aggregate);
  return aggregate;
}

std::uint64_t RuntimeContext::current_round() const noexcept { return impl_->round; }

ProcedureArtifact RuntimeContext::artifact(std::string_view procedure) const {
  const auto found = impl_->artifacts.find(procedure);
  if (found == impl_->artifacts.end()) {
    throw Error("procedure '" + std::string(procedure) + "' is not started");
  }
  return found->second;
}

TraceSnapshot RuntimeContext::snapshot(std::string_view name) const {
  TraceSnapshot result;
  result.name = std::string(name);
  result.mode = TraceCaptureMode::Closed;
  result.closed = true;
  result.replayable = true;
  result.rounds = impl_->rounds;
  result.procedure_history = impl_->history;
  result.procedure_artifacts = impl_->artifacts;
  result.captured_procedures.clear();
  for (const auto& [procedure, engine] : impl_->engines) {
    result.captured_procedures.insert(procedure);
    result.procedure_states.emplace(procedure, engine->values());
    result.procedure_contexts.emplace(procedure, engine->initial_context());
  }
  if (!result.rounds.empty()) result.final_state = result.rounds.back().state;
  return result;
}

TraceResult run_trace(const Program& program, const EventTrace& trace) {
  if (trace.rounds.size() > event_trace_round_limit) {
    throw Error("DTESSL EventTrace exceeds round limit");
  }
  Engine engine(program);
  TraceResult result;
  result.rounds.reserve(trace.rounds.size());
  for (const EventBatch& batch : trace.rounds) {
    if (batch.events.empty()) throw Error("DTESSL EventTrace contains an empty event batch");
    if (batch.events.size() > event_batch_size_limit) {
      throw Error("DTESSL EventTrace batch exceeds event limit");
    }
    result.rounds.push_back(engine.step_parallel(batch.events));
  }
  result.final_state_name = engine.current_state();
  result.final_state = engine.values();
  return result;
}

TraceResult replay_trace(const Program& program, const EventTrace& trace) {
  const TraceResult first = run_trace(program, trace);
  const TraceResult second = run_trace(program, trace);
  if (first != second) throw Error("DTESSL EventTrace replay diverged");
  return first;
}

TraceSnapshot replay_procedures(
    const Program& program, const std::vector<ProcedureArtifact>& artifacts,
    const std::vector<SearchExpectation>& expectations) {
  if (artifacts.empty()) throw Error("procedure replay needs at least one artifact");
  const auto execute = [&]() {
    RuntimeContext runtime(program);
    std::map<std::uint64_t,
             std::vector<std::pair<std::string, TransitionInput>>> rounds;
    std::set<std::string, std::less<>> names;
    for (const ProcedureArtifact& artifact : artifacts) {
      if (!names.insert(artifact.procedure).second) {
        throw Error("procedure replay repeats artifact '" + artifact.procedure + "'");
      }
      const auto declaration = program.implementation()->procedures.find(artifact.procedure);
      if (declaration == program.implementation()->procedures.end()) {
        throw Error("procedure replay references unknown procedure '" +
                    artifact.procedure + "'");
      }
      std::map<std::string, std::string, std::less<>> declared_states;
      for (const StateBinding& binding : declaration->second.initial_states) {
        declared_states.emplace(binding.context, binding.state);
      }
      if (artifact.initial_context != declaration->second.initial_context ||
          artifact.initial_states != declared_states) {
        throw Error("procedure artifact initial configuration does not match '" +
                    artifact.procedure + "'");
      }
      runtime.start(artifact.procedure);
      std::uint64_t previous = 0;
      for (const ProcedureInjection& injection : artifact.injections) {
        if (injection.round == 0U || injection.round < previous) {
          throw Error("procedure artifact injection rounds must not decrease");
        }
        previous = injection.round;
        rounds[injection.round].emplace_back(artifact.procedure, injection.transition);
      }
    }
    for (const auto& [round, contexts] : rounds) {
      static_cast<void>(runtime.inject_at(contexts, round));
    }
    TraceSnapshot result = runtime.snapshot("procedure-replay");
    for (const SearchExpectation& expected : expectations) {
      const auto round = std::find_if(
          result.rounds.begin(), result.rounds.end(), [&](const ParallelStepResult& item) {
            return item.round == expected.round;
          });
      const bool found = round != result.rounds.end() && std::any_of(
          round->transitions.begin(), round->transitions.end(),
          [&](const StepResult& step) {
            return step.procedure == expected.procedure &&
                   step.transition == expected.transition;
          });
      if (!found) {
        throw Error("search replay expectation did not match " + expected.procedure + "/" +
                    expected.transition + " at RoundId " +
                    std::to_string(expected.round));
      }
    }
    return result;
  };
  const TraceSnapshot first = execute();
  const TraceSnapshot second = execute();
  if (first != second) throw Error("procedure replay diverged");
  return first;
}

std::vector<std::string> declared_traces(const Program& program) {
  if (program.empty()) throw Error("cannot inspect traces of an empty program");
  std::vector<std::string> result;
  for (const TraceDeclaration& trace : program.implementation()->traces) {
    result.push_back(trace.name);
  }
  return result;
}

std::vector<std::string> declared_procedures(const Program& program) {
  if (program.empty()) throw Error("cannot inspect procedures of an empty program");
  std::vector<std::string> result;
  for (const auto& [name, procedure] : program.implementation()->procedures) {
    static_cast<void>(procedure);
    result.push_back(name);
  }
  return result;
}

TraceSnapshot run_named_trace(const Program& program, std::string_view name) {
  if (program.empty()) throw Error("cannot run a trace from an empty program");
  const auto& traces = program.implementation()->traces;
  const auto found = std::find_if(traces.begin(), traces.end(),
                                  [&](const TraceDeclaration& trace) {
                                    return trace.name == name;
                                  });
  if (found == traces.end()) throw Error("unknown trace '" + std::string(name) + "'");
  if (!found->has_replay) {
    throw Error("trace '" + std::string(name) +
                "' has no replay section; inspect its native Engine capture instead");
  }
  struct NamedReplayExecution {
    TraceResult logical;
    TraceSnapshot snapshot;
  };
  const auto execute = [&]() {
    if (found->events.rounds.size() > event_trace_round_limit) {
      throw Error("DTESSL replay exceeds round limit");
    }
    NamedReplayExecution execution;
    std::set<std::string, std::less<>> referenced_procedures;
    for (const auto& procedures : found->replay_procedures) {
      referenced_procedures.insert(procedures.begin(), procedures.end());
    }
    referenced_procedures.insert(found->captured_procedures.begin(),
                                 found->captured_procedures.end());
    std::map<std::string, std::unique_ptr<Engine>, std::less<>> engines;
    std::map<std::string, std::uint64_t, std::less<>> revisions;
    for (const std::string& procedure : referenced_procedures) {
      engines.emplace(
          procedure,
          std::make_unique<Engine>(Engine::from_procedure(program, procedure)));
      revisions.emplace(procedure, 0U);
    }
    const bool single_procedure = engines.size() == 1U;
    const auto state_snapshot = [&]() {
      std::map<std::string, Value, std::less<>> state;
      for (const auto& [procedure, engine] : engines) {
        for (const auto& [field, value] : engine->values()) {
          state.emplace(procedure + "." + field, value);
          if (single_procedure) state.emplace(field, value);
        }
      }
      return state;
    };
    for (std::size_t round_index = 0; round_index < found->events.rounds.size();
         ++round_index) {
      const EventBatch& batch = found->events.rounds[round_index];
      if (batch.events.empty()) throw Error("DTESSL replay contains an empty event batch");
      if (batch.events.size() > event_batch_size_limit) {
        throw Error("DTESSL replay batch exceeds event limit");
      }
      const auto& procedures = found->replay_procedures.at(round_index);
      if (procedures.size() != batch.events.size()) {
        throw Error("replay round has inconsistent procedure bindings");
      }
      std::map<std::string, std::vector<std::size_t>, std::less<>> by_procedure;
      for (std::size_t index = 0; index < procedures.size(); ++index) {
        by_procedure[procedures[index]].push_back(index);
      }
      ParallelStepResult aggregate;
      aggregate.round = static_cast<std::uint64_t>(round_index + 1U);
      for (const auto& [procedure, indices] : by_procedure) {
        std::vector<TransitionInput> pending;
        std::vector<std::string> expected_paths;
        pending.reserve(indices.size());
        expected_paths.reserve(indices.size());
        for (const std::size_t index : indices) {
          pending.push_back(TransitionInput{
              found->replay_transitions.at(round_index).at(index),
              batch.events[index].fields});
          const std::string& expected = found->replay_paths.at(round_index).at(index);
          if (!expected.empty()) expected_paths.push_back(expected);
        }
        ParallelStepResult local = engines.at(procedure)->step_transitions_at(
            pending, aggregate.round);
        const std::uint64_t revision = ++revisions.at(procedure);
        std::vector<std::string> actual_paths;
        actual_paths.reserve(local.transitions.size());
        for (StepResult& step : local.transitions) {
          actual_paths.push_back(step.transition);
          step.procedure = procedure;
          step.procedure_revision = revision;
          step.round = aggregate.round;
          step.id = procedure + "/" + step.id;
          std::set<std::string, std::less<>> qualified_predecessors;
          for (const std::string& predecessor : step.causal_predecessors) {
            qualified_predecessors.insert(procedure + "/" + predecessor);
          }
          step.causal_predecessors = std::move(qualified_predecessors);
          aggregate.transitions.push_back(std::move(step));
        }
        std::sort(actual_paths.begin(), actual_paths.end());
        std::sort(expected_paths.begin(), expected_paths.end());
        const bool expectations_met = std::all_of(
            expected_paths.begin(), expected_paths.end(), [&](const std::string& expected) {
              return std::binary_search(actual_paths.begin(), actual_paths.end(), expected);
            });
        if (!expectations_met) {
          throw Error("replay round " + std::to_string(round_index + 1U) +
                      " selected a different transition path for procedure '" +
                      procedure + "'");
        }
      }
      std::sort(aggregate.transitions.begin(), aggregate.transitions.end(),
                [](const StepResult& left, const StepResult& right) {
                  return std::tie(left.procedure, left.transition, left.id) <
                         std::tie(right.procedure, right.transition, right.id);
                });
      aggregate.state = state_snapshot();
      for (const std::string& procedure : referenced_procedures) {
        ProcedureTraceFrame frame;
        frame.round = aggregate.round;
        frame.procedure_revision = revisions.at(procedure);
        frame.context = engines.at(procedure)->initial_context();
        frame.active_states = engines.at(procedure)->current_states();
        frame.state = engines.at(procedure)->values();
        for (const StepResult& step : aggregate.transitions) {
          if (step.procedure == procedure) frame.transitions.push_back(step.transition);
        }
        execution.snapshot.procedure_history[procedure].push_back(std::move(frame));
      }
      execution.logical.rounds.push_back(std::move(aggregate));
    }
    bool first_state = true;
    for (const auto& [procedure, engine] : engines) {
      if (!first_state) execution.logical.final_state_name += ", ";
      first_state = false;
      execution.logical.final_state_name += procedure + "=" + engine->current_state();
    }
    execution.logical.final_state = state_snapshot();
    execution.snapshot.name = found->name;
    execution.snapshot.root_context = found->root_context;
    execution.snapshot.mode = found->has_capture ? found->capture_mode
                                                 : TraceCaptureMode::Static;
    execution.snapshot.closed = true;
    execution.snapshot.rounds = execution.logical.rounds;
    execution.snapshot.final_state = execution.logical.final_state;
    for (const auto& [procedure, engine] : engines) {
      if (found->has_capture && !found->captured_procedures.empty() &&
          !found->captured_procedures.contains(procedure)) {
        continue;
      }
      execution.snapshot.procedure_states.emplace(procedure, engine->values());
      execution.snapshot.procedure_contexts.emplace(procedure, engine->initial_context());
    }
    for (const StateBinding& binding : found->capture) {
      execution.snapshot.captured_contexts.insert(binding.context);
    }
    execution.snapshot.captured_paths = found->paths;
    execution.snapshot.captured_procedures = found->captured_procedures;
    std::set<std::string, std::less<>> closure_procedures =
        found->captured_procedures;
    if (closure_procedures.empty()) {
      for (const ParallelStepResult& round : execution.logical.rounds) {
        for (const StepResult& step : round.transitions) {
          const bool path_seed = found->paths.empty() || found->paths.contains(step.transition);
          const bool state_seed = found->capture.empty() || std::any_of(
              found->capture.begin(), found->capture.end(), [&](const StateBinding& binding) {
                const auto active = step.active_states.find(binding.context);
                return active != step.active_states.end() && active->second == binding.state;
              });
          if (path_seed && state_seed) closure_procedures.insert(step.procedure);
        }
      }
    }
    if (closure_procedures.empty() && found->capture.empty() && found->paths.empty()) {
      closure_procedures = referenced_procedures;
    }
    if (!found->has_capture || found->capture_mode == TraceCaptureMode::Closed) {
      execution.snapshot.captured_procedures = closure_procedures;
      for (const std::string& procedure : closure_procedures) {
        const auto declaration = program.implementation()->procedures.find(procedure);
        if (declaration == program.implementation()->procedures.end()) continue;
        ProcedureArtifact artifact;
        artifact.procedure = procedure;
        artifact.initial_context = declaration->second.initial_context;
        for (const StateBinding& binding : declaration->second.initial_states) {
          artifact.initial_states.emplace(binding.context, binding.state);
        }
        for (std::size_t round_index = 0; round_index < found->events.rounds.size();
             ++round_index) {
          const auto& procedures = found->replay_procedures.at(round_index);
          for (std::size_t index = 0; index < procedures.size(); ++index) {
            if (procedures[index] == procedure) {
              artifact.injections.push_back(ProcedureInjection{
                  static_cast<std::uint64_t>(round_index + 1U),
                  TransitionInput{
                      found->replay_transitions.at(round_index).at(index),
                      found->events.rounds[round_index].events[index].fields}});
            }
          }
        }
        std::stable_sort(
            artifact.injections.begin(), artifact.injections.end(),
            [](const ProcedureInjection& left, const ProcedureInjection& right) {
              if (left.round != right.round) return left.round < right.round;
              return transition_input_less(left.transition, right.transition);
            });
        execution.snapshot.procedure_artifacts.emplace(procedure, std::move(artifact));
      }
      execution.snapshot.replayable =
          !execution.snapshot.procedure_artifacts.empty();
      for (auto history = execution.snapshot.procedure_history.begin();
           history != execution.snapshot.procedure_history.end();) {
        if (!closure_procedures.contains(history->first)) {
          history = execution.snapshot.procedure_history.erase(history);
        } else {
          ++history;
        }
      }
    }
    if (found->has_capture) {
      const auto local_field = [&](std::string_view field) {
        for (const std::string& procedure : referenced_procedures) {
          const std::string prefix = procedure + ".";
          if (field.starts_with(prefix)) return std::string(field.substr(prefix.size()));
        }
        return std::string(field);
      };
      const auto field_procedure_selected = [&](std::string_view field) {
        if (execution.snapshot.captured_procedures.empty()) return true;
        if (single_procedure &&
            std::none_of(referenced_procedures.begin(), referenced_procedures.end(),
                         [&](const std::string& procedure) {
                           return field.starts_with(procedure + ".");
                         })) {
          return true;
        }
        return std::any_of(
            execution.snapshot.captured_procedures.begin(),
            execution.snapshot.captured_procedures.end(),
            [&](const std::string& procedure) {
              return field.starts_with(procedure + ".");
            });
      };
      const auto belongs = [&](std::string_view field) {
        if (!field_procedure_selected(field)) return false;
        if (execution.snapshot.captured_contexts.empty()) return true;
        const std::string local = local_field(field);
        return std::any_of(
            execution.snapshot.captured_contexts.begin(),
            execution.snapshot.captured_contexts.end(),
            [&](const std::string& context) {
              return context.empty() || local.starts_with(context + ".") ||
                     (local.find('.') == std::string::npos &&
                      referenced_procedures.size() == 1U);
            });
      };
      const auto path_selected = [&](const StepResult& step) {
        return execution.snapshot.captured_paths.empty() ||
               execution.snapshot.captured_paths.contains(step.transition);
      };
      const auto procedure_selected = [&](const StepResult& step) {
        return execution.snapshot.captured_procedures.empty() ||
               execution.snapshot.captured_procedures.contains(step.procedure);
      };
      const auto state_selected = [&](const StepResult& step) {
        return found->capture.empty() ||
               std::any_of(found->capture.begin(), found->capture.end(),
                           [&](const StateBinding& binding) {
                             const auto active = step.active_states.find(binding.context);
                             return active != step.active_states.end() &&
                                    active->second == binding.state;
                           });
      };
      if (found->capture_mode == TraceCaptureMode::Projected) {
        execution.snapshot.replayable = false;
        execution.snapshot.procedure_artifacts.clear();
        execution.snapshot.causal_gaps.push_back(
            "projected capture omits decisions outside selected procedures, paths, or contexts");
      }
      for (auto round = execution.snapshot.rounds.begin();
           round != execution.snapshot.rounds.end();) {
        if (!execution.snapshot.captured_paths.empty() ||
            !execution.snapshot.captured_procedures.empty() || !found->capture.empty()) {
          round->transitions.erase(
              std::remove_if(
                  round->transitions.begin(), round->transitions.end(),
                  [&](const StepResult& step) {
                    const bool touches = execution.snapshot.captured_contexts.empty() ||
                        std::any_of(step.reads.begin(), step.reads.end(), belongs) ||
                        std::any_of(step.writes.begin(), step.writes.end(), belongs);
                    return !procedure_selected(step) || !path_selected(step) ||
                           !state_selected(step) || !touches;
                  }),
              round->transitions.end());
          if (round->transitions.empty() &&
              execution.snapshot.procedure_history.empty()) {
            round = execution.snapshot.rounds.erase(round);
            continue;
          }
        }
        for (auto item = round->state.begin(); item != round->state.end();) {
          if (!belongs(item->first)) item = round->state.erase(item);
          else ++item;
        }
        for (StepResult& step : round->transitions) {
          for (auto item = step.state.begin(); item != step.state.end();) {
            const bool local_belongs = execution.snapshot.captured_contexts.empty() ||
                std::any_of(
                    execution.snapshot.captured_contexts.begin(),
                    execution.snapshot.captured_contexts.end(),
                    [&](const std::string& context) {
                      return item->first.starts_with(context + ".") ||
                             item->first.find('.') == std::string::npos;
                    });
            if (!procedure_selected(step) || !local_belongs) {
              item = step.state.erase(item);
            }
            else ++item;
          }
        }
        ++round;
      }
      for (auto item = execution.snapshot.final_state.begin();
           item != execution.snapshot.final_state.end();) {
        if (!belongs(item->first)) item = execution.snapshot.final_state.erase(item);
        else ++item;
      }
      for (auto& [procedure, state] : execution.snapshot.procedure_states) {
        static_cast<void>(procedure);
        for (auto item = state.begin(); item != state.end();) {
          const bool local_belongs = execution.snapshot.captured_contexts.empty() ||
              std::any_of(
                  execution.snapshot.captured_contexts.begin(),
                  execution.snapshot.captured_contexts.end(),
                  [&](const std::string& context) {
                    return item->first.starts_with(context + ".") ||
                           item->first.find('.') == std::string::npos;
                  });
          if (!local_belongs) item = state.erase(item);
          else ++item;
        }
      }
    }
    return execution;
  };
  const NamedReplayExecution execution = execute();
  const NamedReplayExecution replayed = execute();
  if (execution.logical != replayed.logical || execution.snapshot != replayed.snapshot) {
    throw Error("named replay diverged");
  }
  return execution.snapshot;
}

namespace {

std::vector<ClaimEvaluation> evaluate_trace_claims(
    const Program::Impl& program, const TraceSnapshot& trace) {
  FunctionScope function_scope(program.functions);
  std::vector<ClaimEvaluation> results;
  for (const ClaimDeclaration& claim : program.claims) {
    if (claim.trace != trace.name) continue;
    ClaimEvaluation result;
    result.name = claim.name;
    result.trace = trace.name;
    if (claim.kind == ClaimDeclaration::Kind::CountAtMost) {
      std::uint64_t count = 0;
      for (const ParallelStepResult& round : trace.rounds) {
        count += static_cast<std::uint64_t>(std::count_if(
            round.transitions.begin(), round.transitions.end(),
            [&](const StepResult& step) {
              return step.transition == claim.transition ||
                     (claim.transition.find('.') == std::string::npos &&
                      step.transition.starts_with(claim.transition + "."));
            }));
        if (count > claim.limit) {
          result.status = ClaimStatus::Violated;
          result.witness_round = round.round;
          result.detail = "transition count exceeded " + std::to_string(claim.limit);
          break;
        }
      }
      if (result.status != ClaimStatus::Violated) {
        result.status = trace.closed ? ClaimStatus::Satisfied : ClaimStatus::Pending;
        result.detail = trace.closed ? "closed trace stayed within count bound"
                                     : "open trace may still exceed count bound";
      }
    } else {
      bool witness = false;
      for (const ParallelStepResult& round : trace.rounds) {
        Environment environment{round.state, nullptr, {}, round.round};
        const bool value = evaluate(claim.predicate, environment).as_bool();
        const bool decisive = claim.kind == ClaimDeclaration::Kind::Always ? !value : value;
        if (!decisive) continue;
        witness = true;
        result.witness_round = round.round;
        result.status = claim.kind == ClaimDeclaration::Kind::Always
                            ? ClaimStatus::Violated
                            : ClaimStatus::Satisfied;
        result.detail = claim.kind == ClaimDeclaration::Kind::Always
                            ? "predicate is false"
                            : "predicate became true";
        break;
      }
      if (!witness) {
        if (!trace.closed) {
          result.status = ClaimStatus::Pending;
          result.detail = "open trace has no decisive witness yet";
        } else if (claim.kind == ClaimDeclaration::Kind::Always) {
          result.status = ClaimStatus::Satisfied;
          result.detail = "predicate holds throughout the closed trace";
        } else {
          result.status = ClaimStatus::Violated;
          result.detail = "closed trace ended without a witness";
        }
      }
    }
    if (!trace.causal_gaps.empty() && result.status == ClaimStatus::Satisfied) {
      result.status = ClaimStatus::Pending;
      result.detail = "projected trace has causal gaps; satisfaction is not conclusive";
    }
    results.push_back(std::move(result));
  }
  return results;
}

}  // namespace

TraceSnapshot Engine::captured_trace(std::string_view name, bool close) const {
  const auto found = captured_traces_.find(name);
  if (found == captured_traces_.end()) {
    throw Error("unknown dynamic trace '" + std::string(name) + "'");
  }
  TraceSnapshot result = found->second;
  if (close) result.closed = true;
  return result;
}

std::vector<ClaimEvaluation> Engine::evaluate_claims(
    std::string_view trace_name, bool close) const {
  return evaluate_trace_claims(*program_.implementation(), captured_trace(trace_name, close));
}

std::vector<ClaimEvaluation> evaluate_named_trace(
    const Program& program, std::string_view name) {
  return evaluate_trace_claims(*program.implementation(), run_named_trace(program, name));
}

const ProcedureTraceFrame& captured_procedure_at(
    const TraceSnapshot& trace, std::string_view procedure,
    std::uint64_t round_id) {
  const auto found = trace.procedure_history.find(procedure);
  if (found == trace.procedure_history.end()) {
    throw Error("trace did not capture procedure '" + std::string(procedure) + "'");
  }
  const auto frame = std::lower_bound(
      found->second.begin(), found->second.end(), round_id,
      [](const ProcedureTraceFrame& item, std::uint64_t value) {
        return item.round < value;
      });
  if (frame == found->second.end() || frame->round != round_id) {
    throw Error("captured procedure has no frame at RoundId " +
                std::to_string(round_id));
  }
  return *frame;
}

std::string_view claim_status_name(ClaimStatus status) noexcept {
  switch (status) {
    case ClaimStatus::Satisfied: return "satisfied";
    case ClaimStatus::Violated: return "violated";
    case ClaimStatus::Pending: return "pending";
  }
  return "unknown";
}
