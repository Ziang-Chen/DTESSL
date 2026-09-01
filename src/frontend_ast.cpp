// Private frontend AST.  This file is textually included by frontend.cpp so
// parser representation stays out of the public ABI while no longer sharing
// a translation unit section with lexer/parser control flow.

struct Expr;
using ExprPtr = std::shared_ptr<Expr>;

struct ComprehensionClause {
  enum class Kind { Generator, Predicate };
  Kind kind{Kind::Predicate};
  std::string binding;
  ExprPtr expression;
  std::size_t line{0};
  std::size_t column{0};
};

struct MatchArm {
  std::string constructor;
  std::string binding;
  bool wildcard{false};
  ExprPtr body;
};

struct Expr {
  enum class Kind {
    Literal,
    Name,
    Unary,
    Binary,
    RelationMatch,
    Exists,
    Count,
    SetInsert,
    SetErase,
    ForAll,
    Select,
    OptionLiteral,
    NameConstruct,
    Construct,
    RecordConstruct,
    Match,
    Comprehension,
  };
  Kind kind{Kind::Literal};
  std::optional<Value> literal;
  std::string text;
  std::shared_ptr<Expr> left;
  std::shared_ptr<Expr> right;
  std::shared_ptr<Expr> third;
  std::vector<ExprPtr> children;
  std::vector<std::string> names;
  std::vector<MatchArm> arms;
  std::vector<ComprehensionClause> clauses;
  std::optional<DataType> type_argument;
  std::optional<DataType> resolved_type;
  std::size_t line{0};
  std::size_t column{0};
  bool direct_relation_binding{false};
  bool relation_expression{false};
};

// Surface-only anonymous relation pattern. Product preserves position and
// means conjunctive matching; Union is canonical and order-insensitive.
// A compact `Product -> Product` case denotes one intensional relation from a
// before Embedding to an atomically constructed successor Embedding.  It is
// not an imperative left-to-right assignment program. Verification lowers
// leaf predicates to the ordinary RelationMatch graph used by runtime/Solver.
struct AnonymousRelationPattern {
  enum class Kind { Predicate, Product, Union };
  Kind kind{Kind::Predicate};
  ExprPtr predicate;
  std::vector<std::shared_ptr<AnonymousRelationPattern>> children;
};
