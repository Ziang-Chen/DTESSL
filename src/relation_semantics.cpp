// Lazy relation/comprehension semantics.  This private fragment sits between
// the typed frontend AST and the evaluator.  Membership stays predicate-based;
// enumeration is invoked only by a collection consumer or relation algebra.

using CollectionVisitor = std::function<bool(const Value&)>;

class RelationContextScope {
 public:
  RelationContextScope(Environment& environment,
                       const RelationDeclaration& declaration)
      : environment_(environment), previous_(environment.lexical_context) {
    if (!declaration.context.empty()) {
      environment_.lexical_context = declaration.context;
    }
  }
  ~RelationContextScope() { environment_.lexical_context = std::move(previous_); }

 private:
  Environment& environment_;
  std::string previous_;
};

bool relation_row_less(const ValueTuple& left, const ValueTuple& right) {
  for (std::size_t index = 0; index < left.fields.size(); ++index) {
    const int order = canonical_compare(left.fields[index], right.fields[index]);
    if (order != 0) return order < 0;
  }
  return false;
}

bool relation_contains_row(const ValueRelation& relation,
                           const ValueTuple& row) {
  return std::binary_search(relation.rows.begin(), relation.rows.end(), row,
                            relation_row_less);
}

std::vector<Value> unary_collection_values(const Value& collection) {
  if (collection.kind() == Value::Kind::StringSet) {
    std::vector<Value> result;
    result.reserve(collection.as_string_set().values.size());
    for (const std::string& item : collection.as_string_set().values) {
      result.emplace_back(item);
    }
    return result;
  }
  if (collection.kind() == Value::Kind::Set) {
    return collection.as_set().values;
  }
  if (collection.kind() == Value::Kind::Relation &&
      collection.as_relation().arity == 1U) {
    std::vector<Value> result;
    result.reserve(collection.as_relation().rows.size());
    for (const ValueTuple& row : collection.as_relation().rows) {
      result.push_back(row.fields.front());
    }
    return result;
  }
  throw Error("expected a finite set or unary relation domain");
}

bool relation_subset_of(const ValueRelation& left,
                        const ValueRelation& right) {
  return std::includes(right.rows.begin(), right.rows.end(),
                       left.rows.begin(), left.rows.end(), relation_row_less);
}

bool relation_disjoint_from(const ValueRelation& left,
                            const ValueRelation& right) {
  std::size_t lhs = 0;
  std::size_t rhs = 0;
  while (lhs < left.rows.size() && rhs < right.rows.size()) {
    if (!relation_row_less(left.rows[lhs], right.rows[rhs]) &&
        !relation_row_less(right.rows[rhs], left.rows[lhs])) {
      return false;
    }
    if (relation_row_less(left.rows[lhs], right.rows[rhs])) ++lhs;
    else ++rhs;
  }
  return true;
}

bool relation_is_functional(const ValueRelation& relation,
                            bool inverse = false) {
  std::size_t work = 0;
  const std::size_t key = inverse ? 1U : 0U;
  const std::size_t value = inverse ? 0U : 1U;
  for (std::size_t left = 0; left < relation.rows.size(); ++left) {
    for (std::size_t right = left + 1U; right < relation.rows.size(); ++right) {
      if (++work > relation_work_limit) {
        throw Error("relation property exceeds work budget");
      }
      if (equal_values(relation.rows[left].fields[key],
                       relation.rows[right].fields[key]) &&
          !equal_values(relation.rows[left].fields[value],
                        relation.rows[right].fields[value])) {
        return false;
      }
    }
  }
  return true;
}

bool relation_is_symmetric(const ValueRelation& relation) {
  for (const ValueTuple& row : relation.rows) {
    if (!relation_contains_row(
            relation, ValueTuple{{row.fields[1], row.fields[0]}})) {
      return false;
    }
  }
  return true;
}

bool relation_is_antisymmetric(const ValueRelation& relation) {
  for (const ValueTuple& row : relation.rows) {
    if (equal_values(row.fields[0], row.fields[1])) continue;
    if (relation_contains_row(
            relation, ValueTuple{{row.fields[1], row.fields[0]}})) {
      return false;
    }
  }
  return true;
}

bool relation_is_transitive(const ValueRelation& relation) {
  std::size_t work = 0;
  for (const ValueTuple& left : relation.rows) {
    for (const ValueTuple& right : relation.rows) {
      if (++work > relation_work_limit) {
        throw Error("relation transitivity exceeds work budget");
      }
      if (!equal_values(left.fields[1], right.fields[0])) continue;
      if (!relation_contains_row(
              relation, ValueTuple{{left.fields[0], right.fields[1]}})) {
        return false;
      }
    }
  }
  return true;
}

