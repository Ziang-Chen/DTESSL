// Stable instantaneous semantic relations. This file is included once by
// frontend.cpp after the private typed AST is declared. Surface AST churn must
// normalize into these operations rather than teaching runtime or Solver a
// second comparison/membership implementation.

bool equal_values(const Value& left, const Value& right);
int compare_values(const Value& left, const Value& right);

// A Property says what must hold. PropertyUse says where/when it is observed
// and, critically, what a false or unresolved result means. Surface
// invariant/where/ensure/Claim declarations all lower to this stable policy
// model without losing their distinct operational behavior.
enum class PropertyScope { State, Transition, Trace, Procedure };
enum class PropertyTrigger { Candidate, Successor, Occurrence, Target };
enum class PropertyFailure { Disable, Reject, Violation, Counterexample };
enum class PropertyTruth { Satisfied, Violated, Pending };
enum class PropertyDisposition {
  Admit,
  Disable,
  Reject,
  RecordViolation,
  Counterexample,
  Defer,
};

struct PropertyUse {
  std::string name;
  PropertyScope scope{PropertyScope::State};
  PropertyTrigger trigger{PropertyTrigger::Target};
  PropertyFailure failure{PropertyFailure::Counterexample};
};

struct PropertyDecision {
  PropertyTruth truth{PropertyTruth::Pending};
  PropertyDisposition disposition{PropertyDisposition::Defer};
};

PropertyDecision decide_property(const PropertyUse& use,
                                 PropertyTruth truth) noexcept {
  if (truth == PropertyTruth::Satisfied) {
    return {truth, PropertyDisposition::Admit};
  }
  if (truth == PropertyTruth::Pending) {
    return {truth, PropertyDisposition::Defer};
  }
  switch (use.failure) {
    case PropertyFailure::Disable:
      return {truth, PropertyDisposition::Disable};
    case PropertyFailure::Reject:
      return {truth, PropertyDisposition::Reject};
    case PropertyFailure::Violation:
      return {truth, PropertyDisposition::RecordViolation};
    case PropertyFailure::Counterexample:
      return {truth, PropertyDisposition::Counterexample};
  }
  return {truth, PropertyDisposition::Defer};
}

PropertyTruth property_truth(bool value) noexcept {
  return value ? PropertyTruth::Satisfied : PropertyTruth::Violated;
}

struct Environment;
Value evaluate(const ExprPtr& expression, Environment& environment);

PropertyDecision evaluate_instant_property(const ExprPtr& expression,
                                            Environment& environment,
                                            const PropertyUse& use) {
  if (!expression) throw Error("missing expression for Property '" + use.name + "'");
  const Value value = evaluate(expression, environment);
  if (value.kind() != Value::Kind::Bool) {
    throw Error("Property '" + use.name + "' did not evaluate to bool");
  }
  return decide_property(use, property_truth(value.as_bool()));
}

DataType verify_relation_match(Expr& expression, const DataType& left,
                               const DataType& right) {
  if (expression.text == "in" || expression.text == "~") {
    const bool set_member = right.kind == DataType::Kind::Set &&
        same_base_type(left, *right.first);
    const bool relation_row = right.kind == DataType::Kind::Relation &&
        (right.direct_relation_row
             ? same_base_type(left, right.elements.front())
             : same_base_type(
                   left, DataType(DataType::Kind::Tuple, right.elements)));
    expression.direct_relation_binding = right.kind == DataType::Kind::Relation &&
                                         right.direct_relation_row;
    if (!set_member && !relation_row) {
      throw Error("membership item type does not match finite domain row type");
    }
    return bool_type();
  }

  const bool mixed_numeric =
      (left.kind == DataType::Kind::Int || left.kind == DataType::Kind::Rational) &&
      (right.kind == DataType::Kind::Int || right.kind == DataType::Kind::Rational);
  if (!same_base_type(left, right) && !mixed_numeric) {
    throw Error("comparison operands have different types");
  }
  if ((expression.text == "<" || expression.text == "<=" ||
       expression.text == ">" || expression.text == ">=") &&
      left.kind != DataType::Kind::Int && left.kind != DataType::Kind::Rational &&
      left.kind != DataType::Kind::String) {
    throw Error("ordered comparison needs exact numeric or string operands");
  }
  return bool_type();
}

Value evaluate_relation_match(const Expr& expression, Value left, Value right) {
  if (expression.text == "=" || expression.text == "==") {
    return Value(equal_values(left, right));
  }
  if (expression.text == "!=") return Value(!equal_values(left, right));
  if (expression.text == "in" || expression.text == "~") {
    if (right.kind() == Value::Kind::StringSet) {
      return Value(right.as_string_set().values.contains(left.as_string()));
    }
    if (right.kind() == Value::Kind::Relation) {
      const auto& rows = right.as_relation().rows;
      const ValueTuple target = expression.direct_relation_binding
                                    ? ValueTuple{{std::move(left)}}
                                    : left.as_tuple();
      return Value(std::binary_search(
          rows.begin(), rows.end(), target,
          [](const ValueTuple& a, const ValueTuple& b) {
            for (std::size_t index = 0; index < a.fields.size(); ++index) {
              const int order = canonical_compare(a.fields[index], b.fields[index]);
              if (order != 0) return order < 0;
            }
            return false;
          }));
    }
    const auto& values = right.as_set().values;
    return Value(std::binary_search(values.begin(), values.end(), left,
                                    [](const Value& a, const Value& b) {
                                      return canonical_compare(a, b) < 0;
                                    }));
  }

  const int order = compare_values(left, right);
  if (expression.text == "<") return Value(order < 0);
  if (expression.text == "<=") return Value(order <= 0);
  if (expression.text == ">") return Value(order > 0);
  if (expression.text == ">=") return Value(order >= 0);
  throw Error("unknown relation match '" + expression.text + "'");
}
