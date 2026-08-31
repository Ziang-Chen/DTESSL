// Stable instantaneous semantic relations. This file is included once by
// frontend.cpp after the private typed AST is declared. Surface AST churn must
// normalize into these operations rather than teaching runtime or Solver a
// second comparison/membership implementation.

bool equal_values(const Value& left, const Value& right);
int compare_values(const Value& left, const Value& right);

DataType verify_relation_match(Expr& expression, const DataType& left,
                               const DataType& right) {
  if (expression.text == "in" || expression.text == "~") {
    const bool set_member = right.kind == DataType::Kind::Set && left == *right.first;
    const bool relation_row = right.kind == DataType::Kind::Relation &&
        (right.direct_relation_row
             ? left == right.elements.front()
             : left == DataType(DataType::Kind::Tuple, right.elements));
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
  if (left != right && !mixed_numeric) {
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
