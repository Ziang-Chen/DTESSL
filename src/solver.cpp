// High-performance semantic search layer. This file is included once by
// frontend.cpp because the verified typed AST remains private. The Solver owns
// transition expansion and Claim counterexample search; runtime.cpp owns only
// execution, procedure persistence, replay and capture.

namespace {

Embedding embedding_of(const Engine& engine) {
  return Embedding{engine.current_states(), engine.values()};
}

bool unavailable_transition(const Error& error) {
  return std::string_view(error.what()).starts_with(
      "no transition accepts event '");
}

bool observes_round(const ExprPtr& expression) {
  if (!expression) return false;
  if (expression->kind == Expr::Kind::Name && expression->text == "round") {
    return true;
  }
  if (observes_round(expression->left) || observes_round(expression->right) ||
      observes_round(expression->third)) {
    return true;
  }
  if (std::any_of(expression->children.begin(), expression->children.end(),
                  observes_round)) {
    return true;
  }
  return std::any_of(expression->arms.begin(), expression->arms.end(),
                     [](const MatchArm& arm) {
                       return observes_round(arm.body);
                     });
}

bool observes_round(const TemporalExprPtr& expression) {
  if (!expression) return false;
  return observes_round(expression->atom) || observes_round(expression->left) ||
         observes_round(expression->right);
}

bool action_observes_round(const std::shared_ptr<ActionExpr>& action) {
  if (!action) return false;
  if (std::any_of(action->arguments.begin(), action->arguments.end(),
                  [](const ExprPtr& expression) {
                    return observes_round(expression);
                  })) {
    return true;
  }
  return std::any_of(action->children.begin(), action->children.end(),
                     action_observes_round);
}

bool targets_observe_round(const std::vector<TransitionTarget>& targets) {
  for (const TransitionTarget& target : targets) {
    for (const Assignment& assignment : target.assignments) {
      if (observes_round(assignment.value)) return true;
    }
  }
  return false;
}

bool program_observes_round(const Program::Impl& program) {
  for (const State& state : program.states) {
    if (std::any_of(state.invariants.begin(), state.invariants.end(),
                    [](const ExprPtr& expression) {
                      return observes_round(expression);
                    })) {
      return true;
    }
  }
  for (const Transition& transition : program.transitions) {
    if (observes_round(transition.condition) ||
        observes_round(transition.obligation) ||
        observes_round(transition.optimized_score) ||
        targets_observe_round(transition.to) ||
        action_observes_round(transition.action)) {
      return true;
    }
    for (const TransitionAlternative& alternative : transition.alternatives) {
      if (observes_round(alternative.condition) ||
          observes_round(alternative.obligation) ||
          targets_observe_round(alternative.to) ||
          action_observes_round(alternative.action)) {
        return true;
      }
    }
  }
  return false;
}

// EmbeddingExpand is the reachable graph produced by transition expansion. Its
// node index is the matching EmbeddingStore index; it is deliberately not
// a tree because transitions may join or form cycles.
struct EmbeddingExpand {
  using Edge = std::pair<std::size_t, std::string>;

  explicit EmbeddingExpand(std::size_t initial_nodes)
      : outgoing_edges(initial_nodes) {}

  void add_node() { outgoing_edges.emplace_back(); }
  void add_edge(std::size_t source, std::size_t target,
                std::string transition) {
    outgoing_edges.at(source).emplace_back(target, std::move(transition));
  }
  [[nodiscard]] const std::vector<Edge>& outgoing(std::size_t node) const {
    return outgoing_edges.at(node);
  }

  std::vector<std::vector<Edge>> outgoing_edges;
};

enum class MonitorGoal {
  Point,
  Always,
  Eventually,
  Until,
  Within,
  WeakUntil,
  CountAtMost,
};

enum class MonitorPhase : std::uint8_t { Waiting, Satisfied, Violated };

struct ClaimMonitorState {
  MonitorPhase phase{MonitorPhase::Waiting};
  std::uint64_t counter{0};
  std::vector<std::uint8_t> since_values;

