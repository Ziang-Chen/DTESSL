// Finite-trace semantics. This file is included once by runtime.cpp after the
// private typed trace AST is available. It owns trace-position relations and
// temporal recursion; instantaneous predicates are delegated to semantics.cpp.

struct TemporalFrame {
  const std::map<std::string, Value, std::less<>>* state;
  std::uint64_t round;
};

using TemporalMemoKey = std::pair<const TemporalExpr*, std::size_t>;

bool evaluate_temporal(const TemporalExprPtr& expression,
                       const std::vector<TemporalFrame>& frames,
                       std::size_t position,
                       std::map<TemporalMemoKey, bool>& memo) {
  if (!expression || position >= frames.size()) return false;
  const TemporalMemoKey key{expression.get(), position};
  if (const auto found = memo.find(key); found != memo.end()) return found->second;
  const auto at = [&](const TemporalExprPtr& item, std::size_t index) {
    return evaluate_temporal(item, frames, index, memo);
  };
  bool result = false;
  switch (expression->kind) {
    case TemporalExpr::Kind::Atom: {
      Environment environment{*frames[position].state, nullptr, {},
                              frames[position].round};
      result = evaluate(expression->atom, environment).as_bool();
      break;
    }
    case TemporalExpr::Kind::TraceRelationMatch:
      if (expression->relation != "happens_before") {
        throw Error("unknown trace relation '" + expression->relation + "'");
      }
      // Strict happens-before: the first subject must hold at a round where
      // the second is still false, and the second may not have held earlier.
      for (std::size_t index = position; index < frames.size(); ++index) {
        if (at(expression->right, index)) break;
        if (at(expression->left, index)) {
          result = true;
          break;
        }
      }
      break;
    case TemporalExpr::Kind::Not:
      result = !at(expression->left, position);
      break;
    case TemporalExpr::Kind::And:
      result = at(expression->left, position) && at(expression->right, position);
      break;
    case TemporalExpr::Kind::Or:
      result = at(expression->left, position) || at(expression->right, position);
      break;
    case TemporalExpr::Kind::Always:
      result = true;
      for (std::size_t index = position; index < frames.size(); ++index) {
        if (!at(expression->left, index)) {
          result = false;
          break;
        }
      }
      break;
    case TemporalExpr::Kind::Eventually:
      for (std::size_t index = position; index < frames.size(); ++index) {
        if (at(expression->left, index)) {
          result = true;
          break;
        }
      }
      break;
    case TemporalExpr::Kind::Until:
      for (std::size_t index = position; index < frames.size(); ++index) {
        if (at(expression->right, index)) {
          result = true;
          break;
        }
        if (!at(expression->left, index)) break;
      }
      break;
    case TemporalExpr::Kind::Within: {
      const std::size_t available = frames.size() - 1U - position;
      const std::size_t distance = static_cast<std::size_t>(std::min<std::uint64_t>(
          expression->bound, static_cast<std::uint64_t>(available)));
      for (std::size_t index = position; index <= position + distance; ++index) {
        if (at(expression->left, index)) {
          result = true;
          break;
        }
      }
      break;
    }
    case TemporalExpr::Kind::Since:
      for (std::size_t index = position + 1U; index-- > 0U;) {
        if (at(expression->right, index)) {
          result = true;
          break;
        }
        if (!at(expression->left, index)) break;
      }
      break;
  }
  memo.emplace(key, result);
  return result;
}

std::uint64_t temporal_witness_round(const TemporalExprPtr& expression,
                                     const std::vector<TemporalFrame>& frames,
                                     bool property_value,
                                     std::map<TemporalMemoKey, bool>& memo) {
  if (frames.empty()) return 0;
  if (expression->kind == TemporalExpr::Kind::Always && !property_value) {
    for (std::size_t index = 0; index < frames.size(); ++index) {
      if (!evaluate_temporal(expression->left, frames, index, memo)) {
        return frames[index].round;
      }
    }
  }
  if ((expression->kind == TemporalExpr::Kind::Eventually ||
       expression->kind == TemporalExpr::Kind::Within) && property_value) {
    for (std::size_t index = 0; index < frames.size(); ++index) {
      if (evaluate_temporal(expression->left, frames, index, memo)) {
        return frames[index].round;
      }
    }
  }
  return frames.back().round;
}
