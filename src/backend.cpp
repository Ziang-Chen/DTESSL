// Backend-neutral feature discovery and backend projection negotiation.
//
// This private textual fragment is included after the typed Program model. It
// deliberately depends on semantics, not parser syntax or solver internals.

namespace {

void collect_features(const ExprPtr& expr, FeatureSet& features) {
  if (!expr) return;
  if (expr->relation_expression) {
    features.insert(LanguageFeature::RelationExpression);
  }
  switch (expr->kind) {
    case Expr::Kind::Exists: features.insert(LanguageFeature::ExistentialSearch); break;
    case Expr::Kind::ForAll: features.insert(LanguageFeature::UniversalSearch); break;
    case Expr::Kind::Select: features.insert(LanguageFeature::DeterministicSelect); break;
    case Expr::Kind::SetInsert:
    case Expr::Kind::SetErase: features.insert(LanguageFeature::PureSetUpdate); break;
    case Expr::Kind::RecordConstruct:
      features.insert(LanguageFeature::AlgebraicDataTypes);
      features.insert(LanguageFeature::NominalTypes);
      break;
    case Expr::Kind::NameConstruct:
      features.insert(LanguageFeature::NominalTypes);
      features.insert(LanguageFeature::LogicalNames);
      break;
    case Expr::Kind::OptionLiteral:
      features.insert(LanguageFeature::AlgebraicDataTypes);
      break;
    case Expr::Kind::Construct:
      if (expr->text == "tuple" || expr->text == "project" ||
          expr->text == "join" || expr->text == "compose" ||
          expr->text == "inverse" || expr->text == "closure" ||
          expr->text == "union" || expr->text == "intersection" ||
          expr->text == "difference" || expr->text == "domain" ||
          expr->text == "range" || expr->text == "product" ||
          expr->text == "identity" || expr->text == "image" ||
          expr->text == "preimage" || expr->text == "reflexive_closure" ||
          expr->text == "subset" || expr->text == "disjoint" ||
          expr->text == "functional" || expr->text == "injective" ||
          expr->text == "reflexive" || expr->text == "irreflexive" ||
          expr->text == "symmetric" || expr->text == "antisymmetric" ||
          expr->text == "transitive" || expr->text == "acyclic" ||
          expr->text == "equivalence" || expr->text == "partial_order" ||
          expr->text == "left_total" || expr->text == "surjective" ||
          expr->text == "bijective" || expr->text == "total_order") {
        features.insert(LanguageFeature::RelationAlgebra);
        if (expr->text == "subset" || expr->text == "disjoint" ||
            expr->text == "functional" || expr->text == "injective" ||
            expr->text == "reflexive" || expr->text == "irreflexive" ||
            expr->text == "symmetric" || expr->text == "antisymmetric" ||
            expr->text == "transitive" || expr->text == "acyclic" ||
            expr->text == "equivalence" || expr->text == "partial_order" ||
            expr->text == "left_total" || expr->text == "surjective" ||
            expr->text == "bijective" || expr->text == "total_order") {
          features.insert(LanguageFeature::RelationProperties);
        }
      } else {
        features.insert(LanguageFeature::AlgebraicDataTypes);
        features.insert(LanguageFeature::NominalTypes);
      }
      break;
    case Expr::Kind::Match:
      features.insert(LanguageFeature::AlgebraicDataTypes);
      features.insert(LanguageFeature::ExhaustiveMatch);
      break;
    case Expr::Kind::Comprehension:
      features.insert(LanguageFeature::FiniteCollections);
      features.insert(LanguageFeature::RelationAlgebra);
      break;
    case Expr::Kind::Literal:
    case Expr::Kind::Name:
    case Expr::Kind::Unary:
    case Expr::Kind::Binary:
    case Expr::Kind::RelationMatch:
    case Expr::Kind::Count: break;
  }
  collect_features(expr->left, features);
  collect_features(expr->right, features);
  collect_features(expr->third, features);
  for (const ExprPtr& child : expr->children) collect_features(child, features);
  for (const MatchArm& arm : expr->arms) collect_features(arm.body, features);
  for (const ComprehensionClause& clause : expr->clauses) {
    collect_features(clause.expression, features);
  }
}

void collect_temporal_features(const TemporalExprPtr& expression,
                               FeatureSet& features) {
  if (!expression) return;
  features.insert(LanguageFeature::TemporalLogic);
  collect_features(expression->atom, features);
  collect_temporal_features(expression->left, features);
  collect_temporal_features(expression->right, features);
}

void collect_action_features(const std::shared_ptr<ActionExpr>& action,
                             FeatureSet& features) {
  if (!action) return;
  features.insert(LanguageFeature::ActionDag);
  for (const ExprPtr& argument : action->arguments) collect_features(argument, features);
  for (const auto& child : action->children) collect_action_features(child, features);
}

void collect_type_features(const DataType& type, FeatureSet& features) {
  if (!type.specific_trait.empty() || type.finite_domain) {
    features.insert(LanguageFeature::FiniteDomains);
  }
  if (type.kind == DataType::Kind::List || type.kind == DataType::Kind::Set ||
      type.kind == DataType::Kind::Map || type.kind == DataType::Kind::Bag) {
    features.insert(LanguageFeature::FiniteCollections);
  }
  if (type.kind == DataType::Kind::Tuple || type.kind == DataType::Kind::Relation) {
    features.insert(LanguageFeature::RelationAlgebra);
  }
  if (type.kind == DataType::Kind::Relation &&
      std::any_of(type.elements.begin(), type.elements.end(),
                  [](const DataType& element) {
                    return element.kind == DataType::Kind::Relation;
                  })) {
    features.insert(LanguageFeature::HigherOrderRelations);
  }
  if (type.kind == DataType::Kind::Relation && type.direct_relation_row) {
    features.insert(LanguageFeature::DirectRelationBinding);
  }
  if (type.kind == DataType::Kind::Named) features.insert(LanguageFeature::NominalTypes);
  if (type.kind == DataType::Kind::Int || type.kind == DataType::Kind::Rational) {
    features.insert(LanguageFeature::ExactNumeric);
  }
  if (type.kind == DataType::Kind::Option || type.kind == DataType::Kind::Result) {
    features.insert(LanguageFeature::AlgebraicDataTypes);
  }
  if (type.first) collect_type_features(*type.first, features);
  if (type.second) collect_type_features(*type.second, features);
  for (const DataType& element : type.elements) collect_type_features(element, features);
}

void collect_search_plans(const ExprPtr& expr, std::vector<SearchPlanSummary>& plans) {
  if (!expr) return;
  if (expr->kind == Expr::Kind::Exists) {
    plans.push_back({"exists", relation_work_limit, relation_work_limit, true, false});
  } else if (expr->kind == Expr::Kind::ForAll) {
    plans.push_back({"forall", relation_work_limit, relation_work_limit, true, false});
  } else if (expr->kind == Expr::Kind::Select) {
    plans.push_back({"select-by-lex", relation_row_limit, relation_row_limit, true, true});
  } else if (expr->kind == Expr::Kind::Construct &&
             (expr->text == "project" || expr->text == "join" ||
              expr->text == "compose" || expr->text == "inverse" ||
              expr->text == "closure" || expr->text == "union" ||
              expr->text == "intersection" || expr->text == "difference" ||
              expr->text == "domain" || expr->text == "range" ||
              expr->text == "product" || expr->text == "identity" ||
              expr->text == "image" || expr->text == "preimage" ||
              expr->text == "reflexive_closure" || expr->text == "subset" ||
              expr->text == "disjoint" || expr->text == "functional" ||
              expr->text == "injective" || expr->text == "reflexive" ||
              expr->text == "irreflexive" || expr->text == "symmetric" ||
              expr->text == "antisymmetric" || expr->text == "transitive" ||
              expr->text == "acyclic" || expr->text == "equivalence" ||
              expr->text == "partial_order" || expr->text == "left_total" ||
              expr->text == "surjective" || expr->text == "bijective" ||
              expr->text == "total_order")) {
    plans.push_back({expr->text, relation_row_limit, relation_work_limit, true, false});
  }
  collect_search_plans(expr->left, plans);
  collect_search_plans(expr->right, plans);
  collect_search_plans(expr->third, plans);
  for (const ExprPtr& child : expr->children) collect_search_plans(child, plans);
  for (const MatchArm& arm : expr->arms) collect_search_plans(arm.body, plans);
  for (const ComprehensionClause& clause : expr->clauses) {
    collect_search_plans(clause.expression, plans);
  }
}

void collect_action_search_plans(const std::shared_ptr<ActionExpr>& action,
                                 std::vector<SearchPlanSummary>& plans) {
  if (!action) return;
  for (const ExprPtr& argument : action->arguments) collect_search_plans(argument, plans);
  for (const auto& child : action->children) collect_action_search_plans(child, plans);
}

}  // namespace