ValueRelation relation_transitive_closure(ValueRelation current) {
  std::size_t work = 0;
  for (;;) {
    std::vector<ValueTuple> additions;
    for (const ValueTuple& left : current.rows) {
      for (const ValueTuple& right : current.rows) {
        if (++work > relation_work_limit) {
          throw Error("relation closure exceeds work budget");
        }
        if (!equal_values(left.fields[1], right.fields[0])) continue;
        ValueTuple candidate{{left.fields[0], right.fields[1]}};
        if (!relation_contains_row(current, candidate)) {
          additions.push_back(std::move(candidate));
        }
      }
    }
    if (additions.empty()) return current;
    current.rows.insert(current.rows.end(), additions.begin(), additions.end());
    const Value normalized(std::move(current));
    current = normalized.as_relation();
    if (current.rows.size() > relation_row_limit) {
      throw Error("relation closure exceeds row budget");
    }
  }
}

bool relation_is_reflexive(const ValueRelation& relation,
                           const std::vector<Value>& domain) {
  return std::all_of(domain.begin(), domain.end(), [&](const Value& item) {
    return relation_contains_row(relation, ValueTuple{{item, item}});
  });
}

bool relation_is_irreflexive(const ValueRelation& relation) {
  return std::none_of(relation.rows.begin(), relation.rows.end(),
                      [](const ValueTuple& row) {
                        return equal_values(row.fields[0], row.fields[1]);
                      });
}

bool relation_is_acyclic(const ValueRelation& relation) {
  const ValueRelation closure = relation_transitive_closure(relation);
  return relation_is_irreflexive(closure);
}

bool relation_covers_carrier(const ValueRelation& relation,
                             const std::vector<Value>& carrier,
                             std::size_t column) {
  std::size_t work = 0;
  return std::all_of(carrier.begin(), carrier.end(), [&](const Value& item) {
    return std::any_of(relation.rows.begin(), relation.rows.end(),
                       [&](const ValueTuple& row) {
                         if (++work > relation_work_limit) {
                           throw Error("relation coverage exceeds work budget");
                         }
                         return equal_values(item, row.fields[column]);
                       });
  });
}

bool relation_is_between_carriers(const ValueRelation& relation,
                                  const std::vector<Value>& domain,
                                  const std::vector<Value>& codomain) {
  std::size_t work = 0;
  const auto contains = [&](const std::vector<Value>& carrier,
                            const Value& item) {
    return std::any_of(carrier.begin(), carrier.end(), [&](const Value& value) {
      if (++work > relation_work_limit) {
        throw Error("relation carrier check exceeds work budget");
      }
      return equal_values(value, item);
    });
  };
  return std::all_of(relation.rows.begin(), relation.rows.end(),
                     [&](const ValueTuple& row) {
                       return contains(domain, row.fields[0]) &&
                              contains(codomain, row.fields[1]);
                     });
}

bool relation_is_total_order(const ValueRelation& relation,
                             const std::vector<Value>& carrier) {
  if (!relation_is_between_carriers(relation, carrier, carrier) ||
      !relation_is_reflexive(relation, carrier) ||
      !relation_is_antisymmetric(relation) ||
      !relation_is_transitive(relation)) {
    return false;
  }
  std::size_t work = 0;
  for (const Value& left : carrier) {
    for (const Value& right : carrier) {
      if (++work > relation_work_limit) {
        throw Error("total-order property exceeds work budget");
      }
      if (!relation_contains_row(relation, ValueTuple{{left, right}}) &&
          !relation_contains_row(relation, ValueTuple{{right, left}})) {
        return false;
      }
    }
  }
  return true;
}

bool visit_collection_expression(const ExprPtr& expression,
                                 Environment& environment,
                                 const CollectionVisitor& visitor,
                                 std::size_t& work);
Value materialize_relation(const RelationDeclaration& declaration,
                           Environment& environment);

