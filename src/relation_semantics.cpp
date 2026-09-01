// Lazy relation/comprehension semantics.  This private fragment sits between
// the typed frontend AST and the evaluator.  Membership stays predicate-based;
// enumeration is invoked only by a collection consumer or relation algebra.

using CollectionVisitor = std::function<bool(const Value&)>;

bool visit_collection_expression(const ExprPtr& expression,
                                 Environment& environment,
                                 const CollectionVisitor& visitor,
                                 std::size_t& work);

bool relation_membership(const RelationDeclaration& declaration,
                         const Value& subject, Environment& environment) {
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
