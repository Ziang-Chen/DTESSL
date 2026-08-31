// Claim formula normalization and incremental monitor semantics.
//
// This is a private textual implementation fragment. It is included by
// solver.cpp after the frontend's typed AST and runtime Engine are available.
// Keeping it separate makes the temporal monitor stable independently of the
// Product/StateExpand search strategy.

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

std::optional<ClaimMonitorSpec> compile_property_monitor(
    const TemporalExprPtr& source) {
  ClaimMonitorSpec spec;
  const TemporalExprPtr property = lower_trace_relation_matches(source);
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

ClaimMonitorSpec compile_count_monitor(std::string transition,
                                       std::uint64_t bound) {
  ClaimMonitorSpec spec;
  spec.goal = MonitorGoal::CountAtMost;
  spec.bound = bound;
  spec.counted_transition = std::move(transition);
  return spec;
}

std::optional<ClaimMonitorSpec> compile_claim_monitor(
    const ClaimDeclaration& claim) {
  if (claim.count_at_most) {
    return compile_count_monitor(claim.transition, claim.limit);
  }
  return compile_property_monitor(claim.property);
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