FeatureSet required_features(const Program& program) {
  if (program.empty()) throw Error("cannot inspect features of an empty program");
  FeatureSet result{LanguageFeature::TypedState, LanguageFeature::ParallelEventBag};
  const Program::Impl& implementation = *program.implementation();
  if (std::any_of(implementation.state_schemas.begin(),
                  implementation.state_schemas.end(),
                  [](const StateSchema& schema) {
                    return std::any_of(
                        schema.root.children.begin(), schema.root.children.end(),
                        [](const StateSchemaNode& node) {
                          return node.kind == StateSchemaNode::Kind::Choice;
                        });
                  })) {
    result.insert(LanguageFeature::RecursiveStateSchema);
  }
  if (!implementation.action_ports.empty()) result.insert(LanguageFeature::TypedActionPorts);
  if (!implementation.types.empty()) result.insert(LanguageFeature::NominalTypes);
  for (const auto& [name, definition] : implementation.types) {
    static_cast<void>(name);
    if (definition.kind == TypeDefinition::Kind::Variant) {
      result.insert(LanguageFeature::AlgebraicDataTypes);
    }
    if (definition.kind == TypeDefinition::Kind::Name) {
      result.insert(LanguageFeature::LogicalNames);
    }
    if (definition.underlying) collect_type_features(*definition.underlying, result);
    for (const TypeField& field : definition.fields) collect_type_features(field.type, result);
    for (const VariantConstructor& constructor : definition.constructors) {
      if (constructor.payload) collect_type_features(*constructor.payload, result);
    }
  }
  for (const State& state : implementation.states) {
    for (const Field& field : state.fields) {
      collect_type_features(field.type, result);
      if (field.merge == Field::Merge::Equal) result.insert(LanguageFeature::EqualMerge);
      if (field.merge == Field::Merge::Union) result.insert(LanguageFeature::UnionMerge);
    }
    for (const ExprPtr& invariant : state.invariants) collect_features(invariant, result);
  }
  for (const auto& [name, function] : implementation.functions) {
    static_cast<void>(name);
    for (const Parameter& parameter : function.parameters) {
      collect_type_features(parameter.type, result);
    }
    collect_type_features(function.result, result);
    collect_features(function.body, result);
  }
  for (const auto& [name, relation] : implementation.relations) {
    static_cast<void>(name);
    collect_type_features(relation.type, result);
    result.insert(LanguageFeature::RelationAlgebra);
    if (relation.derived_expression) {
      result.insert(LanguageFeature::DerivedRelations);
    }
    collect_features(relation.derived_expression, result);
    for (const ComprehensionClause& clause : relation.clauses) {
      collect_features(clause.expression, result);
    }
  }
  if (!implementation.transition_relations.empty()) {
    result.insert(LanguageFeature::TransitionRelations);
    for (const auto& [name, relation] : implementation.transition_relations) {
      static_cast<void>(name);
      collect_features(relation.condition, result);
      for (const TransitionTarget& target : relation.to) {
        for (const Assignment& assignment : target.assignments) {
          collect_features(assignment.value, result);
        }
      }
      for (const TransitionAlternative& alternative : relation.alternatives) {
        collect_features(alternative.condition, result);
        for (const TransitionTarget& target : alternative.to) {
          for (const Assignment& assignment : target.assignments) {
            collect_features(assignment.value, result);
          }
        }
      }
    }
  }
  for (const Transition& transition : implementation.transitions) {
    if (transition.relation_surface) {
      result.insert(LanguageFeature::TransitionRelations);
    }
    collect_features(transition.condition, result);
    collect_temporal_features(transition.obligation, result);
    if (!transition.procedure_scope.empty()) {
      result.insert(LanguageFeature::ProcedureLambda);
    }
    if (transition.optimized_score) {
      result.insert(LanguageFeature::OptimizedTransition);
      collect_features(transition.optimized_score, result);
    }
    const auto collect_targets = [&](const std::vector<TransitionTarget>& targets) {
      for (const TransitionTarget& target : targets) {
        for (const Assignment& assignment : target.assignments) {
          collect_features(assignment.value, result);
        }
      }
    };
    collect_targets(transition.to);
    for (const TransitionAlternative& alternative : transition.alternatives) {
      collect_features(alternative.condition, result);
      collect_temporal_features(alternative.obligation, result);
      collect_targets(alternative.to);
      collect_action_features(alternative.action, result);
    }
    collect_action_features(transition.action, result);
  }
  if (std::any_of(implementation.transitions.begin(), implementation.transitions.end(),
                  [](const Transition& transition) {
                    return transition.from.size() > 1U || transition.to.size() > 1U ||
                           !transition.alternatives.empty();
                  })) {
    result.insert(LanguageFeature::CompositeStateSet);
  }
  if (!implementation.traces.empty()) result.insert(LanguageFeature::TypedTrace);
  if (std::any_of(implementation.traces.begin(), implementation.traces.end(),
                  [](const TraceDeclaration& trace) {
                    return trace.temporal_rule.has_value();
                  })) {
    result.insert(LanguageFeature::TemporalLogic);
  }
  if (!implementation.claims.empty()) result.insert(LanguageFeature::TraceClaims);
  for (const ClaimDeclaration& claim : implementation.claims) {
    if (claim.target_kind == ClaimDeclaration::TargetKind::Relation) {
      result.insert(LanguageFeature::RelationClaims);
    }
    collect_temporal_features(claim.property, result);
  }
  if (std::any_of(implementation.transitions.begin(),
                  implementation.transitions.end(),
                  [](const Transition& transition) {
                    if (transition.obligation) return true;
                    return std::any_of(
                        transition.alternatives.begin(),
                        transition.alternatives.end(),
                        [](const TransitionAlternative& alternative) {
                          return static_cast<bool>(alternative.obligation);
                        });
                  })) {
    result.insert(LanguageFeature::TransitionObligation);
  }
  if (!implementation.functions.empty()) result.insert(LanguageFeature::PureFunctions);
  if (!implementation.procedures.empty()) result.insert(LanguageFeature::ProcedureEntry);
  return result;
}