  friend bool operator==(const ClaimMonitorState&, const ClaimMonitorState&) = default;
};

struct ClaimMonitorSpec {
  MonitorGoal goal{MonitorGoal::Point};
  // Owns the canonical temporal core produced from trace-relation syntax.
  TemporalExprPtr normalized_root;
  TemporalExprPtr left;
  TemporalExprPtr right;
  std::uint64_t bound{0};
  std::string counted_transition;
  std::map<const TemporalExpr*, std::size_t> since_slots;
};

bool same_temporal_tree(const TemporalExprPtr& left,
                        const TemporalExprPtr& right) {
  if (left == right) return true;
  if (!left || !right || left->kind != right->kind ||
      left->bound != right->bound || left->relation != right->relation) return false;
  if (left->kind == TemporalExpr::Kind::Atom) return left->atom == right->atom;
  return same_temporal_tree(left->left, right->left) &&
         same_temporal_tree(left->right, right->right);
}

TemporalExprPtr lower_trace_relation_matches(const TemporalExprPtr& expression) {
  if (!expression) return {};
  if (expression->kind == TemporalExpr::Kind::TraceRelationMatch) {
    if (expression->relation != "happens_before") {
      throw Error("unknown trace relation '" + expression->relation + "'");
    }
    TemporalExprPtr first = lower_trace_relation_matches(expression->left);
    TemporalExprPtr second = lower_trace_relation_matches(expression->right);
    // (first, second) ~ happens_before
    //   := until(not second, first and not second)
    return make_temporal_binary(
        TemporalExpr::Kind::Until,
        make_temporal_unary(TemporalExpr::Kind::Not, second),
        make_temporal_binary(
            TemporalExpr::Kind::And, std::move(first),
            make_temporal_unary(TemporalExpr::Kind::Not, std::move(second))));
  }
  if (expression->kind == TemporalExpr::Kind::Atom) return expression;
  auto result = std::make_shared<TemporalExpr>(*expression);
  result->left = lower_trace_relation_matches(expression->left);
  result->right = lower_trace_relation_matches(expression->right);
  return result;
}

bool is_past_formula(const TemporalExprPtr& expression) {
  if (!expression) return false;
  switch (expression->kind) {
    case TemporalExpr::Kind::Atom: return true;
    case TemporalExpr::Kind::TraceRelationMatch: return false;
    case TemporalExpr::Kind::Not:
      return is_past_formula(expression->left);
    case TemporalExpr::Kind::And:
    case TemporalExpr::Kind::Or:
    case TemporalExpr::Kind::Since:
      return is_past_formula(expression->left) &&
             is_past_formula(expression->right);
    case TemporalExpr::Kind::Always:
    case TemporalExpr::Kind::Eventually:
    case TemporalExpr::Kind::Until:
    case TemporalExpr::Kind::Within:
      return false;
  }
  return false;
}

void collect_since_slots(const TemporalExprPtr& expression,
                         ClaimMonitorSpec& spec) {
  if (!expression) return;
  if (expression->kind == TemporalExpr::Kind::Since &&
      !spec.since_slots.contains(expression.get())) {
    spec.since_slots.emplace(expression.get(), spec.since_slots.size());
  }
  collect_since_slots(expression->left, spec);
  collect_since_slots(expression->right, spec);
}

std::optional<ClaimMonitorSpec> compile_claim_monitor(
    const ClaimDeclaration& claim) {
  ClaimMonitorSpec spec;
  if (claim.count_at_most) {
    spec.goal = MonitorGoal::CountAtMost;
    spec.bound = claim.limit;
    spec.counted_transition = claim.transition;
    return spec;
  }
  const TemporalExprPtr property = lower_trace_relation_matches(claim.property);
  if (!property) return std::nullopt;
  spec.normalized_root = property;
  if (property->kind == TemporalExpr::Kind::Always &&
      is_past_formula(property->left)) {
    spec.goal = MonitorGoal::Always;
    spec.left = property->left;
  } else if (property->kind == TemporalExpr::Kind::Eventually &&
             is_past_formula(property->left)) {
    spec.goal = MonitorGoal::Eventually;
    spec.left = property->left;
  } else if (property->kind == TemporalExpr::Kind::Until &&
             is_past_formula(property->left) &&
             is_past_formula(property->right)) {
    spec.goal = MonitorGoal::Until;
    spec.left = property->left;
    spec.right = property->right;
  } else if (property->kind == TemporalExpr::Kind::Within &&
             is_past_formula(property->left)) {
    spec.goal = MonitorGoal::Within;
    spec.left = property->left;
    spec.bound = property->bound;
  } else if (property->kind == TemporalExpr::Kind::Or && property->left &&
             property->right &&
             property->left->kind == TemporalExpr::Kind::Until &&
             property->right->kind == TemporalExpr::Kind::Always &&
             same_temporal_tree(property->left->left,
                                property->right->left) &&
             is_past_formula(property->left->left) &&
             is_past_formula(property->left->right)) {
    spec.goal = MonitorGoal::WeakUntil;
    spec.left = property->left->left;
    spec.right = property->left->right;
  } else if (is_past_formula(property)) {
    spec.goal = MonitorGoal::Point;
    spec.left = property;
  } else {
    return std::nullopt;
  }
  collect_since_slots(spec.left, spec);
  collect_since_slots(spec.right, spec);
  return spec;
}

bool evaluate_past_formula(
    const TemporalExprPtr& expression, const Engine& engine,
    const ClaimMonitorSpec& spec, const ClaimMonitorState& previous,
    ClaimMonitorState& current,
    std::map<const TemporalExpr*, bool>& memo) {
  if (!expression) return false;
  if (const auto found = memo.find(expression.get()); found != memo.end()) {
    return found->second;
  }
  bool result = false;
  switch (expression->kind) {
    case TemporalExpr::Kind::Atom: {
      Environment environment{engine.values(), nullptr, {}, engine.current_round()};
      result = evaluate(expression->atom, environment).as_bool();
      break;
    }
    case TemporalExpr::Kind::Not:
      result = !evaluate_past_formula(expression->left, engine, spec, previous,
                                      current, memo);
      break;
    case TemporalExpr::Kind::And:
      result = evaluate_past_formula(expression->left, engine, spec, previous,
                                     current, memo) &&
               evaluate_past_formula(expression->right, engine, spec, previous,
                                     current, memo);
      break;
    case TemporalExpr::Kind::Or:
      result = evaluate_past_formula(expression->left, engine, spec, previous,
                                     current, memo) ||
               evaluate_past_formula(expression->right, engine, spec, previous,
                                     current, memo);
      break;
    case TemporalExpr::Kind::Since: {
      const std::size_t slot = spec.since_slots.at(expression.get());
      const bool origin = evaluate_past_formula(
          expression->right, engine, spec, previous, current, memo);
      const bool hold = evaluate_past_formula(
          expression->left, engine, spec, previous, current, memo);
      result = origin || (hold && previous.since_values.at(slot) != 0U);
      current.since_values.at(slot) = result ? 1U : 0U;
      break;
    }
    case TemporalExpr::Kind::Always:
    case TemporalExpr::Kind::Eventually:
    case TemporalExpr::Kind::Until:
    case TemporalExpr::Kind::Within:
    case TemporalExpr::Kind::TraceRelationMatch:
      throw Error("future temporal operator reached a past monitor");
  }
  memo.emplace(expression.get(), result);
  return result;
}

bool transition_matches(std::string_view selected, std::string_view expected) {
  return selected == expected ||
         (expected.find('.') == std::string_view::npos &&
          selected.starts_with(std::string(expected) + "."));
}

ClaimMonitorState advance_monitor(const ClaimMonitorSpec& spec,
                                  const ClaimMonitorState& previous,
                                  const Engine& engine,
                                  std::string_view transition = {}) {
  ClaimMonitorState current = previous;
  current.since_values.assign(spec.since_slots.size(), 0U);
  if (previous.phase != MonitorPhase::Waiting) return current;
  if (spec.goal == MonitorGoal::CountAtMost) {
    if (!transition.empty() &&
        transition_matches(transition, spec.counted_transition)) {
      if (current.counter >= spec.bound) {
        current.phase = MonitorPhase::Violated;
      } else {
        ++current.counter;
      }
    }
    return current;
  }
  std::map<const TemporalExpr*, bool> memo;
  const bool left = evaluate_past_formula(spec.left, engine, spec, previous,
                                          current, memo);
  const auto right_value = [&]() {
    return evaluate_past_formula(spec.right, engine, spec, previous, current,
                                 memo);
  };
  switch (spec.goal) {
    case MonitorGoal::Point:
      current.phase = left ? MonitorPhase::Satisfied : MonitorPhase::Violated;
      break;
    case MonitorGoal::Always:
      if (!left) current.phase = MonitorPhase::Violated;
      break;
    case MonitorGoal::Eventually:
      if (left) current.phase = MonitorPhase::Satisfied;
      break;
    case MonitorGoal::Until:
      if (right_value()) current.phase = MonitorPhase::Satisfied;
      else if (!left) current.phase = MonitorPhase::Violated;
      break;
    case MonitorGoal::Within:
      if (left) current.phase = MonitorPhase::Satisfied;
      else if (current.counter >= spec.bound) current.phase = MonitorPhase::Violated;
      else ++current.counter;
      break;
    case MonitorGoal::WeakUntil:
      if (right_value()) current.phase = MonitorPhase::Satisfied;
      else if (!left) current.phase = MonitorPhase::Violated;
      break;
    case MonitorGoal::CountAtMost: break;
  }
  return current;
}

std::string monitor_key(const ClaimMonitorState& state) {
  std::string result;
  result.reserve(32U + state.since_values.size());
  result += std::to_string(static_cast<unsigned>(state.phase));
  result += ':';
  result += std::to_string(state.counter);
  result += ':';
  for (const std::uint8_t value : state.since_values) {
    result.push_back(value == 0U ? '0' : '1');
  }
  return result;
}

bool needs_acceptance_cycle(MonitorGoal goal) {
  return goal == MonitorGoal::Eventually || goal == MonitorGoal::Until;
}

bool terminal_satisfaction(MonitorGoal goal) {
  return goal == MonitorGoal::Point || goal == MonitorGoal::Eventually ||
         goal == MonitorGoal::Until || goal == MonitorGoal::Within ||
         goal == MonitorGoal::WeakUntil;
}

struct TransitionObligationSpec {
  std::string trigger;
  ClaimMonitorSpec monitor;
};

struct ActiveObligation {
  bool active{false};
  ClaimMonitorState state;
};

std::string obligation_key(const std::vector<ActiveObligation>& obligations) {
  std::string result;
  for (const ActiveObligation& obligation : obligations) {
    result += obligation.active ? "|1:" : "|0:";
    if (obligation.active) result += monitor_key(obligation.state);
  }
  return result;
}

std::vector<ActiveObligation> advance_obligations(
    const std::vector<TransitionObligationSpec>& specs,
    const std::vector<ActiveObligation>& previous, const Engine& successor,
    std::string_view transition) {
  std::vector<ActiveObligation> current = previous;
  for (std::size_t index = 0; index < specs.size(); ++index) {
    const TransitionObligationSpec& spec = specs[index];
    ActiveObligation& item = current[index];
    const bool triggered = transition_matches(transition, spec.trigger);
    if (item.active) {
      item.state = advance_monitor(spec.monitor, item.state, successor, transition);
      if (item.state.phase == MonitorPhase::Satisfied) item.active = false;
    }
    if (triggered && !item.active) {
      ClaimMonitorState empty;
      empty.since_values.assign(spec.monitor.since_slots.size(), 0U);
      item.state = advance_monitor(spec.monitor, empty, successor, transition);
      item.active = item.state.phase != MonitorPhase::Satisfied;
    }
  }
  return current;
}

bool obligation_violated(const std::vector<ActiveObligation>& obligations) {
  return std::any_of(obligations.begin(), obligations.end(),
                     [](const ActiveObligation& item) {
                       return item.active &&
                              item.state.phase == MonitorPhase::Violated;
                     });
}

bool pending_liveness_obligation(
    const std::vector<TransitionObligationSpec>& specs,
    const std::vector<ActiveObligation>& obligations) {
  for (std::size_t index = 0; index < specs.size(); ++index) {
    if (obligations[index].active &&
        obligations[index].state.phase == MonitorPhase::Waiting &&
        needs_acceptance_cycle(specs[index].monitor.goal)) return true;
  }
  return false;
}

}  // namespace

