// High-performance semantic search layer. This file is included once by
// frontend.cpp because the verified typed AST remains private. The Solver owns
// transition expansion and Claim counterexample search; runtime.cpp owns only
// execution, procedure persistence, replay and capture.

namespace {

Configuration configuration_of(const Engine& engine) {
  return Configuration{engine.current_states(), engine.values()};
}

std::vector<CounterexampleFrame> counterexample_prefix(
    const ConfigurationStore& store, std::size_t index) {
  std::vector<CounterexampleFrame> result;
  for (const std::size_t item : store.path_to(index)) {
    const ConfigurationRecord& record = store.at(item);
    result.push_back(CounterexampleFrame{record.depth, record.digest,
                                         record.transition,
                                         record.configuration});
  }
  return result;
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

bool action_observes_round(const std::shared_ptr<ActionExpr>& action) {
  if (!action) return false;
  if (std::any_of(action->arguments.begin(), action->arguments.end(),
                  observes_round)) {
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
                    observes_round)) {
      return true;
    }
  }
  for (const Transition& transition : program.transitions) {
    if (observes_round(transition.condition) ||
        observes_round(transition.optimized_score) ||
        targets_observe_round(transition.to) ||
        action_observes_round(transition.action)) {
      return true;
    }
    for (const TransitionAlternative& alternative : transition.alternatives) {
      if (observes_round(alternative.condition) ||
          targets_observe_round(alternative.to) ||
          action_observes_round(alternative.action)) {
        return true;
      }
    }
  }
  return false;
}

// StateExpand is the reachable graph produced by transition expansion. Its
// node index is the matching ConfigurationStore index; it is deliberately not
// a tree because transitions may join or form cycles.
struct StateExpand {
  using Edge = std::pair<std::size_t, std::string>;

  explicit StateExpand(std::size_t initial_nodes)
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

}  // namespace

Solver::Solver(Program program, SolverEncoding encoding)
    : program_(std::move(program)), encoding_(encoding) {
  if (program_.empty()) throw Error("cannot construct a Solver from an empty program");
}

Configuration Solver::initial_configuration() const {
  return configuration_of(Engine(program_, encoding_));
}