std::vector<SearchPlanSummary> search_plans(const Program& program) {
  if (program.empty()) throw Error("cannot inspect an empty program");
  const Program::Impl& implementation = *program.implementation();
  std::vector<SearchPlanSummary> plans{
      {"transition-id-index", implementation.transitions.size(), 1U, true, false},
      {"state-signature-index", implementation.transitions.size(),
       implementation.transitions.size(), true, false}};
  for (const State& state : implementation.states) {
    for (const ExprPtr& invariant : state.invariants) collect_search_plans(invariant, plans);
  }
  for (const auto& [name, function] : implementation.functions) {
    static_cast<void>(name);
    collect_search_plans(function.body, plans);
  }
  for (const auto& [name, relation] : implementation.relations) {
    static_cast<void>(name);
    plans.push_back({relation.derived_expression
                         ? "derived-relation-plan"
                         : (relation.enumerable ? "typed-relation-enumeration"
                                                : "typed-relation-membership"),
                     relation.enumerable ? relation_row_limit : 1U,
                     relation_work_limit, true, false});
    collect_search_plans(relation.derived_expression, plans);
    for (const ComprehensionClause& clause : relation.clauses) {
      collect_search_plans(clause.expression, plans);
    }
  }
  for (const auto& [name, relation] : implementation.transition_relations) {
    static_cast<void>(name);
    plans.push_back({"named-transition-relation",
                     1U + relation.alternatives.size(),
                     1U + relation.alternatives.size(), true, false});
    collect_search_plans(relation.condition, plans);
    for (const TransitionAlternative& alternative : relation.alternatives) {
      collect_search_plans(alternative.condition, plans);
    }
  }
  for (const Transition& transition : implementation.transitions) {
    if (transition.relation_surface) {
      plans.push_back({"transition-relation-union",
                       1U + transition.alternatives.size(),
                       1U + transition.alternatives.size(), true, false});
    }
    if (transition.optimized_score) {
      plans.push_back({"optimized-transition-max",
                       1U + transition.alternatives.size(),
                       1U + transition.alternatives.size(), true, true});
    }
    collect_search_plans(transition.optimized_score, plans);
    collect_search_plans(transition.condition, plans);
    const auto collect_targets = [&](const std::vector<TransitionTarget>& targets) {
      for (const TransitionTarget& target : targets) {
        for (const Assignment& assignment : target.assignments) {
          collect_search_plans(assignment.value, plans);
        }
      }
    };
    collect_targets(transition.to);
    for (const TransitionAlternative& alternative : transition.alternatives) {
      collect_search_plans(alternative.condition, plans);
      collect_targets(alternative.to);
      collect_action_search_plans(alternative.action, plans);
    }
    collect_action_search_plans(transition.action, plans);
  }
  return plans;
}

