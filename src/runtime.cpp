// Execution half of the implementation. It is included once by frontend.cpp
// so the private typed AST remains hidden without creating a second public IR.
// All Engine/RuntimeContext round, dispatch, replay and capture behavior lives
// here; parsing and verification remain in frontend.cpp.

namespace {

#include "capture_semantics.cpp"

TraceArtifact make_trace_artifact(
    const std::vector<ParallelStepResult>& rounds,
    std::uint64_t last_round,
    std::map<std::string, std::string, std::less<>> procedure_contexts = {}) {
  TraceArtifact artifact;
  artifact.procedure_contexts = std::move(procedure_contexts);
  for (const ParallelStepResult& round : rounds) {
    if (round.round > last_round) break;
    TraceArtifactRound saved;
    saved.round = round.round;
    for (const StepResult& step : round.transitions) {
      saved.inputs.push_back(step.input);
    }
    saved.expected = round;
    if (!saved.inputs.empty()) artifact.rounds.push_back(std::move(saved));
  }
  return artifact;
}

}  // namespace

Engine::Engine(Program program)
    : Engine(std::move(program), {}, SolverEncoding::DenseIds) {}

Engine::Engine(Program program, SolverEncoding encoding)
    : Engine(std::move(program), {}, encoding) {}

Engine::Engine(Program program,
               std::map<std::string, std::string, std::less<>> initial_states)
    : Engine(std::move(program), std::move(initial_states),
             SolverEncoding::DenseIds) {}

Engine::Engine(Program program,
               std::map<std::string, std::string, std::less<>> initial_states,
               SolverEncoding encoding)
    : program_(std::move(program)), encoding_(encoding) {
  if (program_.empty()) throw Error("cannot construct an engine from an empty program");
  const Program::Impl& implementation = *program_.implementation();
  active_state_ids_.assign(implementation.context_names.size(), invalid_dense_id);
  FunctionScope function_scope(implementation.functions, &implementation.relations,
                               &implementation.types);
  const std::size_t initial_count = static_cast<std::size_t>(std::count_if(
      implementation.states.begin(), implementation.states.end(),
      [](const State& state) { return state.initial; }));
  for (const State& state : implementation.states) {
    if (!state.initial) continue;
    active_states_.emplace(state.context, state.name);
    active_state_ids_.at(state.context_id) = state.state_id;
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
    active_state_ids_.at(state.context_id) = state.state_id;
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
      snapshot.captured_states.emplace(binding.context, binding.state);
    }
    snapshot.captured_paths = trace.paths;
    snapshot.captured_procedures = trace.captured_procedures;
    snapshot.temporal_rule = trace.temporal_rule;
    if (trace.capture_mode == TraceCaptureMode::Projected) {
      snapshot.causal_gaps.push_back(
          "projected capture omits decisions outside selected @ contexts");
    }
    captured_traces_.emplace(trace.name, std::move(snapshot));
  }
}