ClaimSolveResult Solver::verify_claim(std::string_view claim_name,
                                      SolverLimits limits) const {
  if (limits.max_configurations == 0U || limits.max_depth == 0U) {
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
  if (observes_round(claim->predicate) || program_observes_round(program)) {
    result.status = ClaimSolveStatus::Inconclusive;
    result.detail =
        "round-dependent semantics need an explicit finite time model in the "
        "StateExpand product";
    return result;
  }
  if (claim->kind == ClaimDeclaration::Kind::CountAtMost) {
    result.status = ClaimSolveStatus::Inconclusive;
    result.detail = "count claims require a monitor-automaton product";
    return result;
  }

  FunctionScope function_scope(program.functions);
  const auto matches = [&](const Engine& engine) {
    Environment environment{engine.values(), nullptr, {}, engine.current_round()};
    return evaluate(claim->predicate, environment).as_bool();
  };
  struct SearchNode {
    Engine engine;
    bool matches{false};
  };

  ConfigurationStore configurations;
  Engine initial(program_, encoding_);
  const auto root = configurations.insert(configuration_of(initial));
  std::vector<SearchNode> nodes;
  nodes.push_back(SearchNode{initial, matches(initial)});
  // ConfigurationStore's parent pointer is deliberately only one shortest
  // discovery witness through StateExpand.
  StateExpand state_expand(1U);
  std::size_t cursor = 0;
  bool depth_limited = false;
  bool configuration_limited = false;
  bool parameterized_inputs = false;

  if (claim->kind == ClaimDeclaration::Kind::Always && !nodes.front().matches) {
    result.status = ClaimSolveStatus::Counterexample;
    result.counterexample = counterexample_prefix(configurations, root.index);
    result.explored_configurations = 1U;
    result.detail = "initial Configuration violates always predicate";
    return result;
  }
  if (claim->kind == ClaimDeclaration::Kind::Eventually && nodes.front().matches) {
    result.status = ClaimSolveStatus::Verified;
    result.explored_configurations = 1U;
    result.detail = "eventually predicate holds in the initial Configuration";
    return result;
  }

  while (cursor < nodes.size()) {
    const ConfigurationRecord& record = configurations.at(cursor);
    result.max_depth_reached = std::max(result.max_depth_reached, record.depth);
    if (record.depth >= limits.max_depth) {
      depth_limited = true;
      ++cursor;
      continue;
    }
    bool enabled_any = false;
    for (const Transition& transition : program.transitions) {
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
        result.explored_configurations = configurations.size();
        result.detail = "transition expansion stopped: " +
                        std::string(error.what());
        return result;
      }
      enabled_any = true;
      ++result.explored_edges;
      Configuration configuration = configuration_of(successor);
      const std::string digest = configuration_digest(configuration);
      std::size_t successor_index = 0;
      if (const auto existing = configurations.find(digest)) {
        if (!(configurations.at(*existing).configuration == configuration)) {
          throw Error("Configuration digest collision in StateExpand");
        }
        successor_index = *existing;
      } else {
        if (configurations.size() >= limits.max_configurations) {
          configuration_limited = true;
          continue;
        }
        const auto inserted = configurations.insert(
            std::move(configuration), cursor, step.transition);
        successor_index = inserted.index;
        nodes.push_back(SearchNode{std::move(successor), false});
        nodes.back().matches = matches(nodes.back().engine);
        state_expand.add_node();
        if (claim->kind == ClaimDeclaration::Kind::Always &&
            !nodes.back().matches) {
          result.status = ClaimSolveStatus::Counterexample;
          result.counterexample =
              counterexample_prefix(configurations, successor_index);
          result.explored_configurations = configurations.size();
          result.max_depth_reached = configurations.at(successor_index).depth;
          result.detail = "reachable Configuration violates always predicate";
          return result;
        }
      }
      state_expand.add_edge(cursor, successor_index, step.transition);
    }
    if (claim->kind == ClaimDeclaration::Kind::Eventually &&
        !nodes[cursor].matches && !enabled_any && !parameterized_inputs) {
      result.status = ClaimSolveStatus::Counterexample;
      result.counterexample = counterexample_prefix(configurations, cursor);
      result.explored_configurations = configurations.size();
      result.detail = "eventually predicate has a non-matching deadlock";
      return result;
    }
    ++cursor;
  }

  if (claim->kind == ClaimDeclaration::Kind::Eventually) {
    std::vector<std::uint8_t> color(nodes.size(), 0U);
    std::size_t cycle_from = 0;
    std::size_t cycle_to = 0;
    std::string cycle_transition;
    bool cycle = false;
    for (std::size_t index = 0; index < nodes.size() && !cycle; ++index) {
      if (nodes[index].matches || color[index] != 0U) continue;
      std::vector<std::pair<std::size_t, std::size_t>> stack{{index, 0U}};
      color[index] = 1U;
      while (!stack.empty() && !cycle) {
        auto& [current, edge_index] = stack.back();
        if (edge_index == state_expand.outgoing(current).size()) {
          color[current] = 2U;
          stack.pop_back();
          continue;
        }
        const auto& [target, transition] =
            state_expand.outgoing(current)[edge_index++];
        if (nodes[target].matches) continue;
        if (color[target] == 0U) {
          color[target] = 1U;
          stack.emplace_back(target, 0U);
        } else if (color[target] == 1U) {
          cycle = true;
          cycle_from = current;
          cycle_to = target;
          cycle_transition = transition;
        }
      }
    }
    if (cycle) {
      result.status = ClaimSolveStatus::Counterexample;
      result.counterexample = counterexample_prefix(configurations, cycle_from);
      const ConfigurationRecord& target = configurations.at(cycle_to);
      result.counterexample.push_back(CounterexampleFrame{
          result.counterexample.back().depth + 1U, target.digest,
          std::move(cycle_transition), target.configuration});
      result.explored_configurations = configurations.size();
      result.detail = "eventually predicate has a reachable non-matching lasso";
      return result;
    }
  }

  result.explored_configurations = configurations.size();
  if (parameterized_inputs) {
    result.status = ClaimSolveStatus::Inconclusive;
    result.detail = "parameterized transitions need explicit finite input domains";
  } else if (depth_limited || configuration_limited) {
    result.status = ClaimSolveStatus::BoundedVerified;
    result.detail = "no counterexample found within configured Solver bounds";
  } else {
    result.status = ClaimSolveStatus::Verified;
    result.detail =
        "finite StateExpand exhausted without a counterexample";
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