BackendCompatibility negotiate_backend(const Program& program,
                                       const BackendDescriptor& backend,
                                       Projection projection) {
  BackendCompatibility result;
  result.required = required_features(program);
  result.projection_supported = backend.projections.contains(projection);
  std::set_difference(result.required.begin(), result.required.end(),
                      backend.features.begin(), backend.features.end(),
                      std::inserter(result.missing, result.missing.end()));
  result.compatible = result.projection_supported && result.missing.empty();
  return result;
}

std::string_view feature_name(LanguageFeature feature) noexcept {
  switch (feature) {
    case LanguageFeature::TypedState: return "typed-state";
    case LanguageFeature::FiniteCollections: return "finite-collections";
    case LanguageFeature::ExistentialSearch: return "existential-search";
    case LanguageFeature::PureSetUpdate: return "pure-set-update";
    case LanguageFeature::ActionDag: return "action-dag";
    case LanguageFeature::ParallelEventBag: return "parallel-event-bag";
    case LanguageFeature::EqualMerge: return "equal-merge";
    case LanguageFeature::UnionMerge: return "union-merge";
    case LanguageFeature::AlgebraicDataTypes: return "algebraic-data-types";
    case LanguageFeature::NominalTypes: return "nominal-types";
    case LanguageFeature::ExhaustiveMatch: return "exhaustive-match";
    case LanguageFeature::ExactNumeric: return "exact-numeric";
    case LanguageFeature::RelationAlgebra: return "relation-algebra";
    case LanguageFeature::UniversalSearch: return "universal-search";
    case LanguageFeature::DeterministicSelect: return "deterministic-select";
    case LanguageFeature::TypedActionPorts: return "typed-action-ports";
    case LanguageFeature::LogicalNames: return "logical-names";
    case LanguageFeature::DirectRelationBinding: return "direct-relation-binding";
    case LanguageFeature::CompositeStateSet: return "composite-state-set";
    case LanguageFeature::TypedTrace: return "typed-trace";
    case LanguageFeature::TraceClaims: return "trace-claims";
    case LanguageFeature::PureFunctions: return "pure-functions";
    case LanguageFeature::ProcedureEntry: return "procedure-entry";
    case LanguageFeature::OptimizedTransition: return "optimized-transition";
    case LanguageFeature::TemporalLogic: return "temporal-logic";
    case LanguageFeature::ProcedureLambda: return "procedure-lambda";
    case LanguageFeature::TransitionObligation: return "transition-obligation";
    case LanguageFeature::RecursiveStateSchema: return "recursive-state-schema";
    case LanguageFeature::RelationExpression: return "relation-expression";
    case LanguageFeature::FiniteDomains: return "finite-domains";
    case LanguageFeature::DerivedRelations: return "derived-relations";
    case LanguageFeature::RelationProperties: return "relation-properties";
    case LanguageFeature::RelationClaims: return "relation-claims";
    case LanguageFeature::HigherOrderRelations: return "higher-order-relations";
    case LanguageFeature::TransitionRelations: return "transition-relations";
  }
  return "unknown";
}

std::string_view projection_name(Projection projection) noexcept {
  switch (projection) {
    case Projection::Execute: return "execute";
    case Projection::Monitor: return "monitor";
    case Projection::Explore: return "explore";
    case Projection::FormalExport: return "formal-export";
  }
  return "unknown";
}