Engine Engine::from_procedure(Program program, std::string_view procedure_name,
                              SolverEncoding encoding) {
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
  Engine result(std::move(program), std::move(initial_states), encoding);
  result.procedure_name_ = std::string(procedure_name);
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

ParallelStepResult Engine::step_occurrences_at(
    const std::vector<OccurrenceInput>& occurrences,
    std::uint64_t round_id) {
  if (occurrences.empty()) throw Error("a replay round needs at least one occurrence");
  const Program::Impl& program = *program_.implementation();
  std::vector<std::pair<Event, std::string>> inputs;
  inputs.reserve(occurrences.size());
  for (const OccurrenceInput& occurrence : occurrences) {
    if (!occurrence.target_procedure.empty() &&
        occurrence.target_procedure != procedure_name_) {
      throw Error("occurrence targets another procedure instance");
    }
    if (!occurrence.target_context.empty() &&
        occurrence.target_context != initial_context_) {
      throw Error("occurrence target context does not match the Engine entry");
    }
    if (occurrence.kind == OccurrenceInputKind::Event) {
      if (occurrence.event.empty() || occurrence.symbol != occurrence.event) {
        throw Error("stored Event occurrence has inconsistent identity");
      }
      inputs.emplace_back(Event{occurrence.event, occurrence.fields},
                          std::string{});
      continue;
    }
    const auto indexed = program.transition_index.find(occurrence.symbol);
    if (indexed == program.transition_index.end()) {
      throw Error("stored occurrence references unknown transition '" +
                  occurrence.symbol + "'");
    }
    const Transition& transition = program.transitions[indexed->second];
    if (transition.event != occurrence.event) {
      throw Error("stored Transition occurrence has inconsistent Event identity");
    }
    inputs.emplace_back(Event{occurrence.event, occurrence.fields},
                        occurrence.symbol);
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
  FunctionScope function_scope(program.functions, &program.relations, &program.types);
  struct Prepared {
    const Transition* transition;
    const std::string* case_name;
    const std::vector<StateBinding>* from;
    const std::vector<TransitionTarget>* to;
    const std::set<std::string, std::less<>>* read_set;
    const std::set<std::string, std::less<>>* write_set;
    std::map<std::string, Value, std::less<>> writes;
    std::map<std::string, std::string, std::less<>> targets;
    std::vector<std::pair<ContextId, StateId>> dense_targets;
    ActionPlan actions;
    std::set<std::string, std::less<>> causal_predecessors;
    std::string optimization_scope;
    std::optional<Value> optimized_score;
    OccurrenceInput input;
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
      const std::string* lexical_context;
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
      if (!transition.procedure_scope.empty() &&
          transition.procedure_scope != procedure_name_) {
        continue;
      }
      struct Route {
        const std::vector<StateBinding>* from;
        const std::string* case_name;
        const std::vector<TransitionTarget>* to;
        const ExprPtr* condition;
        const std::shared_ptr<ActionExpr>* action;
        const std::set<std::string, std::less<>>* reads;
        const std::set<std::string, std::less<>>* writes;
        const std::string* lexical_context;
      };
      const auto route_at = [&](RouteId route_id) {
        if (route_id == 0U) {
          return Route{&transition.from, &transition.case_name, &transition.to,
                       &transition.condition, &transition.action,
                       &transition.reads, &transition.writes,
                       &transition.route_context};
        }
        const TransitionAlternative& alternative =
            transition.alternatives.at(route_id - 1U);
        return Route{&alternative.from, &alternative.name, &alternative.to,
                     &alternative.condition, &alternative.action,
                     &alternative.reads, &alternative.writes,
                     &alternative.lexical_context};
      };
      validate_event(transition, event, program.types);
      std::vector<RouteId> candidate_routes;
      if (encoding_ == SolverEncoding::DenseIds) {
        for (const DenseRouteStateIndexBucket& bucket :
             program.dense_route_state_index.at(candidate_index)) {
          std::vector<StateId> signature;
          signature.reserve(bucket.contexts.size());
          bool complete = true;
          for (const ContextId context_id : bucket.contexts) {
            const StateId active = active_state_ids_.at(context_id);
            if (active == invalid_dense_id) {
              complete = false;
              break;
            }
            signature.push_back(active);
          }
          if (!complete) continue;
          const auto indexed_routes = bucket.routes.find(signature);
          if (indexed_routes != bucket.routes.end()) {
            candidate_routes.insert(candidate_routes.end(),
                                    indexed_routes->second.begin(),
                                    indexed_routes->second.end());
          }
        }
      } else {
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
      }
      for (const RouteId route_index : candidate_routes) {
        const Route route = route_at(route_index);
        Environment environment{values_, &event, {}, round_id - 1U, nullptr,
                                *route.lexical_context};
        const std::string path = transition.name +
            (route.case_name->empty() ? "" : "." + *route.case_name);
        const PropertyDecision decision = evaluate_instant_property(
            *route.condition, environment,
            PropertyUse{path + ".where", PropertyScope::Transition,
                        PropertyTrigger::Candidate, PropertyFailure::Disable});
        if (decision.disposition == PropertyDisposition::Admit) {
          enabled.push_back(Enabled{&transition, route.case_name, route.from, route.to,
                                    route.condition, route.action,
                                    route.reads, route.writes,
                                    route.lexical_context});
        } else if (decision.disposition != PropertyDisposition::Disable) {
          throw Error("invalid transition guard Property disposition");
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
      Environment environment{values_, &event, {}, round_id - 1U, nullptr,
                              *candidate.lexical_context};
      Prepared decision{&transition, candidate.case_name, candidate.from, candidate.to,
                        candidate.reads, candidate.writes, {}, {}, {}, {}, {}, {}, {}, {}};
      decision.input.kind = injected_transition.empty()
                                ? OccurrenceInputKind::Event
                                : OccurrenceInputKind::Transition;
      decision.input.symbol = injected_transition.empty()
                                  ? event.name
                                  : injected_transition;
      decision.input.event = event.name;
      decision.input.fields = event.fields;
      decision.input.target_procedure = procedure_name_;
      decision.input.target_context = initial_context_;
      for (const StateBinding& binding : *candidate.from) {
        const auto writers = last_state_writers_.find(binding.context);
        if (writers != last_state_writers_.end()) {
          decision.causal_predecessors.insert(writers->second.begin(),
                                              writers->second.end());
        }
      }
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
        decision.dense_targets.emplace_back(target.binding.context_id,
                                            target.binding.state_id);
        if (active_state_ids_.at(target.binding.context_id) != target.binding.state_id) {
          for (const auto& [field, value] : initial_values(target_state)) {
            decision.writes.insert_or_assign(
                single_context ? field : state_key(target.binding.context, field), value);
          }
        }
        for (const Assignment& assignment : target.assignments) {
          Value value = evaluate(assignment.value, environment);
          const Field& field = find_field(target_state, assignment.field);
          if (!value_matches_base_type(value, field.type, program.types)) {
            throw Error("assignment to '" + assignment.field + "' has the wrong type");
          }
          if (!static_type_constraint_accepts(value, field.type, program.types)) {
            throw Error("assignment to '" + assignment.field +
                        "' leaves its finite domain");
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

  std::vector<StateId> next_active_ids = active_state_ids_;
  std::vector<StateId> round_targets(active_state_ids_.size(), invalid_dense_id);
  for (const Prepared& decision : prepared) {
    for (const auto [context_id, state_id] : decision.dense_targets) {
      StateId& selected = round_targets.at(context_id);
      if (selected != invalid_dense_id && selected != state_id) {
        throw Error("parallel transitions choose different states for @" +
                    program.context_names.at(context_id));
      }
      selected = state_id;
      next_active_ids.at(context_id) = state_id;
    }
  }
  std::map<std::string, std::string, std::less<>> next_active = active_states_;
  for (ContextId context_id = 0; context_id < round_targets.size(); ++context_id) {
    if (round_targets[context_id] == invalid_dense_id) continue;
    next_active.insert_or_assign(program.context_names.at(context_id),
                                 find_state(program, round_targets[context_id]).name);
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
  const Embedding before_embedding{active_states_, values_};
  const Embedding after_embedding{next_active, next};
  const EmbeddingDelta round_delta =
      diff_embedding(before_embedding, after_embedding);
  const std::string before_digest = embedding_digest(before_embedding);
  const std::string after_digest = embedding_digest(after_embedding);
  if (apply_embedding_delta(before_embedding, round_delta) != after_embedding) {
    throw Error("internal EmbeddingDelta round reconstruction failed");
  }
  result.transitions.reserve(prepared.size());
  for (std::size_t index = 0; index < prepared.size(); ++index) {
    Prepared& decision = prepared[index];
    StepResult step_result;
    step_result.round = result.round;
    step_result.id = "r" + std::to_string(result.round) + ":" + std::to_string(index);
    step_result.case_name = *decision.case_name;
    step_result.transition_family = decision.transition->name;
    step_result.transition = step_result.transition_family;
    if (!step_result.case_name.empty()) step_result.transition += "." + step_result.case_name;
    step_result.input = std::move(decision.input);
    step_result.optimization_scope = std::move(decision.optimization_scope);
    step_result.optimized_score = std::move(decision.optimized_score);
    step_result.from_state = binding_set_text(*decision.from);
    step_result.to_state = target_set_text(*decision.to);
    step_result.before_active_states = active_states_;
    step_result.active_states = next_active;
    step_result.before_state = values_;
    step_result.state = next;
    step_result.before_embedding_digest = before_digest;
    step_result.embedding_digest = after_digest;
    step_result.delta = round_delta;
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
  for (const Prepared& decision : prepared) {
    for (const auto& [context, state] : decision.targets) {
      static_cast<void>(state);
      last_state_writers_[context].clear();
    }
  }
  for (std::size_t index = 0; index < prepared.size(); ++index) {
    for (const auto& [context, state] : prepared[index].targets) {
      static_cast<void>(state);
      last_state_writers_[context].insert(result.transitions[index].id);
    }
  }

  round_ = round_id;
  active_states_ = std::move(next_active);
  active_state_ids_ = std::move(next_active_ids);
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
    ParallelStepResult captured = result;
    if (trace.mode == TraceCaptureMode::Projected && !trace.temporal_rule) {
      captured.transitions.erase(
          std::remove_if(captured.transitions.begin(), captured.transitions.end(),
                         [&](const StepResult& step_result) {
                           return !capture_relation_matches(
                               trace, step_result, procedure_name_);
                         }),
          captured.transitions.end());
      if (captured.transitions.empty()) continue;
    }
    for (auto it = captured.state.begin(); it != captured.state.end();) {
      if (!belongs(it->first)) it = captured.state.erase(it);
      else ++it;
    }
    for (StepResult& step_result : captured.transitions) {
      for (auto it = step_result.before_state.begin();
           it != step_result.before_state.end();) {
        if (!belongs(it->first)) it = step_result.before_state.erase(it);
        else ++it;
      }
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
    frame.before_active_states = frame.active_states;
    frame.before_state = frame.state;
    for (const StepResult& step : aggregate.transitions) {
      if (step.procedure == procedure) {
        if (frame.inputs.empty()) {
          frame.before_active_states = step.before_active_states;
          frame.before_state = step.before_state;
        }
        frame.transitions.push_back(step.transition);
        frame.inputs.push_back(step.input);
      }
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
  if (!result.rounds.empty()) {
    result.final_state = result.rounds.back().state;
    result.trace_artifact = make_trace_artifact(
        result.rounds, result.rounds.back().round, result.procedure_contexts);
  }
  result.replayable = result.trace_artifact.has_value() &&
      !result.trace_artifact->rounds.empty();

  const auto declaration = std::find_if(
      impl_->program.implementation()->traces.begin(),
      impl_->program.implementation()->traces.end(),
      [&](const TraceDeclaration& trace) { return trace.name == name; });
  if (declaration == impl_->program.implementation()->traces.end() ||
      !declaration->has_capture) {
    return result;
  }

  result.root_context = declaration->root_context;
  result.mode = declaration->capture_mode;
  result.captured_contexts.clear();
  result.captured_states.clear();
  result.captured_paths = declaration->paths;
  result.captured_procedures = declaration->captured_procedures;
  result.temporal_rule = declaration->temporal_rule;
  result.trace_artifact.reset();
  result.replayable = false;
  for (const StateBinding& binding : declaration->capture) {
    result.captured_contexts.insert(binding.context);
    result.captured_states.emplace(binding.context, binding.state);
  }
  const TraceSnapshot capture_seeds = result;
  const CaptureSelection selection = select_capture_occurrences(
      capture_seeds, result.rounds,
      declaration->capture_mode == TraceCaptureMode::Closed, true);
  result.temporal_intervals = selection.intervals;
  result.captured_procedures = selection.procedures;
  if (declaration->capture_mode == TraceCaptureMode::Closed &&
      !selection.rounds.empty()) {
    std::map<std::string, std::string, std::less<>> artifact_contexts;
    for (const auto& [procedure, engine] : impl_->engines) {
      artifact_contexts.emplace(procedure, engine->initial_context());
    }
    result.trace_artifact = make_trace_artifact(
        result.rounds, *selection.rounds.rbegin(), std::move(artifact_contexts));
  }

  for (auto round = result.rounds.begin(); round != result.rounds.end();) {
    round->transitions.erase(
        std::remove_if(
            round->transitions.begin(), round->transitions.end(),
            [&](const StepResult& step) {
              return !selection.occurrences.contains(step.id);
            }),
        round->transitions.end());
    if (round->transitions.empty()) {
      round = result.rounds.erase(round);
      continue;
    }
    if (impl_->engines.size() > 1U) {
      for (auto field = round->state.begin(); field != round->state.end();) {
        if (!capture_qualified_field_selected(selection.procedures,
                                              field->first)) {
          field = round->state.erase(field);
        } else {
          ++field;
        }
      }
    }
    for (StepResult& step : round->transitions) {
      const auto local_selected = [&](std::string_view field) {
        return declaration->capture_mode == TraceCaptureMode::Closed ||
            result.captured_contexts.empty() ||
            std::any_of(result.captured_contexts.begin(),
                        result.captured_contexts.end(),
                        [&](const std::string& context) {
                          return field.starts_with(context + ".") ||
                                 field.find('.') == std::string_view::npos;
                        });
      };
      for (auto field = step.before_state.begin();
           field != step.before_state.end();) {
        if (!local_selected(field->first)) field = step.before_state.erase(field);
        else ++field;
      }
      for (auto field = step.state.begin(); field != step.state.end();) {
        if (!local_selected(field->first)) field = step.state.erase(field);
        else ++field;
      }
    }
    ++round;
  }
  if (result.rounds.empty()) {
    result.final_state.clear();
  } else {
    result.final_state = result.rounds.back().state;
  }
  if (!selection.rounds.empty()) {
    const std::uint64_t last_round = *selection.rounds.rbegin();
    for (const std::string& procedure : selection.procedures) {
      const auto history = result.procedure_history.find(procedure);
      if (history == result.procedure_history.end()) continue;
      const auto frame = std::find_if(
          history->second.begin(), history->second.end(),
          [&](const ProcedureTraceFrame& item) {
            return item.round == last_round;
          });
      if (frame != history->second.end()) {
        result.procedure_states.insert_or_assign(procedure, frame->state);
      }
    }
  }
  if (impl_->engines.size() > 1U) {
    for (auto field = result.final_state.begin(); field != result.final_state.end();) {
      if (!capture_qualified_field_selected(selection.procedures,
                                            field->first)) {
        field = result.final_state.erase(field);
      } else {
        ++field;
      }
    }
  }
  for (auto state = result.procedure_states.begin();
       state != result.procedure_states.end();) {
    if (!capture_procedure_selected(selection.procedures, state->first)) {
      state = result.procedure_states.erase(state);
    } else {
      ++state;
    }
  }
  for (auto context = result.procedure_contexts.begin();
       context != result.procedure_contexts.end();) {
    if (!capture_procedure_selected(selection.procedures, context->first)) {
      context = result.procedure_contexts.erase(context);
    } else {
      ++context;
    }
  }
  for (auto history = result.procedure_history.begin();
       history != result.procedure_history.end();) {
    if (!capture_procedure_selected(selection.procedures, history->first)) {
      history = result.procedure_history.erase(history);
    } else {
      history->second.erase(
          std::remove_if(history->second.begin(), history->second.end(),
                         [&](const ProcedureTraceFrame& frame) {
                           return !selection.rounds.contains(frame.round);
                         }),
          history->second.end());
      ++history;
    }
  }
  for (auto artifact = result.procedure_artifacts.begin();
       artifact != result.procedure_artifacts.end();) {
    if (!capture_procedure_selected(selection.procedures, artifact->first)) {
      artifact = result.procedure_artifacts.erase(artifact);
    } else {
      const std::uint64_t last_round = selection.rounds.empty()
          ? 0U
          : *selection.rounds.rbegin();
      artifact->second.injections.erase(
          std::remove_if(
              artifact->second.injections.begin(),
              artifact->second.injections.end(),
              [&](const ProcedureInjection& injection) {
                return injection.round > last_round;
              }),
          artifact->second.injections.end());
      ++artifact;
    }
  }
  if (declaration->capture_mode == TraceCaptureMode::Projected) {
    result.replayable = false;
    result.trace_artifact.reset();
    result.procedure_history.clear();
    result.procedure_artifacts.clear();
    result.causal_gaps.push_back(
        "projected capture omits decisions outside the capture relation");
  } else {
    result.replayable = result.trace_artifact.has_value() &&
                        !result.trace_artifact->rounds.empty();
  }
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
        throw Error("procedure artifact initial embedding does not match '" +
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

TraceSnapshot replay_trace_artifact(
    const Program& program, const TraceArtifact& artifact) {
  if (program.empty()) throw Error("cannot replay a trace artifact without a Program");
  if (artifact.rounds.empty()) throw Error("trace artifact has no rounds");
  const auto execute = [&]() {
    bool has_free = false;
    bool has_procedure = !artifact.procedure_contexts.empty();
    std::set<std::string, std::less<>> procedures;
    for (const auto& [procedure, context] : artifact.procedure_contexts) {
      const auto declaration =
          program.implementation()->procedures.find(procedure);
      if (declaration == program.implementation()->procedures.end()) {
        throw Error("trace artifact targets unknown procedure '" + procedure + "'");
      }
      if (declaration->second.initial_context != context) {
        throw Error("trace artifact procedure context does not match declaration");
      }
      procedures.insert(procedure);
    }
    std::uint64_t previous_round = 0U;
    for (const TraceArtifactRound& round : artifact.rounds) {
      if (round.round == 0U || round.round <= previous_round ||
          round.inputs.empty()) {
        throw Error("trace artifact RoundId sequence is invalid");
      }
      previous_round = round.round;
      if (round.expected.round != round.round ||
          round.inputs.size() != round.expected.transitions.size()) {
        throw Error("trace artifact input/decision cardinality differs");
      }
      for (std::size_t index = 0; index < round.inputs.size(); ++index) {
        if (round.expected.transitions[index].input != round.inputs[index]) {
          throw Error("trace artifact input disagrees with expected decision");
        }
      }
      for (const OccurrenceInput& input : round.inputs) {
        if (input.target_procedure.empty()) {
          has_free = true;
        } else {
          has_procedure = true;
          procedures.insert(input.target_procedure);
          const auto declaration =
              program.implementation()->procedures.find(input.target_procedure);
          if (declaration == program.implementation()->procedures.end()) {
            throw Error("trace artifact targets unknown procedure '" +
                        input.target_procedure + "'");
          }
          if (declaration->second.initial_context != input.target_context) {
            throw Error("trace artifact procedure context does not match declaration");
          }
          const auto artifact_context =
              artifact.procedure_contexts.find(input.target_procedure);
          if (artifact_context != artifact.procedure_contexts.end() &&
              artifact_context->second != input.target_context) {
            throw Error("trace artifact input disagrees with procedure context table");
          }
          if (input.kind != OccurrenceInputKind::Transition) {
            throw Error("procedure trace artifact requires exact TransitionId inputs");
          }
        }
      }
    }
    if (has_free && has_procedure) {
      throw Error("trace artifact cannot mix a free Engine and procedure instances");
    }

    const auto verify_round = [](const ParallelStepResult& actual,
                                 const TraceArtifactRound& expected) {
      if (actual != expected.expected) {
        throw Error("trace artifact replay derived a different logical result at RoundId " +
                    std::to_string(expected.round));
      }
    };

    if (has_free) {
      Engine engine(program);
      TraceSnapshot result;
      result.name = "trace-artifact-replay";
      result.mode = TraceCaptureMode::Closed;
      result.closed = true;
      result.replayable = true;
      result.trace_artifact = artifact;
      for (const TraceArtifactRound& round : artifact.rounds) {
        ParallelStepResult actual = engine.step_occurrences_at(
            round.inputs, round.round);
        verify_round(actual, round);
        result.rounds.push_back(std::move(actual));
      }
      result.final_state = engine.values();
      return result;
    }

    RuntimeContext runtime(program);
    for (const std::string& procedure : procedures) runtime.start(procedure);
    for (const TraceArtifactRound& round : artifact.rounds) {
      std::vector<std::pair<std::string, TransitionInput>> inputs;
      for (const OccurrenceInput& occurrence : round.inputs) {
        inputs.emplace_back(
            occurrence.target_procedure,
            TransitionInput{occurrence.symbol, occurrence.fields});
      }
      const ParallelStepResult actual = runtime.inject_at(inputs, round.round);
      verify_round(actual, round);
    }
    TraceSnapshot result = runtime.snapshot("trace-artifact-replay");
    result.trace_artifact = artifact;
    result.replayable = true;
    return result;
  };
  const TraceSnapshot first = execute();
  const TraceSnapshot second = execute();
  if (first != second) throw Error("trace artifact replay diverged");
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
        frame.before_active_states = frame.active_states;
        frame.before_state = frame.state;
        for (const StepResult& step : aggregate.transitions) {
          if (step.procedure == procedure) {
            if (frame.inputs.empty()) {
              frame.before_active_states = step.before_active_states;
              frame.before_state = step.before_state;
            }
            frame.transitions.push_back(step.transition);
            frame.inputs.push_back(step.input);
          }
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
      execution.snapshot.procedure_states.emplace(procedure, engine->values());
      execution.snapshot.procedure_contexts.emplace(procedure, engine->initial_context());
    }
    for (const StateBinding& binding : found->capture) {
      execution.snapshot.captured_contexts.insert(binding.context);
      execution.snapshot.captured_states.emplace(binding.context,
                                                  binding.state);
    }
    execution.snapshot.captured_paths = found->paths;
    execution.snapshot.captured_procedures = found->captured_procedures;
    execution.snapshot.temporal_rule = found->temporal_rule;
    const TraceSnapshot capture_seeds = execution.snapshot;
    const CaptureSelection selection = select_capture_occurrences(
        capture_seeds, execution.logical.rounds,
        !found->has_capture || found->capture_mode == TraceCaptureMode::Closed,
        true);
    execution.snapshot.temporal_intervals = selection.intervals;
    execution.snapshot.captured_procedures = selection.procedures;
    if ((!found->has_capture ||
         found->capture_mode == TraceCaptureMode::Closed) &&
        !selection.rounds.empty()) {
      execution.snapshot.trace_artifact = make_trace_artifact(
          execution.logical.rounds, *selection.rounds.rbegin(),
          execution.snapshot.procedure_contexts);
    }
    if (!found->has_capture || found->capture_mode == TraceCaptureMode::Closed) {
      const std::uint64_t last_round = selection.rounds.empty()
          ? 0U
          : *selection.rounds.rbegin();
      for (const std::string& procedure : selection.procedures) {
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
          if (round_index + 1U > last_round) break;
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
          execution.snapshot.trace_artifact.has_value() &&
          !execution.snapshot.trace_artifact->rounds.empty();
      for (auto history = execution.snapshot.procedure_history.begin();
           history != execution.snapshot.procedure_history.end();) {
        if (!selection.procedures.contains(history->first)) {
          history = execution.snapshot.procedure_history.erase(history);
        } else {
          history->second.erase(
              std::remove_if(history->second.begin(), history->second.end(),
                             [&](const ProcedureTraceFrame& frame) {
                               return !selection.rounds.contains(frame.round);
                             }),
              history->second.end());
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
        if (single_procedure &&
            execution.snapshot.captured_procedures.contains(
                *referenced_procedures.begin()) &&
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
      const auto procedure_selected = [&](const StepResult& step) {
        return execution.snapshot.captured_procedures.contains(step.procedure);
      };
      if (found->capture_mode == TraceCaptureMode::Projected) {
        execution.snapshot.replayable = false;
        execution.snapshot.trace_artifact.reset();
        execution.snapshot.procedure_artifacts.clear();
        execution.snapshot.causal_gaps.push_back(
            "projected capture omits decisions outside selected procedures, paths, or contexts");
      }
      for (auto round = execution.snapshot.rounds.begin();
           round != execution.snapshot.rounds.end();) {
        round->transitions.erase(
            std::remove_if(
                round->transitions.begin(), round->transitions.end(),
                [&](const StepResult& step) {
                  return !selection.occurrences.contains(step.id);
                }),
            round->transitions.end());
        if (round->transitions.empty()) {
          round = execution.snapshot.rounds.erase(round);
          continue;
        }
        for (auto item = round->state.begin(); item != round->state.end();) {
          const bool selected = found->capture_mode == TraceCaptureMode::Closed
                                    ? field_procedure_selected(item->first)
                                    : belongs(item->first);
          if (!selected) item = round->state.erase(item);
          else ++item;
        }
        for (StepResult& step : round->transitions) {
          for (auto item = step.before_state.begin();
               item != step.before_state.end();) {
            const bool local_belongs = found->capture_mode == TraceCaptureMode::Closed ||
                execution.snapshot.captured_contexts.empty() ||
                std::any_of(
                    execution.snapshot.captured_contexts.begin(),
                    execution.snapshot.captured_contexts.end(),
                    [&](const std::string& context) {
                      return item->first.starts_with(context + ".") ||
                             item->first.find('.') == std::string::npos;
                    });
            if (!procedure_selected(step) || !local_belongs) {
              item = step.before_state.erase(item);
            } else {
              ++item;
            }
          }
          for (auto item = step.state.begin(); item != step.state.end();) {
            const bool local_belongs = found->capture_mode == TraceCaptureMode::Closed ||
                execution.snapshot.captured_contexts.empty() ||
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
      if (execution.snapshot.rounds.empty()) {
        execution.snapshot.final_state.clear();
      } else {
        execution.snapshot.final_state =
            execution.snapshot.rounds.back().state;
      }
      if (!selection.rounds.empty()) {
        const std::uint64_t last_round = *selection.rounds.rbegin();
        for (const std::string& procedure : selection.procedures) {
          const auto history = execution.snapshot.procedure_history.find(procedure);
          if (history == execution.snapshot.procedure_history.end()) continue;
          const auto frame = std::find_if(
              history->second.begin(), history->second.end(),
              [&](const ProcedureTraceFrame& item) {
                return item.round == last_round;
              });
          if (frame != history->second.end()) {
            execution.snapshot.procedure_states.insert_or_assign(
                procedure, frame->state);
          }
        }
      }
      for (auto item = execution.snapshot.final_state.begin();
           item != execution.snapshot.final_state.end();) {
        const bool selected = found->capture_mode == TraceCaptureMode::Closed
                                  ? field_procedure_selected(item->first)
                                  : belongs(item->first);
        if (!selected) item = execution.snapshot.final_state.erase(item);
        else ++item;
      }
      for (auto procedure = execution.snapshot.procedure_states.begin();
           procedure != execution.snapshot.procedure_states.end();) {
        if (!execution.snapshot.captured_procedures.contains(procedure->first)) {
          procedure = execution.snapshot.procedure_states.erase(procedure);
          continue;
        }
        auto& state = procedure->second;
        for (auto item = state.begin(); item != state.end();) {
          const bool local_belongs = found->capture_mode == TraceCaptureMode::Closed ||
              execution.snapshot.captured_contexts.empty() ||
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
        ++procedure;
      }
      for (auto context = execution.snapshot.procedure_contexts.begin();
           context != execution.snapshot.procedure_contexts.end();) {
        if (!execution.snapshot.captured_procedures.contains(context->first)) {
          context = execution.snapshot.procedure_contexts.erase(context);
        } else {
          ++context;
        }
      }
      if (found->capture_mode == TraceCaptureMode::Projected) {
        execution.snapshot.procedure_history.clear();
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

#include "trace_semantics.cpp"

PropertyScope property_scope(const ClaimDeclaration& claim) {
  switch (claim.target_kind) {
    case ClaimDeclaration::TargetKind::Trace: return PropertyScope::Trace;
    case ClaimDeclaration::TargetKind::State: return PropertyScope::State;
    case ClaimDeclaration::TargetKind::Procedure: return PropertyScope::Procedure;
    case ClaimDeclaration::TargetKind::Relation: return PropertyScope::Relation;
  }
  return PropertyScope::Trace;
}

PropertyTruth property_truth(ClaimStatus status) noexcept {
  switch (status) {
    case ClaimStatus::Satisfied: return PropertyTruth::Satisfied;
    case ClaimStatus::Violated: return PropertyTruth::Violated;
    case ClaimStatus::Pending: return PropertyTruth::Pending;
  }
  return PropertyTruth::Pending;
}

ClaimStatus claim_status(const PropertyDecision& decision) {
  switch (decision.disposition) {
    case PropertyDisposition::Admit: return ClaimStatus::Satisfied;
    case PropertyDisposition::RecordViolation:
    case PropertyDisposition::Counterexample: return ClaimStatus::Violated;
    case PropertyDisposition::Defer: return ClaimStatus::Pending;
    case PropertyDisposition::Disable:
    case PropertyDisposition::Reject:
      throw Error("Property disposition cannot be projected as ClaimStatus");
  }
  throw Error("invalid Property disposition");
}

std::vector<ClaimEvaluation> evaluate_trace_claims(
    const Program::Impl& program, const TraceSnapshot& trace) {
  FunctionScope function_scope(program.functions, &program.relations, &program.types);
  std::map<std::string, Value, std::less<>> initial_state;
  const auto declaration = std::find_if(
      program.traces.begin(), program.traces.end(),
      [&](const TraceDeclaration& item) { return item.name == trace.name; });
  std::set<std::string, std::less<>> procedures;
  if (declaration != program.traces.end()) {
    for (const auto& round : declaration->replay_procedures) {
      procedures.insert(round.begin(), round.end());
    }
  }
  if (procedures.empty()) {
    initial_state = Engine(Program(std::make_shared<Program::Impl>(program))).values();
  } else {
    const bool single = procedures.size() == 1U;
    for (const std::string& procedure : procedures) {
      const Engine initial = Engine::from_procedure(
          Program(std::make_shared<Program::Impl>(program)), procedure);
      for (const auto& [name, value] : initial.values()) {
        initial_state.emplace(procedure + "." + name, value);
        if (single) initial_state.emplace(name, value);
      }
    }
  }
  std::vector<TemporalFrame> frames;
  frames.reserve(trace.rounds.size() + 1U);
  frames.push_back(TemporalFrame{&initial_state, 0U});
  for (const ParallelStepResult& round : trace.rounds) {
    frames.push_back(TemporalFrame{&round.state, round.round});
  }
  std::vector<ClaimEvaluation> results;
  for (const ClaimDeclaration& claim : program.claims) {
    if (claim.target_kind != ClaimDeclaration::TargetKind::Trace ||
        claim.target != trace.name) continue;
    ClaimEvaluation result;
    result.name = claim.name;
    result.trace = trace.name;
    if (claim.count_at_most) {
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
      std::map<TemporalMemoKey, bool> memo;
      const bool value = evaluate_temporal(claim.property, frames, 0U, memo);
      bool decisive = trace.closed;
      if (!trace.closed && claim.property->kind == TemporalExpr::Kind::Always && !value) {
        decisive = true;
      } else if (!trace.closed &&
                 (claim.property->kind == TemporalExpr::Kind::Eventually ||
                  claim.property->kind == TemporalExpr::Kind::Within) && value) {
        decisive = true;
      } else if (!trace.closed &&
                 claim.property->kind == TemporalExpr::Kind::Within &&
                 frames.size() > claim.property->bound) {
        decisive = true;
      }
      if (!decisive) {
        result.status = ClaimStatus::Pending;
        result.detail = "open trace has no decisive temporal witness yet";
      } else {
        result.status = value ? ClaimStatus::Satisfied : ClaimStatus::Violated;
        result.witness_round = temporal_witness_round(claim.property, frames, value, memo);
        result.detail = value ? "temporal property holds on the typed trace"
                              : "temporal property has a finite counterexample";
      }
    }
    result.status = claim_status(decide_property(
        PropertyUse{claim.name, property_scope(claim), PropertyTrigger::Target,
                    PropertyFailure::Counterexample},
        property_truth(result.status)));
    if (!trace.causal_gaps.empty() && result.status == ClaimStatus::Satisfied) {
      result.status = ClaimStatus::Pending;
      result.detail = "projected trace has causal gaps; satisfaction is not conclusive";
    }
    results.push_back(std::move(result));
  }
  const auto evaluate_obligation = [&](std::string path,
                                       const TemporalExprPtr& property) {
    if (!property) return;
    ClaimEvaluation result;
    result.name = path + ".ensure";
    result.trace = trace.name;
    bool observed = false;
    bool all_hold = true;
    for (std::size_t round_index = 0; round_index < trace.rounds.size();
         ++round_index) {
      const ParallelStepResult& round = trace.rounds[round_index];
      const bool occurred = std::any_of(
          round.transitions.begin(), round.transitions.end(),
          [&](const StepResult& step) { return step.transition == path; });
      if (!occurred) continue;
      observed = true;
      std::map<TemporalMemoKey, bool> memo;
      if (!evaluate_temporal(property, frames, round_index + 1U, memo)) {
        all_hold = false;
        result.witness_round = round.round;
        break;
      }
    }
    if (!observed) return;
    if (!trace.closed) {
      result.status = ClaimStatus::Pending;
      result.detail = "open trace cannot close the transition temporal obligation";
    } else {
      result.status = all_hold ? ClaimStatus::Satisfied : ClaimStatus::Violated;
      result.detail = all_hold
                          ? "all selected transition occurrences satisfy ensure"
                          : "selected transition occurrence violates ensure";
    }
    result.status = claim_status(decide_property(
        PropertyUse{result.name, PropertyScope::Transition,
                    PropertyTrigger::Occurrence, PropertyFailure::Violation},
        property_truth(result.status)));
    if (!trace.causal_gaps.empty() && result.status == ClaimStatus::Satisfied) {
      result.status = ClaimStatus::Pending;
      result.detail = "projected trace has causal gaps; ensure is not conclusive";
    }
    results.push_back(std::move(result));
  };
  for (const Transition& transition : program.transitions) {
    evaluate_obligation(
        transition.name +
            (transition.case_name.empty() ? "" : "." + transition.case_name),
        transition.obligation);
    for (const TransitionAlternative& alternative : transition.alternatives) {
      evaluate_obligation(
          transition.name +
              (alternative.name.empty() ? "" : "." + alternative.name),
          alternative.obligation);
    }
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
  const CaptureSelection selection = select_capture_occurrences(
      result, result.rounds, result.mode == TraceCaptureMode::Closed, close);
  result.temporal_intervals = selection.intervals;
  result.captured_procedures = selection.procedures;
  if (result.mode == TraceCaptureMode::Closed && !selection.rounds.empty()) {
    result.trace_artifact = make_trace_artifact(
        result.rounds, *selection.rounds.rbegin());
    result.replayable = close && !result.trace_artifact->rounds.empty();
  }
  for (auto round = result.rounds.begin(); round != result.rounds.end();) {
    round->transitions.erase(
        std::remove_if(
            round->transitions.begin(), round->transitions.end(),
            [&](const StepResult& step) {
              return !selection.occurrences.contains(step.id);
            }),
        round->transitions.end());
    if (round->transitions.empty()) {
      round = result.rounds.erase(round);
    } else {
      ++round;
    }
  }
  if (!result.rounds.empty()) result.final_state = result.rounds.back().state;
  else result.final_state.clear();
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

const ParallelStepResult& captured_round_at(
    const TraceSnapshot& trace, std::uint64_t round_id) {
  const auto round = std::lower_bound(
      trace.rounds.begin(), trace.rounds.end(), round_id,
      [](const ParallelStepResult& item, std::uint64_t value) {
        return item.round < value;
      });
  if (round == trace.rounds.end() || round->round != round_id) {
    throw Error("trace has no captured RoundId " + std::to_string(round_id));
  }
  return *round;
}

std::string_view claim_status_name(ClaimStatus status) noexcept {
  switch (status) {
    case ClaimStatus::Satisfied: return "satisfied";
    case ClaimStatus::Violated: return "violated";
    case ClaimStatus::Pending: return "pending";
  }
  return "unknown";
}