bool relation_membership(const RelationDeclaration& declaration,
                         const Value& subject, Environment& environment) {
  RelationContextScope context_scope(environment, declaration);
  if (declaration.derived_expression) {
    const Value relation = materialize_relation(declaration, environment);
    const ValueTuple target = declaration.type.direct_relation_row
                                  ? ValueTuple{{subject}}
                                  : subject.as_tuple();
    return std::binary_search(
        relation.as_relation().rows.begin(), relation.as_relation().rows.end(),
        target, [](const ValueTuple& left, const ValueTuple& right) {
          for (std::size_t index = 0; index < left.fields.size(); ++index) {
            const int order = canonical_compare(left.fields[index],
                                                right.fields[index]);
            if (order != 0) return order < 0;
          }
          return false;
        });
  }
  std::vector<Value> arguments;
  if (declaration.parameters.size() == 1U && declaration.type.direct_relation_row) {
    arguments.push_back(subject);
  } else {
    if (subject.kind() != Value::Kind::Tuple ||
        subject.as_tuple().fields.size() != declaration.parameters.size()) {
      throw Error("relation '" + declaration.name + "' subject has wrong arity");
    }
    arguments = subject.as_tuple().fields;
  }
  std::map<std::string, std::optional<Value>, std::less<>> saved;
  for (std::size_t index = 0; index < declaration.parameters.size(); ++index) {
    const std::string& name = declaration.parameters[index].name;
    const auto previous = environment.locals.find(name);
    saved.emplace(name, previous == environment.locals.end()
                            ? std::nullopt
                            : std::optional<Value>(previous->second));
    environment.locals.insert_or_assign(name, arguments[index]);
  }
  const auto restore = [&]() {
    for (const auto& [name, value] : saved) {
      if (value) environment.locals.insert_or_assign(name, *value);
      else environment.locals.erase(name);
    }
  };
  try {
    for (const ComprehensionClause& clause : declaration.clauses) {
      if (clause.kind == ComprehensionClause::Kind::Predicate) {
        if (!evaluate(clause.expression, environment).as_bool()) {
          restore();
          return false;
        }
        continue;
      }
      const Value& argument = environment.locals.at(clause.binding);
      bool present = false;
      std::size_t work = 0;
      visit_collection_expression(
          clause.expression, environment,
          [&](const Value& candidate) {
            present = equal_values(candidate, argument);
            return present;
          }, work);
      if (!present) {
        restore();
        return false;
      }
    }
  } catch (...) {
    restore();
    throw;
  }
  restore();
  return true;
}

bool visit_comprehension(const ExprPtr& expression, Environment& environment,
                         const CollectionVisitor& visitor, std::size_t& work) {
  if (expression->text == "set_union") {
    // Braces denote an unordered anonymous relation.  A source permutation
    // must therefore not alter enumeration, budgets, or the first witness
    // observed by a consumer.  Each branch remains lazy until the union is
    // demanded; the union boundary is the canonical finite merge barrier.
    ValueSet merged;
    for (const ExprPtr& branch : expression->children) {
      std::size_t branch_work = 0;
      visit_collection_expression(
          branch, environment,
          [&](const Value& item) {
            merged.values.push_back(item);
            if (merged.values.size() > relation_work_limit) {
              throw Error("lazy set union exceeds work budget");
            }
            return false;
          }, branch_work);
    }
    const Value canonical(std::move(merged));
    for (const Value& item : canonical.as_set().values) {
      if (++work > relation_work_limit) throw Error("lazy set union exceeds work budget");
      if (visitor(item)) return true;
    }
    return false;
  }
  std::map<std::string, std::optional<Value>, std::less<>> saved;
  std::function<bool(std::size_t)> advance = [&](std::size_t index) {
    if (index == expression->clauses.size()) {
      if (++work > relation_work_limit) throw Error("lazy comprehension exceeds work budget");
      return visitor(evaluate(expression->left, environment));
    }
    const ComprehensionClause& clause = expression->clauses[index];
    if (clause.kind == ComprehensionClause::Kind::Predicate) {
      return evaluate(clause.expression, environment).as_bool()
                 ? advance(index + 1U)
                 : false;
    }
    if (!saved.contains(clause.binding)) {
      const auto previous = environment.locals.find(clause.binding);
      saved.emplace(clause.binding, previous == environment.locals.end()
                                        ? std::nullopt
                                        : std::optional<Value>(previous->second));
    }
    bool stopped = false;
    std::size_t nested_work = 0;
    visit_collection_expression(
        clause.expression, environment,
        [&](const Value& item) {
          environment.locals.insert_or_assign(clause.binding, item);
          stopped = advance(index + 1U);
          return stopped;
        }, nested_work);
    return stopped;
  };
  try {
    const bool stopped = advance(0);
    for (const auto& [name, value] : saved) {
      if (value) environment.locals.insert_or_assign(name, *value);
      else environment.locals.erase(name);
    }
    return stopped;
  } catch (...) {
    for (const auto& [name, value] : saved) {
      if (value) environment.locals.insert_or_assign(name, *value);
      else environment.locals.erase(name);
    }
    throw;
  }
}