Solver::Solver(Program program, SolverEncoding encoding)
    : program_(std::move(program)), encoding_(encoding) {
  if (program_.empty()) throw Error("cannot construct a Solver from an empty program");
}

Embedding Solver::initial_embedding() const {
  return embedding_of(Engine(program_, encoding_));
}

const RawKeyMap& Solver::raw_key_map() const {
  return program_.implementation()->raw_key_map;
}

ClaimSolveResult Solver::verify_claim(std::string_view claim_name,
                                      SolverLimits limits) const {
  if (limits.max_embeddings == 0U || limits.max_depth == 0U) {
    throw Error("Solver limits must be positive");
  }
  const Program::Impl& program = *program_.implementation();
  const auto claim = std::find_if(
      program.claims.begin(), program.claims.end(),
      [&](const ClaimDeclaration& item) { return item.name == claim_name; });
  if (claim == program.claims.end()) {
    throw Error("unknown Claim '" + std::string(claim_name) + "'");
  }

  ClaimSolveResult result;
  result.claim = claim->name;
  if (observes_round(claim->property) || program_observes_round(program)) {
    result.status = ClaimSolveStatus::Inconclusive;
    result.detail =
        "round-dependent semantics need an explicit finite time model in the "
        "EmbeddingExpand product";
    return result;
  }
  const std::optional<ClaimMonitorSpec> compiled = compile_claim_monitor(*claim);
  if (!compiled) {
    result.status = ClaimSolveStatus::Inconclusive;
    result.detail =
        "temporal nesting is outside the deterministic ClaimMonitor fragment";
    return result;
  }
  FunctionScope function_scope(program.functions);
  const ClaimMonitorSpec& monitor = *compiled;
  std::vector<TransitionObligationSpec> obligation_specs;
  const auto add_obligation = [&](std::string trigger,
                                  const TemporalExprPtr& property) {
    if (!property) return true;
    ClaimDeclaration synthetic;
    synthetic.property = property;
    const std::optional<ClaimMonitorSpec> compiled_obligation =
        compile_claim_monitor(synthetic);
    if (!compiled_obligation) return false;
    obligation_specs.push_back(
        TransitionObligationSpec{std::move(trigger), *compiled_obligation});
    return true;
  };
  for (const Transition& transition : program.transitions) {
    const std::string base = transition.name +
        (transition.case_name.empty() ? "" : "." + transition.case_name);
    if (!add_obligation(base, transition.obligation)) {
      result.status = ClaimSolveStatus::Inconclusive;
      result.detail = "transition ensure uses unsupported temporal nesting";
      return result;
    }
    for (const TransitionAlternative& alternative : transition.alternatives) {
      const std::string path = transition.name +
          (alternative.name.empty() ? "" : "." + alternative.name);
      if (!add_obligation(path, alternative.obligation)) {
        result.status = ClaimSolveStatus::Inconclusive;
        result.detail = "transition ensure uses unsupported temporal nesting";
        return result;
      }
    }
  }

  Engine initial = [&]() {
    if (claim->target_kind == ClaimDeclaration::TargetKind::Procedure) {
      return Engine::from_procedure(program_, claim->target, encoding_);
    }
    if (claim->target_kind == ClaimDeclaration::TargetKind::State) {
      const State& state = find_state(program, claim->target);
      return Engine(program_, {{state.context, state.name}}, encoding_);
    }
    return Engine(program_, encoding_);
  }();

  struct ProductNode {
    Engine engine;
    std::size_t embedding{0};
    ClaimMonitorState monitor;
    std::vector<ActiveObligation> obligations;
    std::optional<std::size_t> parent;
    std::string transition;
    std::size_t depth{0};
  };
  EmbeddingStore embeddings;
  const auto root = embeddings.insert(embedding_of(initial));
  ClaimMonitorState empty_monitor;
  empty_monitor.since_values.assign(monitor.since_slots.size(), 0U);
  ClaimMonitorState initial_monitor =
      advance_monitor(monitor, empty_monitor, initial);
  std::vector<ProductNode> nodes;
  std::vector<ActiveObligation> initial_obligations(obligation_specs.size());
  nodes.push_back(ProductNode{std::move(initial), root.index,
                              std::move(initial_monitor),
                              std::move(initial_obligations), {}, {}, 0U});
  EmbeddingExpand state_expand(1U);
  std::vector<std::vector<EmbeddingExpand::Edge>> product_edges(1U);
  std::map<std::pair<std::size_t, std::string>, std::size_t> product_index;
  product_index.emplace(
      std::pair{root.index,
                monitor_key(nodes.front().monitor) +
                    obligation_key(nodes.front().obligations)}, 0U);
  std::set<std::string, std::less<>> monitor_states{
      monitor_key(nodes.front().monitor)};

  const auto counterexample = [&](std::size_t index) {
    std::vector<std::size_t> path;
    for (std::optional<std::size_t> cursor = index; cursor;
         cursor = nodes[*cursor].parent) {
      path.push_back(*cursor);
    }
    std::reverse(path.begin(), path.end());
    std::vector<CounterexampleFrame> frames;
    frames.reserve(path.size());
    for (const std::size_t item : path) {
      const ProductNode& node = nodes[item];
      const EmbeddingRecord& embedding =
          embeddings.at(node.embedding);
      frames.push_back(CounterexampleFrame{node.depth, embedding.digest,
                                            node.transition,
                                            embedding.embedding});
    }
    return frames;
  };
  const auto update_counts = [&]() {
    result.explored_embeddings = embeddings.size();
    result.explored_product_states = nodes.size();
    result.claim_monitor_states = monitor_states.size();
  };

  if (nodes.front().monitor.phase == MonitorPhase::Violated) {
    result.status = ClaimSolveStatus::Counterexample;
    result.counterexample = counterexample(0U);
    result.detail = "initial Embedding is rejected by ClaimMonitor";
    update_counts();
    return result;
  }
  if (claim->target_kind == ClaimDeclaration::TargetKind::State) {
    if (nodes.front().monitor.phase == MonitorPhase::Satisfied ||
        monitor.goal == MonitorGoal::Always ||
        monitor.goal == MonitorGoal::WeakUntil ||
        monitor.goal == MonitorGoal::CountAtMost) {
      result.status = ClaimSolveStatus::Verified;
      result.detail = "typed state satisfies the local ClaimMonitor";
    } else {
      result.status = ClaimSolveStatus::Inconclusive;
      result.detail = "a state target has no path on which to discharge a future obligation";
    }
    update_counts();
    return result;
  }

  std::size_t cursor = 0;
  bool depth_limited = false;
  bool embedding_limited = false;
  bool parameterized_inputs = false;

  while (cursor < nodes.size()) {
    result.max_depth_reached = std::max(result.max_depth_reached,
                                        nodes[cursor].depth);
    if (nodes[cursor].depth >= limits.max_depth) {
      depth_limited = true;
      ++cursor;
      continue;
    }
    if (nodes[cursor].monitor.phase == MonitorPhase::Satisfied &&
        terminal_satisfaction(monitor.goal) && obligation_specs.empty() &&
        std::none_of(nodes[cursor].obligations.begin(),
                     nodes[cursor].obligations.end(),
                     [](const ActiveObligation& item) { return item.active; })) {
      ++cursor;
      continue;
    }
    bool enabled_any = false;
    for (const Transition& transition : program.transitions) {
      if (!transition.procedure_scope.empty() &&
          (claim->target_kind != ClaimDeclaration::TargetKind::Procedure ||
           transition.procedure_scope != claim->target)) {
        continue;
      }
      if (!transition.parameters.empty()) {
        parameterized_inputs = true;
        continue;
      }
      Engine successor = nodes[cursor].engine;
      StepResult step;
      try {
        step = successor.step_transition(TransitionInput{transition.name, {}});
      } catch (const Error& error) {
        if (unavailable_transition(error)) continue;
        result.status = ClaimSolveStatus::Inconclusive;
        update_counts();
        result.detail = "transition expansion stopped: " +
                        std::string(error.what());
        return result;
      }
      enabled_any = true;
      ++result.explored_edges;
      Embedding embedding = embedding_of(successor);
      std::size_t embedding_index = 0;
      if (const auto existing = embeddings.find(embedding)) {
        embedding_index = *existing;
      } else {
        if (nodes.size() >= limits.max_embeddings) {
          embedding_limited = true;
          continue;
        }
        const auto inserted = embeddings.insert(
            std::move(embedding), nodes[cursor].embedding,
            step.transition);
        embedding_index = inserted.index;
        state_expand.add_node();
      }
      state_expand.add_edge(nodes[cursor].embedding, embedding_index,
                            step.transition);
      ClaimMonitorState successor_monitor = advance_monitor(
          monitor, nodes[cursor].monitor, successor, step.transition);
      std::vector<ActiveObligation> successor_obligations =
          advance_obligations(obligation_specs, nodes[cursor].obligations,
                              successor, step.transition);
      const std::string next_monitor_key = monitor_key(successor_monitor) +
          obligation_key(successor_obligations);
      monitor_states.insert(next_monitor_key);
      const auto key = std::pair{embedding_index, next_monitor_key};
      std::size_t successor_index = 0;
      if (const auto existing = product_index.find(key);
          existing != product_index.end()) {
        successor_index = existing->second;
      } else {
        if (nodes.size() >= limits.max_embeddings) {
          embedding_limited = true;
          continue;
        }
        successor_index = nodes.size();
        product_index.emplace(key, successor_index);
        nodes.push_back(ProductNode{std::move(successor), embedding_index,
                                    std::move(successor_monitor),
                                    std::move(successor_obligations), cursor,
                                    step.transition, nodes[cursor].depth + 1U});
        product_edges.emplace_back();
        if (nodes.back().monitor.phase == MonitorPhase::Violated ||
            obligation_violated(nodes.back().obligations)) {
          result.status = ClaimSolveStatus::Counterexample;
          result.counterexample = counterexample(successor_index);
          result.max_depth_reached = nodes.back().depth;
          result.detail = "reachable Product state is rejected by ClaimMonitor";
          update_counts();
          return result;
        }
      }
      product_edges[cursor].emplace_back(successor_index, step.transition);
    }
    const bool pending_claim = needs_acceptance_cycle(monitor.goal) &&
        nodes[cursor].monitor.phase == MonitorPhase::Waiting;
    const bool pending_ensure = pending_liveness_obligation(
        obligation_specs, nodes[cursor].obligations);
    if ((pending_claim || pending_ensure) && !enabled_any &&
        !parameterized_inputs) {
      result.status = ClaimSolveStatus::Counterexample;
      result.counterexample = counterexample(cursor);
      result.detail = "pending temporal obligation reaches a deadlock";
      update_counts();
      return result;
    }
    ++cursor;
  }

  if (needs_acceptance_cycle(monitor.goal) || !obligation_specs.empty()) {
    std::vector<std::size_t> cycle_nodes;
    std::vector<std::string> cycle_transitions;
    bool cycle = false;
    for (std::size_t condition = 0;
         condition <= obligation_specs.size() && !cycle; ++condition) {
      if (condition == 0U && !needs_acceptance_cycle(monitor.goal)) continue;
      if (condition != 0U &&
          !needs_acceptance_cycle(obligation_specs[condition - 1U].monitor.goal)) {
        continue;
      }
      const auto waiting = [&](std::size_t node) {
        if (condition == 0U) {
          return nodes[node].monitor.phase == MonitorPhase::Waiting;
        }
        const ActiveObligation& obligation =
            nodes[node].obligations[condition - 1U];
        return obligation.active &&
               obligation.state.phase == MonitorPhase::Waiting;
      };
      std::vector<std::uint8_t> color(nodes.size(), 0U);
      std::vector<std::string> incoming(nodes.size());
      for (std::size_t index = 0; index < nodes.size() && !cycle; ++index) {
        if (!waiting(index) || color[index] != 0U) continue;
        std::vector<std::pair<std::size_t, std::size_t>> stack{{index, 0U}};
        color[index] = 1U;
        while (!stack.empty() && !cycle) {
          auto& [current, edge_index] = stack.back();
          if (edge_index == product_edges[current].size()) {
            color[current] = 2U;
            stack.pop_back();
            continue;
          }
          const auto& [target, transition] = product_edges[current][edge_index++];
          if (!waiting(target)) continue;
          if (color[target] == 0U) {
            color[target] = 1U;
            incoming[target] = transition;
            stack.emplace_back(target, 0U);
          } else if (color[target] == 1U) {
            cycle = true;
            const auto begin = std::find_if(
                stack.begin(), stack.end(), [&](const auto& frame) {
                  return frame.first == target;
                });
            for (auto item = begin; item != stack.end(); ++item) {
              cycle_nodes.push_back(item->first);
              if (item != begin) cycle_transitions.push_back(incoming[item->first]);
            }
            cycle_transitions.push_back(transition);
          }
        }
      }
    }
    if (cycle) {
      result.status = ClaimSolveStatus::Counterexample;
      result.counterexample = counterexample(cycle_nodes.front());
      for (std::size_t index = 1U; index < cycle_nodes.size(); ++index) {
        const EmbeddingRecord& embedding =
            embeddings.at(nodes[cycle_nodes[index]].embedding);
        result.counterexample.push_back(CounterexampleFrame{
            result.counterexample.back().depth + 1U, embedding.digest,
            cycle_transitions[index - 1U], embedding.embedding});
      }
      const EmbeddingRecord& target =
          embeddings.at(nodes[cycle_nodes.front()].embedding);
      result.counterexample.push_back(CounterexampleFrame{
          result.counterexample.back().depth + 1U, target.digest,
          cycle_transitions.back(), target.embedding});
      result.detail =
          "pending temporal obligation has a reachable accepting-cycle counterexample";
      update_counts();
      return result;
    }
  }

  update_counts();
  if (parameterized_inputs) {
    result.status = ClaimSolveStatus::Inconclusive;
    result.detail = "parameterized transitions need explicit finite input domains";
  } else if (depth_limited || embedding_limited) {
    result.status = ClaimSolveStatus::BoundedVerified;
    result.detail = "no counterexample found within configured Solver bounds";
  } else {
    result.status = ClaimSolveStatus::Verified;
    result.detail =
        "finite EmbeddingExpand x ClaimMonitor product exhausted without a counterexample";
  }
  return result;
}

std::string_view claim_solve_status_name(ClaimSolveStatus status) noexcept {
  switch (status) {
    case ClaimSolveStatus::Counterexample: return "counterexample";
    case ClaimSolveStatus::Verified: return "verified";
    case ClaimSolveStatus::BoundedVerified: return "bounded-verified";
    case ClaimSolveStatus::Inconclusive: return "inconclusive";
  }
  return "unknown";
}