Value materialize_relation(const RelationDeclaration& declaration,
                           Environment& environment) {
  RelationContextScope context_scope(environment, declaration);
  if (declaration.derived_expression) {
    return evaluate(declaration.derived_expression, environment);
  }
  if (!declaration.enumerable) {
    throw Error("relation '" + declaration.name +
                "' is decidable but not enumerable; add finite generators");
  }
  auto plan = std::make_shared<Expr>();
  plan->kind = Expr::Kind::Comprehension;
  plan->text = "relation";
  plan->resolved_type = declaration.type;
  plan->clauses = declaration.clauses;
  auto tuple = std::make_shared<Expr>();
  tuple->kind = Expr::Kind::Construct;
  tuple->text = "tuple";
  for (const Parameter& parameter : declaration.parameters) {
    tuple->children.push_back(make_name(parameter.name));
  }
  tuple->resolved_type = DataType(DataType::Kind::Tuple, declaration.type.elements);
  plan->left = declaration.parameters.size() == 1U ? tuple->children.front() : tuple;

  std::set<std::string, std::less<>> generated;
  for (const ComprehensionClause& clause : plan->clauses) {
    if (clause.kind == ComprehensionClause::Kind::Generator) generated.insert(clause.binding);
  }
  std::vector<ComprehensionClause> completed;
  for (const Parameter& parameter : declaration.parameters) {
    if (generated.contains(parameter.name)) continue;
    const auto values = generate_static_constraint_domain(parameter.type, *active_types);
    if (!values) throw Error("relation parameter has no finite generator");
    auto literal = std::make_shared<Expr>();
    literal->kind = Expr::Kind::Literal;
    literal->literal = Value(ValueSet{*values});
    completed.push_back(ComprehensionClause{
        ComprehensionClause::Kind::Generator, parameter.name,
        std::move(literal), 0, 0});
  }
  // Relation clauses are declarative conjunctions.  Their source order must
  // not decide whether a parameter has been bound when a predicate runs.
  for (const ComprehensionClause& clause : plan->clauses) {
    if (clause.kind == ComprehensionClause::Kind::Generator) {
      completed.push_back(clause);
    }
  }
  for (const ComprehensionClause& clause : plan->clauses) {
    if (clause.kind == ComprehensionClause::Kind::Predicate) {
      completed.push_back(clause);
    }
  }
  plan->clauses = std::move(completed);

  ValueRelation output{declaration.parameters.size(), {}};
  std::size_t work = 0;
  visit_comprehension(
      plan, environment,
      [&](const Value& item) {
        output.rows.push_back(declaration.parameters.size() == 1U
                                  ? ValueTuple{{item}}
                                  : item.as_tuple());
        if (output.rows.size() > relation_row_limit) {
          throw Error("declared relation exceeds row budget");
        }
        return false;
      }, work);
  return Value(std::move(output));
}

bool visit_collection_expression(const ExprPtr& expression,
                                 Environment& environment,
                                 const CollectionVisitor& visitor,
                                 std::size_t& work) {
  if (expression->kind == Expr::Kind::Comprehension) {
    return visit_comprehension(expression, environment, visitor, work);
  }
  if (expression->kind == Expr::Kind::Name && active_relations != nullptr) {
    if (const auto found = active_relations->find(expression->text);
        found != active_relations->end()) {
      const Value relation = materialize_relation(found->second, environment);
      for (const ValueTuple& row : relation.as_relation().rows) {
        if (++work > relation_work_limit) throw Error("relation enumeration exceeds work budget");
        const Value item = found->second.type.direct_relation_row
                               ? row.fields.front()
                               : Value(row);
        if (visitor(item)) return true;
      }
      return false;
    }
  }
  const Value collection = evaluate(expression, environment);
  if (collection.kind() == Value::Kind::StringSet) {
    for (const std::string& item : collection.as_string_set().values) {
      if (++work > relation_work_limit) throw Error("collection exceeds work budget");
      if (visitor(Value(item))) return true;
    }
  } else if (collection.kind() == Value::Kind::Set) {
    for (const Value& item : collection.as_set().values) {
      if (++work > relation_work_limit) throw Error("collection exceeds work budget");
      if (visitor(item)) return true;
    }
  } else if (collection.kind() == Value::Kind::Relation) {
    for (const ValueTuple& row : collection.as_relation().rows) {
      if (++work > relation_work_limit) throw Error("relation exceeds work budget");
      const Value item = collection.as_relation().arity == 1U
                             ? row.fields.front()
                             : Value(row);
      if (visitor(item)) return true;
    }
  } else {
    throw Error("expression is not a finite collection");
  }
  return false;
}
