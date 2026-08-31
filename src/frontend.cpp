#include "dtessl/dtessl.hpp"
#include "dtessl/backend.hpp"
#include "dtessl/language_service.hpp"
#include "dtessl/solver.hpp"

#include <algorithm>
#include <charconv>
#include <cctype>
#include <functional>
#include <iterator>
#include <limits>
#include <optional>
#include <sstream>
#include <tuple>
#include <unordered_set>
#include <variant>

namespace dtessl {
namespace {

enum class TokenKind { Identifier, Integer, String, Symbol, Newline, Indent, Dedent, End };

struct Token {
  TokenKind kind;
  std::string text;
  std::size_t line;
  std::size_t column;
  std::size_t offset{0};
  std::size_t length{0};
};

[[noreturn]] void fail(const Token& token, const std::string& message) {
  throw Error(message, token.line, token.column);
}

SyntaxClass identifier_syntax(std::string_view text) {
  static const std::unordered_set<std::string_view> builtin_types{
      "bool", "int", "rational", "string", "list", "set", "map", "bag",
      "tuple", "relation", "option", "result"};
  static const std::unordered_set<std::string_view> keywords{
      "name", "record", "variant", "enum", "newtype", "port", "state", "initial",
      "invariant", "transition", "from", "to", "where", "do", "merge",
      "equal", "union", "and", "or", "not", "in", "exists", "select",
      "by", "lex", "match", "E", "A", "true", "false", "none", "some",
      "ok", "err", "round", "trace", "replay", "capture", "closed",
      "projected", "Claim", "always", "eventually", "until", "within",
      "since", "never", "before", "weak_until", "case", "function",
      "procedure", "initial", "inject", "when", "ensure", "trans",
      "optimized_score"};
  if (builtin_types.contains(text)) return SyntaxClass::BuiltinType;
  if (keywords.contains(text)) return SyntaxClass::Keyword;
  return SyntaxClass::Identifier;
}

SourceRange source_range(std::size_t line, std::size_t column,
                         std::size_t offset, std::size_t length) {
  return {{line, column, offset}, {line, column + length, offset + length}};
}

std::vector<Token> lex(std::string_view source,
                       std::vector<HighlightToken>* highlights = nullptr) {
  std::vector<Token> result;
  std::vector<std::size_t> indents{0};
  std::size_t offset = 0;
  std::size_t line_number = 1;
  std::size_t delimiter_depth = 0;

  const auto highlight = [&](SyntaxClass syntax, std::size_t column,
                             std::size_t absolute_offset, std::size_t length) {
    if (highlights != nullptr && length != 0) {
      highlights->push_back(
          {syntax, source_range(line_number, column, absolute_offset, length)});
    }
  };

  while (offset <= source.size()) {
    const std::size_t line_end = source.find('\n', offset);
    const std::size_t count = line_end == std::string_view::npos
                                  ? source.size() - offset
                                  : line_end - offset;
    std::string_view line = source.substr(offset, count);
    if (!line.empty() && line.back() == '\r') {
      line.remove_suffix(1);
    }

    std::size_t indent = 0;
    while (indent < line.size() && line[indent] == ' ') {
      ++indent;
    }
    if (indent < line.size() && line[indent] == '\t') {
      throw Error("tabs are not allowed for indentation", line_number, indent + 1);
    }

    std::size_t first = indent;
    while (first < line.size() && std::isspace(static_cast<unsigned char>(line[first])) != 0) {
      ++first;
    }
    const bool comment_only = first < line.size() && line.substr(first, 2) == "//";
    if (comment_only) {
      highlight(SyntaxClass::Comment, first + 1, offset + first, line.size() - first);
    }
    const bool blank = first == line.size() || comment_only;
    if (!blank) {
      if (delimiter_depth == 0) {
        if (indent > indents.back()) {
          indents.push_back(indent);
          result.push_back({TokenKind::Indent, "<indent>", line_number, 1, offset, 0});
        } else {
          while (indent < indents.back()) {
            indents.pop_back();
            result.push_back({TokenKind::Dedent, "<dedent>", line_number, 1, offset, 0});
          }
          if (indent != indents.back()) {
            throw Error("indentation does not match an outer block", line_number, 1);
          }
        }
      }

      std::size_t cursor = indent;
      while (cursor < line.size()) {
        const unsigned char ch = static_cast<unsigned char>(line[cursor]);
        if (std::isspace(ch) != 0) {
          ++cursor;
          continue;
        }
        if (line.substr(cursor, 2) == "//") {
          highlight(SyntaxClass::Comment, cursor + 1, offset + cursor,
                    line.size() - cursor);
          break;
        }
        const std::size_t column = cursor + 1;
        if (std::isalpha(ch) != 0 || ch == '_') {
          const std::size_t begin = cursor++;
          while (cursor < line.size()) {
            const unsigned char next = static_cast<unsigned char>(line[cursor]);
            if (std::isalnum(next) == 0 && next != '_') {
              break;
            }
            ++cursor;
          }
          const std::string text(line.substr(begin, cursor - begin));
          result.push_back({TokenKind::Identifier, text, line_number, column,
                            offset + begin, cursor - begin});
          highlight(identifier_syntax(text), column, offset + begin, cursor - begin);
          continue;
        }
        if (std::isdigit(ch) != 0) {
          const std::size_t begin = cursor++;
          while (cursor < line.size() &&
                 std::isdigit(static_cast<unsigned char>(line[cursor])) != 0) {
            ++cursor;
          }
          result.push_back({TokenKind::Integer, std::string(line.substr(begin, cursor - begin)),
                            line_number, column, offset + begin, cursor - begin});
          highlight(SyntaxClass::Number, column, offset + begin, cursor - begin);
          continue;
        }
        if (ch == '"') {
          const std::size_t begin = cursor;
          ++cursor;
          std::string value;
          bool closed = false;
          while (cursor < line.size()) {
            char item = line[cursor++];
            if (item == '"') {
              closed = true;
              break;
            }
            if (item == '\\') {
              if (cursor == line.size()) {
                throw Error("unfinished string escape", line_number, column);
              }
              const char escaped = line[cursor++];
              switch (escaped) {
                case 'n': value.push_back('\n'); break;
                case 'r': value.push_back('\r'); break;
                case 't': value.push_back('\t'); break;
                case '\\': value.push_back('\\'); break;
                case '"': value.push_back('"'); break;
                default: throw Error("unsupported string escape", line_number, cursor);
              }
            } else {
              value.push_back(item);
            }
          }
          if (!closed) {
            throw Error("unterminated string", line_number, column);
          }
          result.push_back({TokenKind::String, std::move(value), line_number, column,
                            offset + begin, cursor - begin});
          highlight(SyntaxClass::String, column, offset + begin, cursor - begin);
          continue;
        }

        const std::string_view two = line.substr(cursor, 2);
        if (two == "<=" || two == ">=" || two == "!=" || two == "==" || two == "->") {
          result.push_back({TokenKind::Symbol, std::string(two), line_number, column,
                            offset + cursor, 2});
          highlight(SyntaxClass::Operator, column, offset + cursor, 2);
          cursor += 2;
          continue;
        }
        static constexpr std::string_view symbols = "@:$,|;&(){}[]<>=+-.*~/";
        if (symbols.find(static_cast<char>(ch)) != std::string_view::npos) {
          if (ch == '(' || ch == '[' || ch == '{') {
            ++delimiter_depth;
          } else if (ch == ')' || ch == ']' || ch == '}') {
            if (delimiter_depth == 0) {
              throw Error("unmatched closing delimiter", line_number, column);
            }
            --delimiter_depth;
          }
          result.push_back({TokenKind::Symbol, std::string(1, static_cast<char>(ch)), line_number,
                            column, offset + cursor, 1});
          static constexpr std::string_view operators = "|<>=+-*/~";
          highlight(operators.find(static_cast<char>(ch)) != std::string_view::npos
                        ? SyntaxClass::Operator
                        : SyntaxClass::Punctuation,
                    column, offset + cursor, 1);
          ++cursor;
          continue;
        }
        throw Error("unexpected character", line_number, column);
      }
      if (delimiter_depth == 0) {
        result.push_back({TokenKind::Newline, "<newline>", line_number, line.size() + 1,
                          offset + line.size(), 0});
      }
    }

    if (line_end == std::string_view::npos) {
      break;
    }
    offset = line_end + 1;
    ++line_number;
  }

  while (indents.size() > 1) {
    indents.pop_back();
    result.push_back({TokenKind::Dedent, "<dedent>", line_number, 1,
                      source.size(), 0});
  }
  if (delimiter_depth != 0) throw Error("unterminated delimiter", line_number, 1);
  result.push_back({TokenKind::End, "<end>", line_number, 1, source.size(), 0});
  return result;
}

struct DataType {
  enum class Kind {
    Bool,
    Int,
    Rational,
    String,
    List,
    Set,
    Map,
    Bag,
    Tuple,
    Relation,
    Named,
    Option,
    Result,
  };

  Kind kind{Kind::Bool};
  std::string name;
  std::shared_ptr<DataType> first;
  std::shared_ptr<DataType> second;
  std::vector<DataType> elements;
  bool direct_relation_row{false};

  explicit DataType(Kind value = Kind::Bool) : kind(value) {}
  DataType(Kind value, DataType nested)
      : kind(value), first(std::make_shared<DataType>(std::move(nested))) {}
  DataType(Kind value, DataType key, DataType item)
      : kind(value),
        first(std::make_shared<DataType>(std::move(key))),
        second(std::make_shared<DataType>(std::move(item))) {}
  DataType(Kind value, std::string identity) : kind(value), name(std::move(identity)) {}
  DataType(Kind value, std::vector<DataType> fields)
      : kind(value), elements(std::move(fields)) {}

  friend bool operator==(const DataType& left, const DataType& right) {
    if (left.kind != right.kind || left.name != right.name ||
        left.direct_relation_row != right.direct_relation_row ||
        left.elements != right.elements) return false;
    if (static_cast<bool>(left.first) != static_cast<bool>(right.first) ||
        static_cast<bool>(left.second) != static_cast<bool>(right.second)) return false;
    return (!left.first || *left.first == *right.first) &&
           (!left.second || *left.second == *right.second);
  }
};

DataType bool_type() { return DataType(DataType::Kind::Bool); }
DataType int_type() { return DataType(DataType::Kind::Int); }
DataType rational_type() { return DataType(DataType::Kind::Rational); }
DataType string_type() { return DataType(DataType::Kind::String); }

struct TypeField {
  std::string name;
  DataType type;
};

struct VariantConstructor {
  std::string name;
  std::optional<DataType> payload;
};

struct TypeDefinition {
  enum class Kind { Record, Variant, Newtype, Name };
  Kind kind{Kind::Record};
  std::string name;
  std::vector<TypeField> fields;
  std::vector<VariantConstructor> constructors;
  std::optional<DataType> underlying;
};

using TypeRegistry = std::map<std::string, TypeDefinition, std::less<>>;

struct Expr;
using ExprPtr = std::shared_ptr<Expr>;

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
  std::optional<DataType> type_argument;
  std::optional<DataType> resolved_type;
  std::size_t line{0};
  std::size_t column{0};
  bool direct_relation_binding{false};
  bool relation_expression{false};
};

ExprPtr make_literal(Value value, std::size_t line = 0, std::size_t column = 0) {
  auto expr = std::make_shared<Expr>();
  expr->kind = Expr::Kind::Literal;
  expr->literal = std::move(value);
  expr->line = line;
  expr->column = column;
  return expr;
}

ExprPtr make_name(std::string name, std::size_t line = 0, std::size_t column = 0) {
  auto expr = std::make_shared<Expr>();
  expr->kind = Expr::Kind::Name;
  expr->text = std::move(name);
  expr->line = line;
  expr->column = column;
  return expr;
}

ExprPtr make_unary(std::string op, ExprPtr operand) {
  auto expr = std::make_shared<Expr>();
  expr->kind = Expr::Kind::Unary;
  expr->text = std::move(op);
  expr->left = std::move(operand);
  expr->line = expr->left ? expr->left->line : 0;
  expr->column = expr->left ? expr->left->column : 0;
  return expr;
}

ExprPtr make_binary(std::string op, ExprPtr left, ExprPtr right) {
  auto expr = std::make_shared<Expr>();
  expr->kind = Expr::Kind::Binary;
  expr->text = std::move(op);
  expr->left = std::move(left);
  expr->right = std::move(right);
  expr->line = expr->left ? expr->left->line : 0;
  expr->column = expr->left ? expr->left->column : 0;
  return expr;
}

ExprPtr make_relation_match(std::string relation, ExprPtr left, ExprPtr right) {
  auto expr = std::make_shared<Expr>();
  expr->kind = Expr::Kind::RelationMatch;
  expr->text = std::move(relation);
  expr->left = std::move(left);
  expr->right = std::move(right);
  expr->line = expr->left ? expr->left->line : 0;
  expr->column = expr->left ? expr->left->column : 0;
  return expr;
}

struct Field {
  enum class Merge { Reject, Equal, Union };
  std::string name;
  DataType type;
  Value initial;
  Merge merge{Merge::Reject};
  std::size_t line{0};
  std::size_t column{0};
};

using ContextId = std::size_t;
using StateId = std::size_t;
using TransitionId = std::size_t;
using RouteId = std::size_t;
constexpr std::size_t invalid_dense_id = std::numeric_limits<std::size_t>::max();

struct CaptureBinding {
  std::string trace;
  std::string session{"default"};

  friend bool operator<(const CaptureBinding& left,
                        const CaptureBinding& right) {
    return std::tie(left.trace, left.session) <
           std::tie(right.trace, right.session);
  }
};

using CaptureBindings = std::set<CaptureBinding>;

struct State {
  std::string name;
  std::string context;
  ContextId context_id{invalid_dense_id};
  StateId state_id{invalid_dense_id};
  bool initial{false};
  // Active semantic ancestors for a recursively declared control state.
  // They are expanded into transition patterns during verification.
  std::vector<std::string> ancestors;
  std::vector<Field> fields;
  std::vector<ExprPtr> invariants;
  CaptureBindings captures;
  std::size_t line{0};
  std::size_t column{0};
};

// Recursive semantic state schema.  Product children coexist, Choice children
// select exactly one Control alternative, and Value leaves carry typed state.
// Runtime dense state/context IDs are a lowering of this tree, not its meaning.
struct StateSchemaNode {
  enum class Kind { Product, Choice, Control, Value };
  Kind kind{Kind::Product};
  std::string name;
  std::string canonical_path;
  std::vector<StateSchemaNode> children;
  std::optional<Field> value;
  std::vector<ExprPtr> invariants;
  std::size_t line{0};
  std::size_t column{0};
};

struct StateSchema {
  std::string name;
  std::string context;
  bool initial{false};
  StateSchemaNode root;
  std::vector<ExprPtr> invariants;
  CaptureBindings captures;
  std::size_t line{0};
  std::size_t column{0};
};

struct ParsedStateDeclaration {
  StateSchema schema;
  std::vector<State> lowered_states;
};

struct Parameter {
  std::string name;
  DataType type;
  std::size_t line{0};
  std::size_t column{0};
};

struct FunctionDeclaration {
  std::string name;
  std::vector<Parameter> parameters;
  DataType result;
  ExprPtr body;
  std::size_t line{0};
  std::size_t column{0};
};

using FunctionRegistry = std::map<std::string, FunctionDeclaration, std::less<>>;

struct ActionPortDeclaration {
  std::string name;
  std::vector<DataType> parameters;
  std::size_t line{0};
  std::size_t column{0};
};

struct ActionExpr {
  enum class Kind { Call, Sequence, Parallel };
  Kind kind{Kind::Call};
  std::string label;
  std::string function;
  std::vector<ExprPtr> arguments;
  std::string context;
  std::vector<std::shared_ptr<ActionExpr>> children;
};

struct Assignment {
  std::string field;
  ExprPtr value;
  std::size_t line{0};
  std::size_t column{0};
};

struct StateBinding {
  std::string state;
  std::string context;
  std::size_t line{0};
  std::size_t column{0};
  ContextId context_id{invalid_dense_id};
  StateId state_id{invalid_dense_id};
};

struct TransitionTarget {
  StateBinding binding;
  std::vector<Assignment> assignments;
};

struct TransitionAlternative {
  std::string name;
  std::vector<StateBinding> from;
  std::vector<TransitionTarget> to;
  ExprPtr condition{make_literal(Value(true))};
  std::shared_ptr<ActionExpr> action;
  std::shared_ptr<struct TemporalExpr> obligation;
  std::set<std::string, std::less<>> reads;
  std::set<std::string, std::less<>> writes;
};

struct Transition {
  std::string name;
  // Empty for package transitions.  Anonymous transitions declared inside a
  // procedure are visible only to that procedure's automaton instance.
  std::string procedure_scope;
  // Optional declaration/observation context introduced by a postfix
  // extension header. It is not an Event alias or authority.
  std::string declaration_context;
  std::string event;
  std::string optimization_scope;
  ExprPtr optimized_score;
  std::vector<Parameter> parameters;
  std::string case_name;
  std::vector<StateBinding> from;
  std::vector<TransitionTarget> to;
  std::vector<TransitionAlternative> alternatives;
  ExprPtr condition{make_literal(Value(true))};
  std::shared_ptr<ActionExpr> action;
  std::shared_ptr<struct TemporalExpr> obligation;
  std::set<std::string, std::less<>> reads;
  std::set<std::string, std::less<>> writes;
  CaptureBindings captures;
  std::size_t line{0};
  std::size_t column{0};
};

// Temporal formulas deliberately wrap the ordinary typed predicate AST.  An
// Atom is evaluated by the same expression engine used by state invariants and
// transition guards; only the temporal structure is owned by ClaimMonitor.
struct TemporalExpr {
  enum class Kind {
    Atom,
    TraceRelationMatch,
    Not,
    And,
    Or,
    Always,
    Eventually,
    Until,
    Within,
    Since
  };
  Kind kind{Kind::Atom};
  std::string relation;
  ExprPtr atom;
  std::shared_ptr<TemporalExpr> left;
  std::shared_ptr<TemporalExpr> right;
  std::uint64_t bound{0};
  std::size_t line{0};
  std::size_t column{0};
};

using TemporalExprPtr = std::shared_ptr<TemporalExpr>;

TemporalExprPtr make_temporal_atom(ExprPtr atom) {
  auto result = std::make_shared<TemporalExpr>();
  result->kind = TemporalExpr::Kind::Atom;
  result->line = atom ? atom->line : 0;
  result->column = atom ? atom->column : 0;
  result->atom = std::move(atom);
  return result;
}

TemporalExprPtr make_temporal_unary(TemporalExpr::Kind kind,
                                    TemporalExprPtr operand) {
  auto result = std::make_shared<TemporalExpr>();
  result->kind = kind;
  result->line = operand ? operand->line : 0;
  result->column = operand ? operand->column : 0;
  result->left = std::move(operand);
  return result;
}

TemporalExprPtr make_temporal_binary(TemporalExpr::Kind kind,
                                     TemporalExprPtr left,
                                     TemporalExprPtr right) {
  auto result = std::make_shared<TemporalExpr>();
  result->kind = kind;
  result->line = left ? left->line : 0;
  result->column = left ? left->column : 0;
  result->left = std::move(left);
  result->right = std::move(right);
  return result;
}

TemporalExprPtr make_trace_relation_match(std::string relation,
                                          TemporalExprPtr subject_left,
                                          TemporalExprPtr subject_right) {
  auto result = make_temporal_binary(TemporalExpr::Kind::TraceRelationMatch,
                                     std::move(subject_left),
                                     std::move(subject_right));
  result->relation = std::move(relation);
  return result;
}

struct TraceDeclaration {
  std::string name;
  std::string root_context;
  bool has_replay{false};
  bool has_capture{false};
  TraceCaptureMode capture_mode{TraceCaptureMode::Closed};
  std::vector<StateBinding> capture;
  std::set<std::string, std::less<>> paths;
  std::set<std::string, std::less<>> captured_procedures;
  EventTrace events;
  std::vector<std::vector<std::string>> replay_paths;
  std::vector<std::vector<std::string>> replay_procedures;
  std::vector<std::vector<std::string>> replay_transitions;
  std::size_t line{0};
  std::size_t column{0};
};

struct ClaimDeclaration {
  enum class TargetKind { Trace, State, Procedure };
  std::string name;
  TargetKind target_kind{TargetKind::Trace};
  std::string target;
  std::vector<std::string> contexts;
  TemporalExprPtr property;
  bool count_at_most{false};
  std::string transition;
  std::uint64_t limit{0};
  std::size_t line{0};
  std::size_t column{0};
};

struct ProcedureDeclaration {
  std::string name;
  std::string initial_context;
  std::vector<StateBinding> initial_states;
  std::vector<Transition> anonymous_transitions;
  CaptureBindings captures;
  std::size_t line{0};
  std::size_t column{0};
};

// Semicolon-terminated, single-line quick-model syntax. These nodes preserve
// their source identity until Parser::program lowers them into the same typed
// State/Transition/Procedure/Trace AST used by the full surface language.
struct CompactStateDeclaration {
  std::vector<std::string> names;
  std::size_t line{0};
  std::size_t column{0};
};

struct CompactTransitionDeclaration {
  std::string family;
  std::string from;
  std::string to;
  ExprPtr condition{make_literal(Value(true))};
  std::size_t line{0};
  std::size_t column{0};
};

struct CompactFieldInitialization {
  std::string state;
  std::string field;
  DataType type;
  Value value;
  std::size_t line{0};
  std::size_t column{0};
};

struct CompactProcedureDeclaration {
  std::string name;
  std::vector<std::string> active_states;
  std::vector<CompactFieldInitialization> fields;
  std::string injection;
  std::size_t line{0};
  std::size_t column{0};
};

struct CompactTraceDeclaration {
  std::string procedure;
  std::size_t line{0};
  std::size_t column{0};
};

class FlatParser {
 public:
  FlatParser(std::vector<Token> tokens, const TypeRegistry& types)
      : tokens_(std::move(tokens)), types_(types) {}

  ExprPtr expression() { return parse_implication(); }

  std::shared_ptr<ActionExpr> action() {
    auto result = parse_parallel();
    expect_end();
    return result;
  }

  void expect_end() {
    if (!at_end()) {
      fail(peek(), "unexpected token '" + peek().text + "'");
    }
  }

 private:
  class RecursionGuard {
   public:
    RecursionGuard(std::size_t& depth, const Token& token,
                   std::string_view construct)
        : depth_(depth) {
      constexpr std::size_t recursion_depth_limit = 64U;
      if (depth_ >= recursion_depth_limit) {
        throw Error(std::string(construct) + " exceeds recursion depth limit",
                    token.line, token.column);
      }
      ++depth_;
    }

    ~RecursionGuard() { --depth_; }

    RecursionGuard(const RecursionGuard&) = delete;
    RecursionGuard& operator=(const RecursionGuard&) = delete;

   private:
    std::size_t& depth_;
  };

  const Token& peek() const {
    static const Token end{TokenKind::End, "<end>", 0, 0};
    return cursor_ < tokens_.size() ? tokens_[cursor_] : end;
  }

  [[nodiscard]] bool begins_type_argument() const {
    if (at_end() || peek().text != "<") return false;
    std::size_t depth = 0;
    for (std::size_t index = cursor_; index < tokens_.size(); ++index) {
      if (tokens_[index].text == "<") ++depth;
      else if (tokens_[index].text == ">") {
        if (depth == 0U) return false;
        --depth;
        if (depth == 0U) {
          return index + 1U < tokens_.size() &&
                 tokens_[index + 1U].text == "(";
        }
      }
      if (tokens_[index].kind == TokenKind::Newline ||
          tokens_[index].kind == TokenKind::End) return false;
    }
    return false;
  }

  bool at_end() const { return cursor_ >= tokens_.size() || peek().kind == TokenKind::End; }

  bool match(std::string_view text) {
    if (!at_end() && peek().text == text) {
      ++cursor_;
      return true;
    }
    return false;
  }

  Token take() {
    if (at_end()) {
      fail(peek(), "unexpected end of expression");
    }
    return tokens_[cursor_++];
  }

  Token expect(std::string_view text) {
    if (!match(text)) {
      fail(peek(), "expected '" + std::string(text) + "'");
    }
    return tokens_[cursor_ - 1];
  }

  Token identifier() {
    if (peek().kind != TokenKind::Identifier) {
      fail(peek(), "expected identifier");
    }
    return take();
  }

  Token path_component() {
    if (peek().kind != TokenKind::Identifier && peek().kind != TokenKind::Integer) {
      fail(peek(), "expected field name or tuple index");
    }
    return take();
  }

  DataType parse_type() {
    const RecursionGuard guard(type_depth_, peek(), "type");
    if (match("bool")) return bool_type();
    if (match("int")) return int_type();
    if (match("rational")) return rational_type();
    if (match("string")) return string_type();
    if (match("[")) {
      DataType item = parse_type();
      expect("]");
      return DataType(DataType::Kind::Option, std::move(item));
    }
    if (match("~")) {
      std::vector<DataType> elements;
      if (match("(")) {
        do {
          elements.push_back(parse_type());
        } while (match(","));
        expect(")");
      } else {
        elements.push_back(parse_type());
      }
      if (elements.empty() || elements.size() > relation_arity_limit) {
        fail(peek(), "relation type exceeds arity limit");
      }
      const bool direct = elements.size() == 1U;
      DataType relation(DataType::Kind::Relation, std::move(elements));
      relation.direct_relation_row = direct;
      return relation;
    }
    if (match("list") || match("set") || match("bag") || match("option")) {
      const std::string constructor = tokens_[cursor_ - 1].text;
      expect("<");
      DataType item = parse_type();
      expect(">");
      if (constructor == "list") return DataType(DataType::Kind::List, std::move(item));
      if (constructor == "set") return DataType(DataType::Kind::Set, std::move(item));
      if (constructor == "bag") return DataType(DataType::Kind::Bag, std::move(item));
      return DataType(DataType::Kind::Option, std::move(item));
    }
    if (match("map") || match("result")) {
      const std::string constructor = tokens_[cursor_ - 1].text;
      expect("<");
      DataType first = parse_type();
      expect(",");
      DataType second = parse_type();
      expect(">");
      return DataType(constructor == "map" ? DataType::Kind::Map : DataType::Kind::Result,
                      std::move(first), std::move(second));
    }
    if (match("tuple")) {
      expect("<");
      std::vector<DataType> elements;
      do {
        elements.push_back(parse_type());
        if (elements.size() > relation_arity_limit) {
          fail(peek(), "tuple/relation type exceeds arity limit");
        }
      } while (match(","));
      expect(">");
      return DataType(DataType::Kind::Tuple, std::move(elements));
    }
    if (match("relation")) {
      std::vector<DataType> elements;
      bool direct = true;
      if (match("<")) {
        direct = false;  // v0 tuple-row compatibility form
        do elements.push_back(parse_type()); while (match(","));
        expect(">");
      } else if (match("(")) {
        do elements.push_back(parse_type()); while (match(","));
        expect(")");
      } else {
        elements.push_back(parse_type());
      }
      if (elements.empty() || elements.size() > relation_arity_limit) {
        fail(peek(), "relation type exceeds arity limit");
      }
      DataType relation(DataType::Kind::Relation, std::move(elements));
      relation.direct_relation_row = direct && relation.elements.size() == 1U;
      return relation;
    }
    Token name = identifier();
    if (!types_.contains(name.text)) fail(name, "unknown nominal type '" + name.text + "'");
    return DataType(DataType::Kind::Named, std::move(name.text));
  }

  ExprPtr parse_implication() {
    const RecursionGuard guard(expression_depth_, peek(), "expression");
    auto left = parse_or();
    if (match("->")) {
      return make_binary("->", std::move(left), parse_implication());
    }
    return left;
  }

  ExprPtr parse_or() {
    auto left = parse_and();
    while (match("or")) {
      left = make_binary("or", std::move(left), parse_and());
    }
    return left;
  }

  ExprPtr parse_and() {
    auto left = parse_compare();
    while (match("and")) {
      left = make_binary("and", std::move(left), parse_compare());
    }
    return left;
  }

  ExprPtr parse_compare() {
    auto left = parse_add();
    static const std::unordered_set<std::string> operators{
        "=", "==", "!=", "<", "<=", ">", ">=", "in"};
    if (!at_end() && operators.contains(peek().text)) {
      const std::string op = take().text;
      return make_relation_match(op, std::move(left), parse_add());
    }
    if (match("~")) {
      // `subject ~ R1, (R2 | R3)` recursively lowers to
      // subject~R1 and (subject~R2 or subject~R3).  The subject AST is shared
      // immutably; relation expressions never bind or search implicitly.
      const ExprPtr subject = left;
      std::size_t relation_depth = 0U;
      std::size_t relation_nodes = 0U;
      std::function<ExprPtr()> relation_or;
      std::function<ExprPtr()> relation_and;
      std::function<ExprPtr()> relation_atom;
      relation_atom = [&]() {
        constexpr std::size_t relation_depth_limit = 64U;
        constexpr std::size_t relation_node_limit = 4096U;
        if (++relation_nodes > relation_node_limit) {
          fail(peek(), "relation expression exceeds node limit");
        }
        if (match("(")) {
          if (++relation_depth > relation_depth_limit) {
            fail(peek(), "relation expression exceeds recursion depth limit");
          }
          ExprPtr nested = relation_or();
          expect(")");
          --relation_depth;
          return nested;
        }
        return make_relation_match("~", subject, parse_add());
      };
      relation_and = [&]() {
        ExprPtr result = relation_atom();
        while (match(",")) {
          result = make_binary("and", std::move(result), relation_atom());
        }
        return result;
      };
      relation_or = [&]() {
        ExprPtr result = relation_and();
        while (match("|")) {
          result = make_binary("or", std::move(result), relation_and());
        }
        return result;
      };
      ExprPtr result = relation_or();
      result->relation_expression = true;
      return result;
    }
    return left;
  }

  ExprPtr parse_add() {
    auto left = parse_multiply();
    while (!at_end() && (peek().text == "+" || peek().text == "-")) {
      const std::string op = take().text;
      left = make_binary(op, std::move(left), parse_multiply());
    }
    return left;
  }

  ExprPtr parse_multiply() {
    auto left = parse_unary();
    while (!at_end() && (peek().text == "*" || peek().text == "/")) {
      const std::string op = take().text;
      left = make_binary(op, std::move(left), parse_unary());
    }
    return left;
  }

  ExprPtr parse_unary() {
    if (match("not")) {
      return make_unary("not", parse_unary());
    }
    if (match("-")) {
      return make_unary("-", parse_unary());
    }
    return parse_primary();
  }

  ExprPtr parse_primary() {
    if (match("(")) {
      auto first = parse_implication();
      if (match(",")) {
        auto tuple = std::make_shared<Expr>();
        tuple->kind = Expr::Kind::Construct;
        tuple->text = "tuple";
        tuple->line = first->line;
        tuple->column = first->column;
        tuple->children.push_back(std::move(first));
        do tuple->children.push_back(parse_implication()); while (match(","));
        expect(")");
        return tuple;
      }
      expect(")");
      return first;
    }
    if (peek().text == "exists" || peek().text == "E" || peek().text == "all" ||
        peek().text == "A") {
      const Token start = take();
      const std::string quantifier = start.text;
      const std::string variable = identifier().text;
      if (!match("~")) expect("in");
      auto domain = parse_add();
      if (!match("where")) expect(":");
      auto predicate = parse_implication();
      auto result = std::make_shared<Expr>();
      result->kind = quantifier == "exists" || quantifier == "E"
                         ? Expr::Kind::Exists
                         : Expr::Kind::ForAll;
      result->text = variable;
      result->left = std::move(domain);
      result->right = std::move(predicate);
      result->line = start.line;
      result->column = start.column;
      return result;
    }
    if (peek().text == "select") {
      const Token start = take();
      auto result = std::make_shared<Expr>();
      result->kind = Expr::Kind::Select;
      result->text = identifier().text;
      if (!match("~")) expect("in");
      result->left = parse_add();
      if (!match("where")) expect(":");
      result->right = parse_implication();
      expect("by");
      expect("lex");
      expect("(");
      do {
        result->children.push_back(parse_implication());
      } while (match(","));
      expect(")");
      result->line = start.line;
      result->column = start.column;
      return result;
    }
    if (peek().text == "match") {
      const Token start = take();
      auto result = std::make_shared<Expr>();
      result->kind = Expr::Kind::Match;
      result->line = start.line;
      result->column = start.column;
      if (match("(")) {
        result->left = parse_implication();
        expect(")");
      } else {
        std::string scrutinee = identifier().text;
        while (match(".")) scrutinee += "." + path_component().text;
        result->left = make_name(std::move(scrutinee));
      }
      expect("{");
      do {
        MatchArm arm;
        if (match("[")) {
          if (match("]")) {
            arm.constructor = "none";
          } else {
            arm.constructor = "some";
            arm.binding = identifier().text;
            expect("]");
          }
        } else if (match("_")) {
          arm.wildcard = true;
        } else {
          arm.constructor = identifier().text;
          while (match(".")) arm.constructor += "." + identifier().text;
          if (match("(")) {
            arm.binding = identifier().text;
            expect(")");
          }
        }
        expect("->");
        arm.body = parse_implication();
        result->arms.push_back(std::move(arm));
      } while (match(","));
      expect("}");
      return result;
    }
    if (peek().text == "count") {
      const Token start = take();
      expect("(");
      auto result = std::make_shared<Expr>();
      result->kind = Expr::Kind::Count;
      result->left = parse_implication();
      expect(")");
      result->line = start.line;
      result->column = start.column;
      return result;
    }
    if (peek().text == "insert" || peek().text == "erase") {
      const Token start = take();
      const bool inserting = start.text == "insert";
      expect("(");
      auto result = std::make_shared<Expr>();
      result->kind = inserting ? Expr::Kind::SetInsert : Expr::Kind::SetErase;
      result->left = parse_implication();
      expect(",");
      result->right = parse_implication();
      expect(")");
      result->line = start.line;
      result->column = start.column;
      return result;
    }
    if (match("[")) {
      auto result = std::make_shared<Expr>();
      result->kind = Expr::Kind::OptionLiteral;
      if (!match("]")) {
        result->children.push_back(parse_implication());
        expect("]");
      }
      return result;
    }
    if (peek().kind == TokenKind::Integer) {
      const Token token = take();
      try {
        return make_literal(Value(ExactInt::parse(token.text)), token.line, token.column);
      } catch (const Error& error) {
        fail(token, error.what());
      }
    }
    if (peek().kind == TokenKind::String) {
      const Token token = take();
      return make_literal(Value(token.text), token.line, token.column);
    }
    if (peek().text == "true") {
      const Token token = take();
      return make_literal(Value(true), token.line, token.column);
    }
    if (peek().text == "false") {
      const Token token = take();
      return make_literal(Value(false), token.line, token.column);
    }
    Token name = identifier();
    std::string path = std::move(name.text);
    while (match(".")) {
      path += "." + path_component().text;
    }
    std::optional<DataType> type_argument;
    if (begins_type_argument() && match("<")) {
      type_argument = parse_type();
      expect(">");
    }
    if (match("{")) {
      auto result = std::make_shared<Expr>();
      result->kind = Expr::Kind::RecordConstruct;
      result->text = std::move(path);
      result->line = name.line;
      result->column = name.column;
      if (!match("}")) {
        do {
          result->names.push_back(identifier().text);
          expect(":");
          result->children.push_back(parse_implication());
        } while (match(","));
        expect("}");
      }
      return result;
    }
    if (match("(")) {
      auto result = std::make_shared<Expr>();
      result->kind = Expr::Kind::Construct;
      result->text = std::move(path);
      result->type_argument = std::move(type_argument);
      result->line = name.line;
      result->column = name.column;
      const auto named = types_.find(result->text);
      if (named != types_.end() && named->second.kind == TypeDefinition::Kind::Name) {
        result->kind = Expr::Kind::NameConstruct;
        if (match(")")) fail(peek(), "name constructor requires one atom");
        const Token atom = identifier();
        result->children.push_back(make_literal(Value(atom.text)));
        expect(")");
      } else if (!match(")")) {
        do {
          result->children.push_back(parse_implication());
        } while (match(","));
        expect(")");
      }
      return result;
    }
    if (type_argument) fail(peek(), "type argument requires a constructor call");
    if (const std::size_t dot = path.rfind('.'); dot != std::string::npos) {
      const auto definition = types_.find(path.substr(0, dot));
      if (definition != types_.end() &&
          definition->second.kind == TypeDefinition::Kind::Variant) {
        const std::string constructor = path.substr(dot + 1U);
        const auto found = std::find_if(
            definition->second.constructors.begin(), definition->second.constructors.end(),
            [&](const VariantConstructor& item) { return item.name == constructor; });
        if (found != definition->second.constructors.end() && !found->payload) {
          auto result = std::make_shared<Expr>();
          result->kind = Expr::Kind::Construct;
          result->text = std::move(path);
          result->line = name.line;
          result->column = name.column;
          return result;
        }
      }
    }
    return make_name(std::move(path), name.line, name.column);
  }

  std::shared_ptr<ActionExpr> parse_parallel() {
    std::vector<std::shared_ptr<ActionExpr>> children;
    children.push_back(parse_sequence());
    while (match("|")) {
      children.push_back(parse_sequence());
    }
    if (children.size() == 1) {
      return children.front();
    }
    auto result = std::make_shared<ActionExpr>();
    result->kind = ActionExpr::Kind::Parallel;
    result->children = std::move(children);
    return result;
  }

  std::shared_ptr<ActionExpr> parse_sequence() {
    std::vector<std::shared_ptr<ActionExpr>> children;
    children.push_back(parse_action_primary());
    while (match(",")) {
      children.push_back(parse_action_primary());
    }
    if (children.size() == 1) {
      return children.front();
    }
    auto result = std::make_shared<ActionExpr>();
    result->kind = ActionExpr::Kind::Sequence;
    result->children = std::move(children);
    return result;
  }

  std::shared_ptr<ActionExpr> parse_action_primary() {
    if (match("(")) {
      auto result = parse_parallel();
      expect(")");
      return result;
    }
    auto result = std::make_shared<ActionExpr>();
    result->kind = ActionExpr::Kind::Call;
    result->label = identifier().text;
    expect(":");
    expect("$");
    result->function = identifier().text;
    while (match(".")) {
      result->function += "." + identifier().text;
    }
    expect("(");
    if (!match(")")) {
      do {
        result->arguments.push_back(parse_implication());
      } while (match(","));
      expect(")");
    }
    if (match("@")) {
      result->context = identifier().text;
      while (match(".")) {
        result->context += "." + identifier().text;
      }
    }
    return result;
  }

  std::vector<Token> tokens_;
  const TypeRegistry& types_;
  std::size_t cursor_{0};
  std::size_t expression_depth_{0};
  std::size_t type_depth_{0};
};

// Compact functional temporal syntax, for example
// always(since(active, accepted)) or within(5, done).  Predicate leaves are
// parsed by FlatParser, so Claim, state and transition logic share one typed
// expression language rather than three similar parsers.
class TemporalParser {
 public:
  TemporalParser(std::vector<Token> tokens, const TypeRegistry& types)
      : tokens_(std::move(tokens)), types_(types) {}

  TemporalExprPtr expression() {
    if (tokens_.empty()) throw Error("empty temporal expression");
    return segment(0U, tokens_.size());
  }

 private:
  [[nodiscard]] std::vector<std::pair<std::size_t, std::size_t>> arguments(
      std::size_t begin, std::size_t end) const {
    std::vector<std::pair<std::size_t, std::size_t>> result;
    std::size_t depth = 0;
    std::size_t item = begin;
    for (std::size_t index = begin; index < end; ++index) {
      if (tokens_[index].text == "(" || tokens_[index].text == "[" ||
          tokens_[index].text == "{") {
        ++depth;
      } else if (tokens_[index].text == ")" || tokens_[index].text == "]" ||
                 tokens_[index].text == "}") {
        if (depth == 0U) fail(tokens_[index], "unmatched temporal delimiter");
        --depth;
      } else if (tokens_[index].text == "," && depth == 0U) {
        if (item == index) fail(tokens_[index], "empty temporal argument");
        result.emplace_back(item, index);
        item = index + 1U;
      }
    }
    if (depth != 0U) fail(tokens_[begin], "unterminated temporal argument");
    if (item == end) fail(tokens_[end - 1U], "empty temporal argument");
    result.emplace_back(item, end);
    return result;
  }

  [[nodiscard]] bool complete_call(std::size_t begin, std::size_t end) const {
    if (end - begin < 3U || tokens_[begin + 1U].text != "(" ||
        tokens_[end - 1U].text != ")") return false;
    std::size_t depth = 0;
    for (std::size_t index = begin + 1U; index < end; ++index) {
      if (tokens_[index].text == "(") ++depth;
      else if (tokens_[index].text == ")") {
        if (depth == 0U) return false;
        --depth;
        if (depth == 0U && index + 1U != end) return false;
      }
    }
    return depth == 0U;
  }

  TemporalExprPtr segment(std::size_t begin, std::size_t end) {
    if (begin >= end) throw Error("empty temporal expression");
    std::size_t delimiter_depth = 0U;
    std::size_t relation_operator = end;
    for (std::size_t index = begin; index < end; ++index) {
      if (tokens_[index].text == "(" || tokens_[index].text == "[" ||
          tokens_[index].text == "{") {
        ++delimiter_depth;
      } else if (tokens_[index].text == ")" || tokens_[index].text == "]" ||
                 tokens_[index].text == "}") {
        if (delimiter_depth == 0U) fail(tokens_[index], "unmatched temporal delimiter");
        --delimiter_depth;
      } else if (tokens_[index].text == "~" && delimiter_depth == 0U) {
        relation_operator = index;
        break;
      }
    }
    if (relation_operator != end && relation_operator + 2U == end &&
        tokens_[relation_operator + 1U].text == "happens_before") {
      if (relation_operator <= begin + 2U || tokens_[begin].text != "(" ||
          tokens_[relation_operator - 1U].text != ")") {
        fail(tokens_[relation_operator],
             "happens_before subject must be a pair '(first, second)'");
      }
      const auto subjects = arguments(begin + 1U, relation_operator - 1U);
      if (subjects.size() != 2U) {
        fail(tokens_[relation_operator], "happens_before needs two temporal subjects");
      }
      return make_trace_relation_match(
          "happens_before",
          segment(subjects[0].first, subjects[0].second),
          segment(subjects[1].first, subjects[1].second));
    }
    const std::string& name = tokens_[begin].text;
    static const std::unordered_set<std::string_view> temporal_calls{
        "always", "eventually", "until", "within", "since", "never",
        "before", "weak_until"};
    if (temporal_calls.contains(name) && complete_call(begin, end)) {
      const auto args = arguments(begin + 2U, end - 1U);
      const auto unary = [&](TemporalExpr::Kind kind) {
        if (args.size() != 1U) fail(tokens_[begin], name + " needs one argument");
        return make_temporal_unary(kind, segment(args[0].first, args[0].second));
      };
      const auto binary = [&](TemporalExpr::Kind kind) {
        if (args.size() != 2U) fail(tokens_[begin], name + " needs two arguments");
        return make_temporal_binary(kind,
                                    segment(args[0].first, args[0].second),
                                    segment(args[1].first, args[1].second));
      };
      if (name == "always") return unary(TemporalExpr::Kind::Always);
      if (name == "eventually") return unary(TemporalExpr::Kind::Eventually);
      if (name == "until") return binary(TemporalExpr::Kind::Until);
      if (name == "since") return binary(TemporalExpr::Kind::Since);
      if (name == "within") {
        if (args.size() != 2U || args[0].second - args[0].first != 1U ||
            tokens_[args[0].first].kind != TokenKind::Integer) {
          fail(tokens_[begin], "within needs an integer bound and one property");
        }
        std::uint64_t bound = 0;
        const Token& token = tokens_[args[0].first];
        const auto parsed = std::from_chars(token.text.data(),
                                            token.text.data() + token.text.size(), bound);
        if (parsed.ec != std::errc{} || parsed.ptr != token.text.data() + token.text.size()) {
          fail(token, "invalid within bound");
        }
        auto result = make_temporal_unary(
            TemporalExpr::Kind::Within, segment(args[1].first, args[1].second));
        result->bound = bound;
        return result;
      }
      if (name == "never") {
        if (args.size() != 1U) fail(tokens_[begin], "never needs one argument");
        return make_temporal_unary(
            TemporalExpr::Kind::Always,
            make_temporal_unary(TemporalExpr::Kind::Not,
                                segment(args[0].first, args[0].second)));
      }
      if (name == "before") {
        if (args.size() != 2U) fail(tokens_[begin], "before needs two arguments");
        return make_trace_relation_match(
            "happens_before",
            segment(args[0].first, args[0].second),
            segment(args[1].first, args[1].second));
      }
      // weak_until(p, q) is a library-level definition: (p until q) or
      // always(p).  The Solver sees only core operators.
      if (args.size() != 2U) fail(tokens_[begin], "weak_until needs two arguments");
      TemporalExprPtr left = segment(args[0].first, args[0].second);
      TemporalExprPtr right = segment(args[1].first, args[1].second);
      return make_temporal_binary(
          TemporalExpr::Kind::Or,
          make_temporal_binary(TemporalExpr::Kind::Until, left, right),
          make_temporal_unary(TemporalExpr::Kind::Always, std::move(left)));
    }
    std::vector<Token> atom(tokens_.begin() + static_cast<std::ptrdiff_t>(begin),
                            tokens_.begin() + static_cast<std::ptrdiff_t>(end));
    FlatParser parser(std::move(atom), types_);
    ExprPtr expression = parser.expression();
    parser.expect_end();
    return make_temporal_atom(std::move(expression));
  }

  std::vector<Token> tokens_;
  const TypeRegistry& types_;
};

class Parser {
 public:
  explicit Parser(std::vector<Token> tokens) : tokens_(std::move(tokens)) {}

  std::shared_ptr<Program::Impl> program();

 private:
  const Token& peek() const { return tokens_[cursor_]; }
  const Token& peek(std::size_t offset) const {
    return tokens_[std::min(cursor_ + offset, tokens_.size() - 1U)];
  }
  bool at(TokenKind kind) const { return peek().kind == kind; }
  bool at(std::string_view text) const { return peek().text == text; }

  Token take() { return tokens_[cursor_++]; }
  bool match(std::string_view text) {
    if (at(text)) {
      ++cursor_;
      return true;
    }
    return false;
  }
  void expect(std::string_view text) {
    if (!match(text)) {
      fail(peek(), "expected '" + std::string(text) + "'");
    }
  }
  void expect(TokenKind kind, std::string_view description) {
    if (!at(kind)) {
      fail(peek(), "expected " + std::string(description));
    }
    ++cursor_;
  }
  std::string identifier() {
    if (!at(TokenKind::Identifier)) {
      fail(peek(), "expected identifier");
    }
    return take().text;
  }
  void newline() { expect(TokenKind::Newline, "end of line"); }
  void indent() { expect(TokenKind::Indent, "an indented block"); }
  void dedent() { expect(TokenKind::Dedent, "end of indented block"); }

  DataType type();
  Value initial_value(DataType type);
  TypeDefinition type_definition();
  FunctionDeclaration function_declaration();
  ActionPortDeclaration action_port_declaration();
  ParsedStateDeclaration state();
  Transition transition();
  TraceDeclaration trace(const std::vector<Transition>& transitions);
  ClaimDeclaration claim();
  ProcedureDeclaration procedure();
  Transition anonymous_transition(std::string_view procedure,
                                  std::size_t ordinal);
  CompactStateDeclaration compact_state();
  CompactTransitionDeclaration compact_transition();
  CompactProcedureDeclaration compact_procedure();
  CompactTraceDeclaration compact_trace();
  void lower_compact(Program::Impl& program,
                     const std::vector<CompactStateDeclaration>& states,
                     const std::vector<CompactTransitionDeclaration>& transitions,
                     const std::vector<CompactProcedureDeclaration>& procedures,
                     const std::vector<CompactTraceDeclaration>& traces);
  ExprPtr line_expression();
  TemporalExprPtr line_temporal_expression();
  TemporalExprPtr block_temporal_expression();
  ExprPtr block_expression();
  std::shared_ptr<ActionExpr> block_action();
  std::vector<Token> take_block_tokens();

  struct DeclarationExtensions {
    CaptureBindings captures;
    ExprPtr optimized_score;
  };
  DeclarationExtensions declaration_extensions(bool allow_optimizer);

  std::vector<Token> tokens_;
  std::size_t cursor_{0};
  TypeRegistry types_;
};

}  // namespace

struct RouteStateIndexBucket {
  std::vector<std::string> contexts;
  std::map<std::vector<std::string>, std::vector<std::size_t>> routes;
};

struct DenseRouteStateIndexBucket {
  std::vector<ContextId> contexts;
  std::map<std::vector<StateId>, std::vector<RouteId>> routes;
};

struct Program::Impl {
  TypeRegistry types;
  FunctionRegistry functions;
  std::vector<ActionPortDeclaration> action_ports;
  std::vector<State> states;
  std::vector<StateSchema> state_schemas;
  std::vector<Transition> transitions;
  std::vector<TraceDeclaration> traces;
  std::vector<ClaimDeclaration> claims;
  std::map<std::string, ProcedureDeclaration, std::less<>> procedures;
  std::map<std::string, ContextId, std::less<>> context_index;
  std::vector<std::string> context_names;
  std::map<std::string, StateId, std::less<>> state_index;
  std::vector<std::size_t> state_positions_by_id;
  RawKeyMap raw_key_map;
  std::map<std::string, TransitionId, std::less<>> transition_index;
  std::map<std::string, std::vector<TransitionId>, std::less<>> event_index;
  std::map<std::string, std::vector<RouteStateIndexBucket>, std::less<>>
      route_state_index;
  std::vector<std::vector<DenseRouteStateIndexBucket>> dense_route_state_index;
};

namespace {

thread_local const FunctionRegistry* active_functions = nullptr;

class FunctionScope {
 public:
  explicit FunctionScope(const FunctionRegistry& functions)
      : previous_(active_functions) {
    active_functions = &functions;
  }
  ~FunctionScope() { active_functions = previous_; }

 private:
  const FunctionRegistry* previous_;
};

DataType Parser::type() {
  if (match("bool")) return bool_type();
  if (match("int")) return int_type();
  if (match("rational")) return rational_type();
  if (match("string")) return string_type();
  if (match("[")) {
    DataType item = type();
    expect("]");
    return DataType(DataType::Kind::Option, std::move(item));
  }
  if (match("~")) {
    std::vector<DataType> elements;
    if (match("(")) {
      do {
        elements.push_back(type());
        if (elements.size() > relation_arity_limit) {
          fail(peek(), "relation type exceeds arity limit");
        }
      } while (match(","));
      expect(")");
    } else {
      elements.push_back(type());
    }
    const bool direct = elements.size() == 1U;
    DataType relation(DataType::Kind::Relation, std::move(elements));
    relation.direct_relation_row = direct;
    return relation;
  }
  if (match("list")) {
    expect("<");
    DataType item = type();
    expect(">");
    return DataType(DataType::Kind::List, std::move(item));
  }
  if (match("set")) {
    expect("<");
    DataType item = type();
    expect(">");
    return DataType(DataType::Kind::Set, std::move(item));
  }
  if (match("map")) {
    expect("<");
    DataType key = type();
    expect(",");
    DataType item = type();
    expect(">");
    return DataType(DataType::Kind::Map, std::move(key), std::move(item));
  }
  if (match("bag")) {
    expect("<");
    DataType item = type();
    expect(">");
    return DataType(DataType::Kind::Bag, std::move(item));
  }
  if (match("option")) {
    expect("<");
    DataType item = type();
    expect(">");
    return DataType(DataType::Kind::Option, std::move(item));
  }
  if (match("result")) {
    expect("<");
    DataType item = type();
    expect(",");
    DataType error = type();
    expect(">");
    return DataType(DataType::Kind::Result, std::move(item), std::move(error));
  }
  if (match("tuple")) {
    expect("<");
    std::vector<DataType> elements;
    do {
      elements.push_back(type());
      if (elements.size() > relation_arity_limit) {
        fail(peek(), "tuple/relation type exceeds arity limit");
      }
    } while (match(","));
    expect(">");
    return DataType(DataType::Kind::Tuple, std::move(elements));
  }
  if (match("relation")) {
    std::vector<DataType> elements;
    bool direct = true;
    if (match("<")) {
      direct = false;  // v0 tuple-row compatibility form
      do elements.push_back(type()); while (match(","));
      expect(">");
    } else if (match("(")) {
      do elements.push_back(type()); while (match(","));
      expect(")");
    } else {
      elements.push_back(type());
    }
    if (elements.empty() || elements.size() > relation_arity_limit) {
      fail(peek(), "relation type exceeds arity limit");
    }
    DataType relation(DataType::Kind::Relation, std::move(elements));
    relation.direct_relation_row = direct && relation.elements.size() == 1U;
    return relation;
  }
  if (at(TokenKind::Identifier)) {
    const Token name = take();
    if (!types_.contains(name.text)) fail(name, "unknown nominal type '" + name.text + "'");
    return DataType(DataType::Kind::Named, name.text);
  }
  fail(peek(), "expected a primitive, collection, tuple, relation, option, result, or nominal type");
}

TypeDefinition Parser::type_definition() {
  TypeDefinition result;
  if (match("name")) {
    result.kind = TypeDefinition::Kind::Name;
    result.name = identifier();
    newline();
    return result;
  }
  if (match("newtype")) {
    result.kind = TypeDefinition::Kind::Newtype;
    result.name = identifier();
    expect("=");
    result.underlying = type();
    newline();
    return result;
  }
  const bool record = match("record");
  const bool variant = !record && match("variant");
  const bool enumeration = !record && !variant && match("enum");
  if (!record && !variant && !enumeration) fail(peek(), "expected type declaration");
  result.kind = record ? TypeDefinition::Kind::Record : TypeDefinition::Kind::Variant;
  result.name = identifier();
  expect(":");
  newline();
  indent();
  std::unordered_set<std::string> members;
  while (!at(TokenKind::Dedent)) {
    if (record) {
      TypeField field;
      field.name = identifier();
      if (!members.insert(field.name).second) fail(peek(), "duplicate record field");
      expect(":");
      field.type = type();
      newline();
      result.fields.push_back(std::move(field));
    } else {
      VariantConstructor constructor;
      constructor.name = identifier();
      if (!members.insert(constructor.name).second) fail(peek(), "duplicate constructor");
      if (match("(")) {
        if (enumeration) fail(peek(), "enum constructors cannot carry payloads");
        constructor.payload = type();
        expect(")");
      }
      newline();
      result.constructors.push_back(std::move(constructor));
    }
  }
  dedent();
  if ((record && result.fields.empty()) || (!record && result.constructors.empty())) {
    fail(peek(), "type declaration must not be empty");
  }
  return result;
}

ActionPortDeclaration Parser::action_port_declaration() {
  const Token start = peek();
  expect("port");
  ActionPortDeclaration result;
  result.line = start.line;
  result.column = start.column;
  result.name = identifier();
  while (match(".")) result.name += "." + identifier();
  expect("(");
  if (!match(")")) {
    do {
      result.parameters.push_back(type());
    } while (match(","));
    expect(")");
  }
  newline();
  return result;
}

FunctionDeclaration Parser::function_declaration() {
  const Token start = peek();
  expect("function");
  FunctionDeclaration result;
  result.line = start.line;
  result.column = start.column;
  result.name = identifier();
  expect("(");
  if (!match(")")) {
    do {
      Parameter parameter;
      const Token parameter_start = peek();
      parameter.name = identifier();
      parameter.line = parameter_start.line;
      parameter.column = parameter_start.column;
      expect(":");
      parameter.type = type();
      result.parameters.push_back(std::move(parameter));
    } while (match(","));
    expect(")");
  }
  expect("->");
  result.result = type();
  expect(":");
  result.body = block_expression();
  return result;
}

std::string type_identity(const DataType& type) {
  const auto variadic_identity = [&](std::string_view constructor) {
    std::string result(constructor);
    result += "<";
    for (std::size_t index = 0; index < type.elements.size(); ++index) {
      if (index != 0) result += ",";
      result += type_identity(type.elements[index]);
    }
    return result + ">";
  };
  switch (type.kind) {
    case DataType::Kind::Bool: return "bool";
    case DataType::Kind::Int: return "int";
    case DataType::Kind::Rational: return "rational";
    case DataType::Kind::String: return "string";
    case DataType::Kind::Named: return type.name;
    case DataType::Kind::List: return "list<" + type_identity(*type.first) + ">";
    case DataType::Kind::Set: return "set<" + type_identity(*type.first) + ">";
    case DataType::Kind::Bag: return "bag<" + type_identity(*type.first) + ">";
    case DataType::Kind::Option: return "option<" + type_identity(*type.first) + ">";
    case DataType::Kind::Tuple: return variadic_identity("tuple");
    case DataType::Kind::Relation: return variadic_identity("relation");
    case DataType::Kind::Map:
      return "map<" + type_identity(*type.first) + "," + type_identity(*type.second) + ">";
    case DataType::Kind::Result:
      return "result<" + type_identity(*type.first) + "," + type_identity(*type.second) + ">";
  }
  throw Error("invalid type");
}

Value Parser::initial_value(DataType expected_type) {
  if (expected_type.kind == DataType::Kind::Bool) {
    if (match("true")) return Value(true);
    if (match("false")) return Value(false);
    fail(peek(), "expected boolean literal");
  }
  if (expected_type.kind == DataType::Kind::Int) {
    bool negative = match("-");
    if (!at(TokenKind::Integer)) fail(peek(), "expected integer literal");
    const Token token = take();
    try {
      ExactInt value = ExactInt::parse(token.text);
      return Value(negative ? -value : value);
    } catch (const Error& error) {
      fail(token, error.what());
    }
  }
  if (expected_type.kind == DataType::Kind::Rational) {
    Value numerator = initial_value(int_type());
    expect("/");
    Value denominator = initial_value(int_type());
    return Value(Rational(numerator.as_exact_int(), denominator.as_exact_int()));
  }
  if (expected_type.kind == DataType::Kind::String) {
    if (!at(TokenKind::String)) fail(peek(), "expected string literal");
    return Value(take().text);
  }
  if (expected_type.kind == DataType::Kind::List) {
    match("list");
    expect("[");
    ValueList value;
    if (!match("]")) {
      do {
        value.values.push_back(initial_value(*expected_type.first));
      } while (match(","));
      expect("]");
    }
    return Value(std::move(value));
  }
  if (expected_type.kind == DataType::Kind::Set) {
    expect("{");
    if (expected_type.first->kind == DataType::Kind::String) {
      StringSet value;
      if (!match("}")) {
        do {
          Value item = initial_value(*expected_type.first);
          if (!value.values.insert(item.as_string()).second) {
            fail(tokens_[cursor_ - 1], "duplicate set element");
          }
        } while (match(","));
        expect("}");
      }
      return Value(std::move(value));
    }
    ValueSet value;
    if (!match("}")) {
      do {
        value.values.push_back(initial_value(*expected_type.first));
      } while (match(","));
      expect("}");
    }
    return Value(std::move(value));
  }
  if (expected_type.kind == DataType::Kind::Map) {
    expect("{");
    ValueMap value;
    if (!match("}")) {
      do {
        Value key = initial_value(*expected_type.first);
        expect(":");
        Value item = initial_value(*expected_type.second);
        value.entries.emplace_back(std::move(key), std::move(item));
      } while (match(","));
      expect("}");
    }
    return Value(std::move(value));
  }
  if (expected_type.kind == DataType::Kind::Tuple) {
    expect("(");
    ValueTuple tuple;
    for (std::size_t index = 0; index < expected_type.elements.size(); ++index) {
      if (index != 0) expect(",");
      tuple.fields.push_back(initial_value(expected_type.elements[index]));
    }
    expect(")");
    return Value(std::move(tuple));
  }
  if (expected_type.kind == DataType::Kind::Relation) {
    if (!at("{")) {
      if (!match("~")) expect("relation");
    }
    expect("{");
    ValueRelation relation{expected_type.elements.size(), {}};
    const DataType row_type(DataType::Kind::Tuple, expected_type.elements);
    if (!match("}")) {
      do {
        if (relation.rows.size() == relation_row_limit) {
          fail(peek(), "relation literal exceeds row budget");
        }
        if (expected_type.direct_relation_row) {
          ValueTuple row;
          row.fields.push_back(initial_value(expected_type.elements.front()));
          relation.rows.push_back(std::move(row));
        } else {
          relation.rows.push_back(initial_value(row_type).as_tuple());
        }
      } while (match(","));
      expect("}");
    }
    return Value(std::move(relation));
  }
  if (expected_type.kind == DataType::Kind::Named) {
    const TypeDefinition& definition = types_.at(expected_type.name);
    expect(definition.name);
    if (definition.kind == TypeDefinition::Kind::Name) {
      expect("(");
      const std::string atom = identifier();
      expect(")");
      return Value(ValueName{definition.name, atom});
    }
    if (definition.kind == TypeDefinition::Kind::Newtype) {
      expect("(");
      Value item = initial_value(*definition.underlying);
      expect(")");
      return Value(ValueNewtype{definition.name, {std::move(item)}});
    }
    if (definition.kind == TypeDefinition::Kind::Record) {
      expect("{");
      ValueRecord record{definition.name, {}};
      std::map<std::string, const TypeField*, std::less<>> fields;
      for (const TypeField& field : definition.fields) fields.emplace(field.name, &field);
      std::unordered_set<std::string> seen;
      if (!match("}")) {
        do {
          const Token name = peek();
          const std::string field_name = identifier();
          const auto found = fields.find(field_name);
          if (found == fields.end()) fail(name, "unknown record field '" + field_name + "'");
          if (!seen.insert(field_name).second) fail(name, "duplicate record field");
          expect(":");
          record.fields.emplace_back(field_name, initial_value(found->second->type));
        } while (match(","));
        expect("}");
      }
      if (seen.size() != definition.fields.size()) fail(peek(), "record literal is missing fields");
      return Value(std::move(record));
    }
    expect(".");
    const Token constructor_token = peek();
    const std::string constructor_name = identifier();
    const auto found = std::find_if(
        definition.constructors.begin(), definition.constructors.end(),
        [&](const VariantConstructor& item) { return item.name == constructor_name; });
    if (found == definition.constructors.end()) {
      fail(constructor_token, "unknown constructor '" + constructor_name + "'");
    }
    ValueVariant variant{definition.name, constructor_name, {}};
    if (found->payload) {
      expect("(");
      variant.payload.push_back(initial_value(*found->payload));
      expect(")");
    }
    return Value(std::move(variant));
  }
  if (expected_type.kind == DataType::Kind::Option) {
    if (match("[")) {
      if (match("]")) {
        return Value(ValueVariant{type_identity(expected_type), "none", {}});
      }
      Value item = initial_value(*expected_type.first);
      expect("]");
      return Value(ValueVariant{type_identity(expected_type), "some", {std::move(item)}});
    }
    if (match("none")) {
      return Value(ValueVariant{type_identity(expected_type), "none", {}});
    }
    expect("some");
    expect("(");
    Value item = initial_value(*expected_type.first);
    expect(")");
    return Value(ValueVariant{type_identity(expected_type), "some", {std::move(item)}});
  }
  if (expected_type.kind == DataType::Kind::Result) {
    const bool success = match("ok");
    if (!success) expect("err");
    expect("(");
    Value item = initial_value(success ? *expected_type.first : *expected_type.second);
    expect(")");
    return Value(ValueVariant{type_identity(expected_type), success ? "ok" : "err",
                              {std::move(item)}});
  }
  expect("bag");
  expect("{");
  ValueBag value;
  if (!match("}")) {
    do {
      Value item = initial_value(*expected_type.first);
      expect(":");
      if (!at(TokenKind::Integer)) fail(peek(), "bag multiplicity must be a positive integer");
      const Token token = take();
      std::uint64_t count = 0;
      const auto parsed = std::from_chars(token.text.data(), token.text.data() + token.text.size(),
                                          count);
      if (parsed.ec != std::errc{} || count == 0) fail(token, "invalid bag multiplicity");
      value.entries.emplace_back(std::move(item), count);
    } while (match(","));
    expect("}");
  }
  return Value(std::move(value));
}

std::vector<Token> Parser::take_block_tokens() {
  newline();
  indent();
  std::vector<Token> result;
  int nested = 0;
  while (!(at(TokenKind::Dedent) && nested == 0)) {
    if (at(TokenKind::End)) fail(peek(), "unterminated block");
    Token token = take();
    if (token.kind == TokenKind::Indent) {
      ++nested;
    } else if (token.kind == TokenKind::Dedent) {
      --nested;
    } else if (token.kind != TokenKind::Newline) {
      result.push_back(std::move(token));
    }
  }
  dedent();
  return result;
}

ExprPtr Parser::line_expression() {
  std::vector<Token> flat;
  while (!at(TokenKind::Newline)) {
    if (at(TokenKind::End)) fail(peek(), "expected end of line");
    flat.push_back(take());
  }
  newline();
  FlatParser parser(std::move(flat), types_);
  auto result = parser.expression();
  parser.expect_end();
  return result;
}

TemporalExprPtr Parser::line_temporal_expression() {
  std::vector<Token> flat;
  while (!at(TokenKind::Newline)) {
    if (at(TokenKind::End)) fail(peek(), "expected end of temporal expression");
    flat.push_back(take());
  }
  newline();
  TemporalParser parser(std::move(flat), types_);
  return parser.expression();
}

TemporalExprPtr Parser::block_temporal_expression() {
  TemporalParser parser(take_block_tokens(), types_);
  return parser.expression();
}

ExprPtr Parser::block_expression() {
  FlatParser parser(take_block_tokens(), types_);
  auto result = parser.expression();
  parser.expect_end();
  return result;
}

std::shared_ptr<ActionExpr> Parser::block_action() {
  FlatParser parser(take_block_tokens(), types_);
  return parser.action();
}

Parser::DeclarationExtensions Parser::declaration_extensions(
    bool allow_optimizer) {
  DeclarationExtensions result;
  expect("[");
  bool first = true;
  while (!at("]")) {
    if (!first) expect(",");
    first = false;
    const Token key_token = peek();
    const std::string key = identifier();
    expect("=");
    if (key == "capture") {
      do {
        CaptureBinding binding;
        binding.trace = identifier();
        if (match("/")) binding.session = identifier();
        if (!result.captures.insert(binding).second) {
          fail(key_token, "capture extension repeats Trace/Session target");
        }
      } while (match("|"));
      continue;
    }
    if (key == "optimized_score") {
      if (!allow_optimizer) {
        fail(key_token, "optimized_score is valid only on a transition");
      }
      if (result.optimized_score) {
        fail(key_token, "extension repeats optimized_score");
      }
      std::vector<Token> expression_tokens;
      std::size_t depth = 0U;
      while (!(depth == 0U && (at(",") || at("]")))) {
        if (at(TokenKind::End) || at(TokenKind::Newline)) {
          fail(key_token, "unterminated optimized_score extension");
        }
        Token token = take();
        if (token.text == "(" || token.text == "[" || token.text == "{") {
          ++depth;
        } else if (token.text == ")" || token.text == "]" || token.text == "}") {
          if (depth == 0U) fail(token, "unbalanced optimized_score extension");
          --depth;
        }
        expression_tokens.push_back(std::move(token));
      }
      if (expression_tokens.empty()) {
        fail(key_token, "optimized_score needs an expression");
      }
      FlatParser parser(std::move(expression_tokens), types_);
      result.optimized_score = parser.expression();
      parser.expect_end();
      continue;
    }
    fail(key_token, "unknown declaration extension '" + key + "'");
  }
  expect("]");
  if (first) fail(peek(), "declaration extension cannot be empty");
  return result;
}

ParsedStateDeclaration Parser::state() {
  const Token start = peek();
  expect("state");
  ParsedStateDeclaration result;
  StateSchema& schema = result.schema;
  schema.line = start.line;
  schema.column = start.column;
  schema.name = identifier();
  if (match("@")) schema.context = identifier();
  bool saw_extensions = false;
  for (;;) {
    if (match("initial")) {
      if (schema.initial) fail(start, "state repeats initial");
      schema.initial = true;
      continue;
    }
    if (at("[")) {
      if (saw_extensions) fail(peek(), "state repeats declaration extensions");
      saw_extensions = true;
      schema.captures = declaration_extensions(false).captures;
      continue;
    }
    break;
  }
  schema.root.kind = StateSchemaNode::Kind::Product;
  schema.root.name = schema.name;
  schema.root.canonical_path = schema.name;
  schema.root.line = start.line;
  schema.root.column = start.column;

  const auto field_node = [&]() {
    StateSchemaNode node;
    node.kind = StateSchemaNode::Kind::Value;
    Field field{"", bool_type(), Value(false), Field::Merge::Reject};
    const Token field_start = peek();
    field.name = identifier();
    field.line = field_start.line;
    field.column = field_start.column;
    expect(":");
    field.type = type();
    expect("=");
    field.initial = initial_value(field.type);
    if (match("merge")) {
      if (match("equal")) field.merge = Field::Merge::Equal;
      else if (match("union")) field.merge = Field::Merge::Union;
      else fail(peek(), "expected equal or union merge relation");
      if (field.merge == Field::Merge::Union && field.type.kind != DataType::Kind::Set) {
        fail(tokens_[cursor_ - 1], "union merge needs a set type");
      }
    }
    node.name = field.name;
    node.value = std::move(field);
    node.line = field_start.line;
    node.column = field_start.column;
    return node;
  };

  const auto inline_invariant = [&]() {
    const Token invariant_start = peek();
    expect("invariant");
    expect("(");
    std::vector<Token> expression_tokens;
    std::size_t depth = 0U;
    while (!(at(")") && depth == 0U)) {
      if (at(TokenKind::End) || at(TokenKind::Newline)) {
        fail(invariant_start, "unterminated recursive invariant");
      }
      Token token = take();
      if (token.text == "(" || token.text == "[" || token.text == "{") ++depth;
      else if (token.text == ")" || token.text == "]" || token.text == "}") --depth;
      expression_tokens.push_back(std::move(token));
    }
    expect(")");
    if (expression_tokens.empty()) fail(invariant_start, "recursive invariant is empty");
    FlatParser parser(std::move(expression_tokens), types_);
    ExprPtr result = parser.expression();
    parser.expect_end();
    return result;
  };

  std::function<StateSchemaNode()> choice_group;
  std::function<StateSchemaNode()> control;
  control = [&]() {
    const Token node_start = peek();
    StateSchemaNode node;
    node.kind = StateSchemaNode::Kind::Control;
    node.name = identifier();
    node.line = node_start.line;
    node.column = node_start.column;
    if (match("(")) {
      if (match(")")) fail(node_start, "nested control state cannot be empty");
      do {
        if (at("invariant")) node.invariants.push_back(inline_invariant());
        else if (peek(1).text == ":") node.children.push_back(field_node());
        else node.children.push_back(choice_group());
      } while (match(","));
      expect(")");
    }
    return node;
  };
  choice_group = [&]() {
    const Token group_start = peek();
    StateSchemaNode group;
    group.kind = StateSchemaNode::Kind::Choice;
    group.name = identifier();
    group.line = group_start.line;
    group.column = group_start.column;
    expect("(");
    group.children.push_back(control());
    while (match("|")) group.children.push_back(control());
    expect(")");
    if (group.children.size() < 2U) {
      fail(group_start, "nested state choice needs at least two alternatives");
    }
    return group;
  };

  const bool compact = match("=");
  if (compact) {
    schema.initial = true;
    if (at(";")) fail(peek(), "compact recursive state cannot be empty");
    do {
      if (at("invariant")) schema.invariants.push_back(inline_invariant());
      else if (peek(1).text == ":") schema.root.children.push_back(field_node());
      else schema.root.children.push_back(choice_group());
    } while (match(","));
    expect(";");
    newline();
  } else {
    expect(":");
    newline();
    indent();
    while (!at(TokenKind::Dedent)) {
      if (match("invariant")) {
        expect(":");
        schema.invariants.push_back(block_expression());
        continue;
      }
      if (peek(1).text == ":") schema.root.children.push_back(field_node());
      else schema.root.children.push_back(choice_group());
      static_cast<void>(match(","));
      newline();
    }
    dedent();
  }

  constexpr std::size_t schema_depth_limit = 64U;
  constexpr std::size_t schema_node_limit = 4096U;
  std::size_t node_count = 1U;
  std::set<std::string, std::less<>> canonical_paths;
  canonical_paths.insert(schema.name);
  std::function<void(StateSchemaNode&, std::string_view, std::size_t)> validate =
      [&](StateSchemaNode& node, std::string_view parent, std::size_t depth) {
        if (depth > schema_depth_limit) {
          throw Error("recursive state schema exceeds depth limit", node.line, node.column);
        }
        if (++node_count > schema_node_limit) {
          throw Error("recursive state schema exceeds node limit", node.line, node.column);
        }
        node.canonical_path = std::string(parent) + "." + node.name;
        if (!canonical_paths.insert(node.canonical_path).second) {
          throw Error("recursive state path is duplicated: '" + node.canonical_path + "'",
                      node.line, node.column);
        }
        std::set<std::string, std::less<>> sibling_names;
        for (StateSchemaNode& child : node.children) {
          if (!sibling_names.insert(child.name).second) {
            throw Error("recursive state repeats child '" + child.name + "'",
                        child.line, child.column);
          }
          validate(child, node.canonical_path, depth + 1U);
        }
        if (node.kind == StateSchemaNode::Kind::Choice) {
          if (node.children.size() < 2U ||
              std::any_of(node.children.begin(), node.children.end(),
                          [](const StateSchemaNode& child) {
                            return child.kind != StateSchemaNode::Kind::Control;
                          })) {
            throw Error("recursive choice must contain control alternatives",
                        node.line, node.column);
          }
        }
        if (node.kind == StateSchemaNode::Kind::Value && !node.children.empty()) {
          throw Error("typed state value cannot contain child states", node.line, node.column);
        }
      };
  std::set<std::string, std::less<>> root_names;
  for (StateSchemaNode& child : schema.root.children) {
    if (!root_names.insert(child.name).second) {
      throw Error("state schema repeats root child '" + child.name + "'",
                  child.line, child.column);
    }
    validate(child, schema.name, 1U);
  }

  State root;
  root.name = schema.name;
  root.context = schema.context;
  root.initial = schema.initial;
  root.invariants = schema.invariants;
  root.captures = schema.captures;
  root.line = schema.line;
  root.column = schema.column;
  for (const StateSchemaNode& child : schema.root.children) {
    if (child.kind == StateSchemaNode::Kind::Value) root.fields.push_back(*child.value);
  }
  result.lowered_states.push_back(std::move(root));

  std::function<void(const StateSchemaNode&, std::vector<std::string>)> lower =
      [&](const StateSchemaNode& node, std::vector<std::string> ancestors) {
    if (node.kind == StateSchemaNode::Kind::Choice) {
      const std::string context = schema.context + "::" + node.canonical_path;
      for (std::size_t index = 0; index < node.children.size(); ++index) {
        const StateSchemaNode& alternative = node.children[index];
        State state;
        state.name = alternative.canonical_path;
        state.context = context;
        state.initial = index == 0U;
        state.ancestors = ancestors;
        state.line = alternative.line;
        state.column = alternative.column;
        state.invariants = alternative.invariants;
        state.captures = schema.captures;
        for (const StateSchemaNode& child : alternative.children) {
          if (child.kind == StateSchemaNode::Kind::Value) state.fields.push_back(*child.value);
        }
        result.lowered_states.push_back(std::move(state));
        auto child_ancestors = ancestors;
        child_ancestors.push_back(alternative.canonical_path);
        for (const StateSchemaNode& child : alternative.children) {
          lower(child, child_ancestors);
        }
      }
      return;
    }
    for (const StateSchemaNode& child : node.children) lower(child, ancestors);
  };
  for (const StateSchemaNode& child : schema.root.children) lower(child, {});
  return result;
}

Transition Parser::transition() {
  const Token start = peek();
  expect("transition");
  Transition result;
  result.line = start.line;
  result.column = start.column;
  result.name = identifier();
  result.event = result.name;
  bool saw_extensions = false;
  const auto apply_extensions = [&](DeclarationExtensions extensions) {
    if (saw_extensions) fail(peek(), "transition repeats declaration extensions");
    saw_extensions = true;
    result.captures = std::move(extensions.captures);
    result.optimized_score = std::move(extensions.optimized_score);
    if (result.optimized_score) {
      if (result.declaration_context.empty()) {
        fail(peek(), "optimized_score requires an explicit @ context");
      }
      result.optimization_scope = result.declaration_context;
    }
  };
  if (match("@")) {
    const std::string binding = identifier();
    if (at("[")) {
      result.declaration_context = binding;
      apply_extensions(declaration_extensions(true));
    } else {
      // v0 compatibility: the pre-parameter @ name is the legacy Event alias.
      result.event = binding;
    }
  }
  expect("(");
  if (!match(")")) {
    do {
      Parameter parameter;
      const Token parameter_start = peek();
      parameter.name = identifier();
      parameter.line = parameter_start.line;
      parameter.column = parameter_start.column;
      expect(":");
      parameter.type = type();
      result.parameters.push_back(std::move(parameter));
    } while (match(","));
    expect(")");
  }
  if (match("@")) {
    if (!result.declaration_context.empty()) {
      fail(peek(), "transition repeats declaration context");
    }
    result.declaration_context = identifier();
  }
  if (at("[")) {
    apply_extensions(declaration_extensions(true));
  }
  expect(":");
  newline();
  indent();
  const auto qualified_identifier = [&]() {
    std::string result = identifier();
    while (match(".")) result += "." + identifier();
    return result;
  };
  const auto binding = [&]() {
    StateBinding result;
    const Token start_token = peek();
    result.line = start_token.line;
    result.column = start_token.column;
    result.state = qualified_identifier();
    if (match("@")) result.context = identifier();
    return result;
  };
  const auto structural_bindings = [&]() {
    const Token root_start = peek();
    const std::string root = qualified_identifier();
    std::vector<StateBinding> result;
    result.push_back(StateBinding{root, {}, root_start.line, root_start.column});
    if (match("(")) {
      std::function<void(std::string_view)> nested = [&](std::string_view parent) {
        const Token item_start = peek();
        std::string relative = identifier();
        while (match(".")) relative += "." + identifier();
        const std::string full = root + "." +
            (parent.empty() ? relative : std::string(parent) + "." + relative);
        result.push_back(StateBinding{full, {}, item_start.line, item_start.column});
        if (match("(")) {
          do nested(parent.empty() ? relative
                                   : std::string(parent) + "." + relative);
          while (match(","));
          expect(")");
        }
      };
      if (!match(")")) {
        do nested(""); while (match(","));
        expect(")");
      }
    }
    if (match("@")) result.front().context = identifier();
    return result;
  };
  const auto assignments = [&]() {
    std::vector<Assignment> result;
    while (!at(TokenKind::Dedent)) {
      Assignment assignment;
      const Token assignment_start = peek();
      assignment.field = identifier();
      assignment.line = assignment_start.line;
      assignment.column = assignment_start.column;
      expect("=");
      assignment.value = line_expression();
      result.push_back(std::move(assignment));
    }
    return result;
  };
  const auto scoped_assignments = [&]() {
    std::vector<std::pair<std::string, Assignment>> result;
    while (!at(TokenKind::Dedent)) {
      Assignment assignment;
      const Token assignment_start = peek();
      assignment.field = identifier();
      assignment.line = assignment_start.line;
      assignment.column = assignment_start.column;
      expect("=");
      std::vector<Token> expression_tokens;
      while (!at(TokenKind::Newline)) {
        if (at(TokenKind::End)) fail(peek(), "expected end of set assignment");
        expression_tokens.push_back(take());
      }
      newline();
      if (expression_tokens.size() < 3U ||
          expression_tokens[expression_tokens.size() - 2U].text != "@" ||
          expression_tokens.back().kind != TokenKind::Identifier) {
        fail(assignment_start,
             "compact set assignment requires 'field = expression @ context'");
      }
      const std::string context = expression_tokens.back().text;
      expression_tokens.resize(expression_tokens.size() - 2U);
      FlatParser parser(std::move(expression_tokens), types_);
      assignment.value = parser.expression();
      parser.expect_end();
      result.emplace_back(context, std::move(assignment));
    }
    return result;
  };
  const auto source_patterns = [&]() {
    std::vector<std::vector<StateBinding>> expansions(1);
    expect("(");
    if (!match(")")) {
      do {
        const Token pattern_start = peek();
        std::vector<std::vector<StateBinding>> choices;
        if (match("{")) {
          do {
            const std::string choice = qualified_identifier();
            choices.push_back({StateBinding{choice, {}, pattern_start.line,
                                            pattern_start.column}});
          } while (match(","));
          expect("}");
        } else if (match("_")) {
          choices.push_back({StateBinding{"_", {}, pattern_start.line,
                                          pattern_start.column}});
        } else {
          choices.push_back(structural_bindings());
        }
        std::string context;
        if (match("@")) context = identifier();
        if (!context.empty()) {
          for (auto& choice : choices) choice.front().context = context;
        }
        std::vector<std::vector<StateBinding>> next;
        for (const auto& expansion : expansions) {
          for (const auto& choice : choices) {
            auto item = expansion;
            item.insert(item.end(), choice.begin(), choice.end());
            next.push_back(std::move(item));
          }
        }
        expansions = std::move(next);
      } while (match(","));
      expect(")");
    }
    return expansions;
  };
  const auto exact_targets = [&]() {
    std::vector<TransitionTarget> targets;
    expect("(");
    if (!match(")")) {
      do {
        if (at("{") || at("_")) {
          fail(peek(), "transition targets must be exact states");
        }
        for (StateBinding& item : structural_bindings()) {
          targets.push_back(TransitionTarget{std::move(item), {}});
        }
      } while (match(","));
      expect(")");
    }
    return targets;
  };
  while (!at(TokenKind::Dedent)) {
    if (match("case")) {
      bool first_route = result.from.empty();
      std::string case_name;
      if (!at("(")) case_name = identifier();
      auto sources = source_patterns();
      expect("->");
      auto targets = exact_targets();
      expect(":");
      newline();
      indent();
      ExprPtr condition = make_literal(Value(true));
      std::shared_ptr<ActionExpr> action;
      TemporalExprPtr obligation;
      while (!at(TokenKind::Dedent)) {
        if (match("where")) {
          expect(":");
          condition = block_expression();
          continue;
        }
        if (match("set")) {
          const auto attach = [&](std::string_view context,
                                  const Assignment& update) {
            const auto found = std::find_if(
                targets.begin(), targets.end(), [&](const TransitionTarget& target) {
                  return target.binding.context == context;
                });
            if (found == targets.end()) {
              fail(peek(), "set @" + std::string(context) +
                               " has no matching path target");
            }
            found->assignments.push_back(update);
          };
          if (match("@")) {
            const std::string context = identifier();
            expect(":");
            newline();
            indent();
            const std::vector<Assignment> updates = assignments();
            dedent();
            for (const Assignment& update : updates) attach(context, update);
          } else {
            expect(":");
            newline();
            indent();
            const auto updates = scoped_assignments();
            dedent();
            for (const auto& [context, update] : updates) attach(context, update);
          }
          continue;
        }
        if (match("do")) {
          expect(":");
          action = block_action();
          continue;
        }
        if (match("ensure")) {
          expect(":");
          obligation = block_temporal_expression();
          continue;
        }
        fail(peek(), "case path requires where, set, do, or ensure");
      }
      dedent();
      for (auto& source : sources) {
        if (first_route) {
          result.from = std::move(source);
          result.to = targets;
          result.case_name = case_name;
          result.condition = condition;
          result.action = action;
          result.obligation = obligation;
          first_route = false;
        } else {
          result.alternatives.push_back(TransitionAlternative{
              case_name, std::move(source), targets, condition, action,
              obligation, {}, {}});
        }
      }
      continue;
    }
    if (match("set")) {
      const auto attach = [&](std::vector<TransitionTarget>& targets,
                              std::string_view context,
                              const Assignment& update) {
        const auto found = std::find_if(targets.begin(), targets.end(),
                                        [&](const TransitionTarget& target) {
                                          return target.binding.context == context;
                                        });
        if (found == targets.end()) {
          fail(peek(), "set @" + std::string(context) +
                           " has no matching target context");
        }
        found->assignments.push_back(update);
      };
      const auto attach_all = [&](std::string_view context, const Assignment& update) {
        attach(result.to, context, update);
        for (TransitionAlternative& alternative : result.alternatives) {
          attach(alternative.to, context, update);
        }
      };
      if (match("@")) {
        const std::string context = identifier();
        expect(":");
        newline();
        indent();
        const std::vector<Assignment> updates = assignments();
        dedent();
        for (const Assignment& update : updates) attach_all(context, update);
      } else {
        expect(":");
        newline();
        indent();
        const auto updates = scoped_assignments();
        dedent();
        for (const auto& [context, update] : updates) attach_all(context, update);
      }
      continue;
    }
    if (match("from")) {
      if (match(":")) {
        newline();
        indent();
        while (!at(TokenKind::Dedent)) {
          result.from.push_back(binding());
          newline();
        }
        dedent();
      } else {
        result.from.push_back(binding());
        newline();
      }
      continue;
    }
    if (match("to")) {
      if (match(":")) {
        newline();
        indent();
        while (!at(TokenKind::Dedent)) {
          TransitionTarget target;
          target.binding = binding();
          if (match(":")) {
            newline();
            indent();
            target.assignments = assignments();
            dedent();
          } else {
            newline();
          }
          result.to.push_back(std::move(target));
        }
        dedent();
      } else {
        TransitionTarget target;
        target.binding = binding();
        expect(":");
        newline();
        indent();
        target.assignments = assignments();
        dedent();
        result.to.push_back(std::move(target));
      }
      continue;
    }
    if (match("where")) {
      expect(":");
      result.condition = block_expression();
      continue;
    }
    if (match("do")) {
      expect(":");
      result.action = block_action();
      continue;
    }
    if (match("ensure")) {
      expect(":");
      TemporalExprPtr obligation = block_temporal_expression();
      result.obligation = obligation;
      for (TransitionAlternative& alternative : result.alternatives) {
        alternative.obligation = obligation;
      }
      continue;
    }
    fail(peek(), "expected case, set, from, to, where, do, or ensure");
  }
  dedent();
  return result;
}

TraceDeclaration Parser::trace(const std::vector<Transition>& transitions) {
  const Token start = peek();
  expect("trace");
  TraceDeclaration result;
  result.line = start.line;
  result.column = start.column;
  result.name = identifier();
  if (match("@")) result.root_context = identifier();
  expect(":");
  newline();
  indent();
  const auto parse_replay = [&]() {
    result.has_replay = true;
    expect(":");
    newline();
    indent();
    const auto transition_for = [&](std::string_view transition_name,
                                    std::string_view case_name) -> const Transition& {
      const auto found = std::find_if(
          transitions.begin(), transitions.end(), [&](const Transition& item) {
            return item.name == transition_name;
          });
      if (found == transitions.end()) {
        fail(peek(), "replay references unknown transition '" +
                         std::string(transition_name) + "'");
      }
      const auto has_named_path = [&](std::string_view path) {
        return found->case_name == path ||
               std::any_of(found->alternatives.begin(), found->alternatives.end(),
                           [&](const TransitionAlternative& alternative) {
                             return alternative.name == path;
                           });
      };
      const bool declares_named_paths = !found->case_name.empty() ||
          std::any_of(found->alternatives.begin(), found->alternatives.end(),
                      [](const TransitionAlternative& alternative) {
                        return !alternative.name.empty();
                      });
      if (case_name.empty() && declares_named_paths) {
        fail(peek(), "replay must name an exact path of transition '" +
                         std::string(transition_name) + "'");
      }
      if (!case_name.empty() && !has_named_path(case_name)) {
        fail(peek(), "replay references unknown transition path '" +
                         std::string(transition_name) + "." + std::string(case_name) + "'");
      }
      return *found;
    };
    while (!at(TokenKind::Dedent)) {
      EventBatch batch;
      std::vector<std::string> paths;
      std::vector<std::string> procedures;
      std::vector<std::string> injected_transitions;
      for (;;) {
        const bool explicit_inject = match("inject");
        const std::string occurrence_name = identifier();
        std::string case_name;
        if (match(".")) case_name = identifier();
        if (explicit_inject && !case_name.empty()) {
          fail(peek(), "inject addresses a transition family, not a case");
        }
        const Transition* transition = nullptr;
        std::string expected_path;
        if (!explicit_inject) {
          transition = &transition_for(occurrence_name, case_name);
          expected_path = occurrence_name +
                          (case_name.empty() ? "" : "." + case_name);
        } else {
          const auto found = std::find_if(
              transitions.begin(), transitions.end(), [&](const Transition& item) {
                return item.name == occurrence_name;
              });
          if (found == transitions.end()) {
            fail(peek(), "inject references unknown transition '" + occurrence_name + "'");
          }
          transition = &*found;
        }
        Event event;
        event.name = transition->event;
        expect("(");
        for (std::size_t index = 0; index < transition->parameters.size(); ++index) {
          if (index != 0) expect(",");
          event.fields.emplace(transition->parameters[index].name,
                               initial_value(transition->parameters[index].type));
        }
        expect(")");
        expect("@");
        const std::string procedure_name = identifier();
        if (explicit_inject && match("->")) {
          const std::string expected_transition = identifier();
          std::string expected_case;
          if (match(".")) expected_case = identifier();
          static_cast<void>(transition_for(expected_transition, expected_case));
          if (expected_transition != occurrence_name) {
            fail(peek(), "search expectation must belong to injected transition '" +
                             occurrence_name + "'");
          }
          expected_path = expected_transition +
                          (expected_case.empty() ? "" : "." + expected_case);
        }
        batch.events.push_back(std::move(event));
        paths.push_back(std::move(expected_path));
        procedures.push_back(procedure_name);
        injected_transitions.push_back(occurrence_name);
        if (!match("|")) break;
      }
      result.events.rounds.push_back(std::move(batch));
      result.replay_paths.push_back(std::move(paths));
      result.replay_procedures.push_back(std::move(procedures));
      result.replay_transitions.push_back(std::move(injected_transitions));
      static_cast<void>(match(","));
      newline();
    }
    dedent();
  };
  const auto parse_capture = [&]() {
    result.has_capture = true;
    result.capture_mode = TraceCaptureMode::Closed;
    if (match("closed")) result.capture_mode = TraceCaptureMode::Closed;
    else if (match("projected")) result.capture_mode = TraceCaptureMode::Projected;
    expect(":");
    newline();
    indent();
    while (!at(TokenKind::Dedent)) {
      const Token binding_start = peek();
      if (match("state")) {
        expect("(");
        if (!match(")")) {
          do {
            StateBinding binding;
            binding.line = binding_start.line;
            binding.column = binding_start.column;
            binding.state = identifier();
            expect("@");
            binding.context = identifier();
            const bool duplicate = std::any_of(
                result.capture.begin(), result.capture.end(),
                [&](const StateBinding& item) {
                  return item.state == binding.state && item.context == binding.context;
                });
            if (duplicate) fail(binding_start, "capture filter repeats a state");
            result.capture.push_back(std::move(binding));
          } while (match(","));
          expect(")");
        }
        newline();
        continue;
      }
      if (match("transition")) {
        expect("(");
        if (!match(")")) {
          do {
            const std::string transition_name = identifier();
            std::string path = transition_name;
            if (match(".")) path += "." + identifier();
            if (!result.paths.insert(path).second) {
              fail(binding_start, "capture filter repeats a transition path");
            }
          } while (match(","));
          expect(")");
        }
        newline();
        continue;
      }
      if (match("procedure")) {
        expect("(");
        if (!match(")")) {
          do {
            const std::string procedure_name = identifier();
            if (!result.captured_procedures.insert(procedure_name).second) {
              fail(binding_start, "capture filter repeats a procedure");
            }
          } while (match(","));
          expect(")");
        }
        newline();
        continue;
      }
      // v0 compatibility: bare State @ context and Transition.case selectors.
      const std::string state_name = identifier();
      if (match(".")) {
        const std::string path = state_name + "." + identifier();
        if (!result.paths.insert(path).second) {
          fail(binding_start, "trace captures the same transition path twice");
        }
        newline();
        continue;
      }
      std::string context;
      if (match("@")) context = identifier();
      if (context.empty()) {
        fail(peek(), "dynamic trace capture requires an explicit @ context");
      }
      const bool duplicate = std::any_of(
          result.capture.begin(), result.capture.end(),
          [&](const StateBinding& item) {
            return item.state == state_name && item.context == context;
          });
      if (duplicate) fail(peek(), "trace captures the same state twice");
      result.capture.push_back(
          StateBinding{state_name, context, binding_start.line, binding_start.column});
      newline();
    }
    dedent();
  };
  if (match("replay")) parse_replay();
  if (match("capture")) parse_capture();
  if (!result.has_replay && !result.has_capture) {
    fail(peek(), "trace requires replay: and/or capture [closed|projected]:");
  }
  if (!at(TokenKind::Dedent)) {
    fail(peek(), "trace sections must be ordered replay then capture");
  }
  dedent();
  return result;
}

Transition Parser::anonymous_transition(std::string_view procedure,
                                        std::size_t ordinal) {
  const Token start = peek();
  Transition result;
  result.line = start.line;
  result.column = start.column;
  result.name = std::string(procedure) + "_lambda_" + std::to_string(ordinal);
  result.procedure_scope = std::string(procedure);
  result.event = result.name;
  const auto binding = [&]() {
    StateBinding item;
    const Token token = peek();
    item.line = token.line;
    item.column = token.column;
    item.state = identifier();
    if (match("@")) item.context = identifier();
    return item;
  };
  expect("(");
  if (!match(")")) {
    do result.from.push_back(binding()); while (match(","));
    expect(")");
  }
  expect("->");
  expect("(");
  if (!match(")")) {
    do result.to.push_back(TransitionTarget{binding(), {}}); while (match(","));
    expect(")");
  }
  expect(":");
  newline();
  indent();
  while (!at(TokenKind::Dedent)) {
    if (match("where")) {
      expect(":");
      result.condition = block_expression();
      continue;
    }
    if (match("set")) {
      expect("@");
      const Token context_token = peek();
      const std::string context = identifier();
      const auto target = std::find_if(
          result.to.begin(), result.to.end(), [&](const TransitionTarget& item) {
            return item.binding.context == context;
          });
      if (target == result.to.end()) {
        fail(context_token, "anonymous transition set has no target @" + context);
      }
      expect(":");
      newline();
      indent();
      while (!at(TokenKind::Dedent)) {
        Assignment assignment;
        const Token assignment_start = peek();
        assignment.line = assignment_start.line;
        assignment.column = assignment_start.column;
        assignment.field = identifier();
        expect("=");
        assignment.value = line_expression();
        target->assignments.push_back(std::move(assignment));
      }
      dedent();
      continue;
    }
    if (match("do")) {
      expect(":");
      result.action = block_action();
      continue;
    }
    if (match("ensure")) {
      expect(":");
      result.obligation = block_temporal_expression();
      continue;
    }
    fail(peek(), "anonymous transition requires where, set, do, or ensure");
  }
  dedent();
  if (result.from.empty() || result.to.empty()) {
    fail(start, "anonymous transition requires non-empty source and target sets");
  }
  return result;
}

ProcedureDeclaration Parser::procedure() {
  const Token start = peek();
  expect("procedure");
  ProcedureDeclaration result;
  result.line = start.line;
  result.column = start.column;
  result.name = identifier();
  expect("@");
  result.initial_context = identifier();
  if (at("[")) {
    result.captures = declaration_extensions(false).captures;
  }
  expect(":");
  newline();
  indent();
  expect("initial");
  expect("(");
  if (!match(")")) {
    do {
      StateBinding binding;
      const Token binding_start = peek();
      binding.line = binding_start.line;
      binding.column = binding_start.column;
      binding.state = identifier();
      if (match("@")) binding.context = identifier();
      result.initial_states.push_back(std::move(binding));
    } while (match(","));
    expect(")");
  }
  newline();
  std::size_t ordinal = 1U;
  while (!at(TokenKind::Dedent)) {
    if (!at("(")) {
      fail(peek(), "procedure body accepts only initial and anonymous (from)->(to) transitions");
    }
    result.anonymous_transitions.push_back(
        anonymous_transition(result.name, ordinal++));
  }
  dedent();
  if (result.initial_states.empty()) {
    fail(start, "procedure requires at least one initial state");
  }
  return result;
}

ClaimDeclaration Parser::claim() {
  const Token start = peek();
  expect("Claim");
  ClaimDeclaration result;
  result.line = start.line;
  result.column = start.column;
  result.name = identifier();
  expect("@");
  if (match("trace")) {
    result.target_kind = ClaimDeclaration::TargetKind::Trace;
    result.target = identifier();
  } else if (match("state")) {
    result.target_kind = ClaimDeclaration::TargetKind::State;
    result.target = identifier();
  } else if (match("procedure")) {
    result.target_kind = ClaimDeclaration::TargetKind::Procedure;
    result.target = identifier();
  } else {
    // v0.3 compatibility: an untyped target after @ denotes a trace.
    result.target_kind = ClaimDeclaration::TargetKind::Trace;
    result.target = identifier();
  }
  if (match("@")) {
    expect("(");
    if (!match(")")) {
      do result.contexts.push_back(identifier()); while (match(","));
      expect(")");
    }
  }
  expect(":");
  newline();
  indent();
  if (at("always") && peek(1).text == ":") {
    take();
    expect(":");
    result.property = make_temporal_unary(TemporalExpr::Kind::Always,
                                          make_temporal_atom(block_expression()));
  } else if (at("eventually") && peek(1).text == ":") {
    take();
    expect(":");
    result.property = make_temporal_unary(TemporalExpr::Kind::Eventually,
                                          make_temporal_atom(block_expression()));
  } else if (at("until") && peek(1).text == ":") {
    take();
    expect(":");
    newline();
    indent();
    expect("hold");
    expect(":");
    ExprPtr hold = block_expression();
    expect("release");
    expect(":");
    ExprPtr release = block_expression();
    dedent();
    result.property = make_temporal_binary(
        TemporalExpr::Kind::Until, make_temporal_atom(std::move(hold)),
        make_temporal_atom(std::move(release)));
  } else if (at("within") && peek(1).kind == TokenKind::Integer) {
    take();
    if (!at(TokenKind::Integer)) fail(peek(), "within requires a non-negative round bound");
    const Token bound = take();
    std::uint64_t value = 0;
    const auto parsed = std::from_chars(bound.text.data(),
                                        bound.text.data() + bound.text.size(), value);
    if (parsed.ec != std::errc{} || parsed.ptr != bound.text.data() + bound.text.size()) {
      fail(bound, "invalid within bound");
    }
    expect(":");
    auto property = make_temporal_unary(TemporalExpr::Kind::Within,
                                        make_temporal_atom(block_expression()));
    property->bound = value;
    result.property = std::move(property);
  } else if (at("since") && peek(1).text == ":") {
    take();
    expect(":");
    newline();
    indent();
    expect("hold");
    expect(":");
    ExprPtr hold = block_expression();
    expect("origin");
    expect(":");
    ExprPtr origin = block_expression();
    dedent();
    result.property = make_temporal_binary(
        TemporalExpr::Kind::Since, make_temporal_atom(std::move(hold)),
        make_temporal_atom(std::move(origin)));
  } else if (match("count")) {
    result.count_at_most = true;
    result.transition = identifier();
    if (match(".")) result.transition += "." + identifier();
    expect("<=");
    if (!at(TokenKind::Integer)) fail(peek(), "claim count limit must be an integer");
    const Token limit = take();
    const auto parsed = std::from_chars(limit.text.data(), limit.text.data() + limit.text.size(),
                                        result.limit);
    if (parsed.ec != std::errc{} || parsed.ptr != limit.text.data() + limit.text.size()) {
      fail(limit, "invalid claim count limit");
    }
    newline();
  } else {
    result.property = line_temporal_expression();
  }
  if (!at(TokenKind::Dedent)) fail(peek(), "Claim has more than one property");
  dedent();
  return result;
}

CompactStateDeclaration Parser::compact_state() {
  const Token start = peek();
  expect("state");
  CompactStateDeclaration result;
  result.line = start.line;
  result.column = start.column;
  do {
    if (at(TokenKind::Newline) || at(TokenKind::End)) {
      fail(peek(), "compact state declaration must end with ';' on the same line");
    }
    result.names.push_back(identifier());
  } while (match(","));
  expect(";");
  newline();
  return result;
}

CompactTransitionDeclaration Parser::compact_transition() {
  const Token start = peek();
  expect("trans");
  CompactTransitionDeclaration result;
  result.line = start.line;
  result.column = start.column;
  const std::string first = identifier();
  if (match(":")) {
    result.family = first;
    result.from = identifier();
  } else {
    result.from = first;
  }
  expect("->");
  result.to = identifier();
  if (match("when")) {
    std::vector<Token> expression_tokens;
    std::size_t depth = 0;
    while (!at(";")) {
      if (at(TokenKind::Newline) || at(TokenKind::End)) {
        fail(peek(), "compact trans declaration must end with ';' on the same line");
      }
      Token token = take();
      if (token.text == "(" || token.text == "[" || token.text == "{") {
        ++depth;
      } else if (token.text == ")" || token.text == "]" || token.text == "}") {
        if (depth != 0) --depth;
      } else if (token.text == "," && depth == 0) {
        token.kind = TokenKind::Identifier;
        token.text = "and";
      }
      expression_tokens.push_back(std::move(token));
    }
    if (expression_tokens.empty()) fail(start, "compact trans when clause is empty");
    FlatParser parser(std::move(expression_tokens), types_);
    result.condition = parser.expression();
    parser.expect_end();
  }
  expect(";");
  newline();
  return result;
}

CompactProcedureDeclaration Parser::compact_procedure() {
  const Token start = peek();
  expect("procedure");
  CompactProcedureDeclaration result;
  result.line = start.line;
  result.column = start.column;
  result.name = identifier();

  const auto compact_literal = [&]() -> std::pair<DataType, Value> {
    if (match("true")) return {bool_type(), Value(true)};
    if (match("false")) return {bool_type(), Value(false)};
    if (at(TokenKind::String)) return {string_type(), Value(take().text)};
    const bool negative = match("-");
    if (!at(TokenKind::Integer)) {
      fail(peek(), "compact procedure field requires a bool, integer, rational, or string literal");
    }
    const Token numerator_token = take();
    ExactInt numerator;
    try {
      numerator = ExactInt::parse(numerator_token.text);
    } catch (const Error& error) {
      fail(numerator_token, error.what());
    }
    if (negative) numerator = -numerator;
    if (!match("/")) return {int_type(), Value(std::move(numerator))};
    const bool denominator_negative = match("-");
    if (!at(TokenKind::Integer)) fail(peek(), "expected rational denominator");
    const Token denominator_token = take();
    ExactInt denominator;
    try {
      denominator = ExactInt::parse(denominator_token.text);
    } catch (const Error& error) {
      fail(denominator_token, error.what());
    }
    if (denominator_negative) denominator = -denominator;
    try {
      return {rational_type(),
              Value(Rational(std::move(numerator), std::move(denominator)))};
    } catch (const Error& error) {
      fail(denominator_token, error.what());
    }
  };

  bool need_item = true;
  while (!at("&") && !at(";")) {
    if (at(TokenKind::Newline) || at(TokenKind::End)) {
      fail(peek(), "compact procedure declaration must end with ';' on the same line");
    }
    if (!need_item) expect(",");
    const Token item_start = peek();
    const std::string state_name = identifier();
    if (match(".")) {
      const std::string field_name = identifier();
      expect("=");
      auto [type, value] = compact_literal();
      result.fields.push_back(CompactFieldInitialization{
          state_name, field_name, std::move(type), std::move(value),
          item_start.line, item_start.column});
    } else {
      result.active_states.push_back(state_name);
    }
    need_item = false;
  }
  if (result.active_states.empty()) {
    fail(start, "compact procedure requires at least one initial state");
  }
  if (match("&")) {
    expect("inject");
    result.injection = identifier();
  }
  expect(";");
  newline();
  return result;
}

CompactTraceDeclaration Parser::compact_trace() {
  const Token start = peek();
  expect("trace");
  expect("@");
  CompactTraceDeclaration result;
  result.line = start.line;
  result.column = start.column;
  result.procedure = identifier();
  expect(";");
  newline();
  return result;
}

void Parser::lower_compact(
    Program::Impl& program,
    const std::vector<CompactStateDeclaration>& states,
    const std::vector<CompactTransitionDeclaration>& transitions,
    const std::vector<CompactProcedureDeclaration>& procedures,
    const std::vector<CompactTraceDeclaration>& traces) {
  if (states.empty() && transitions.empty() && procedures.empty() && traces.empty()) return;

  std::map<std::string, std::string, std::less<>> parent;
  std::map<std::string, std::pair<std::size_t, std::size_t>, std::less<>> locations;
  for (const CompactStateDeclaration& declaration : states) {
    for (const std::string& name : declaration.names) {
      if (!parent.emplace(name, name).second) {
        throw Error("duplicate compact state '" + name + "'", declaration.line,
                    declaration.column);
      }
      locations.emplace(name, std::pair{declaration.line, declaration.column});
    }
  }
  if (parent.empty()) {
    const auto& declaration = !transitions.empty()
                                  ? std::pair{transitions.front().line,
                                              transitions.front().column}
                                  : !procedures.empty()
                                        ? std::pair{procedures.front().line,
                                                    procedures.front().column}
                                        : std::pair{traces.front().line,
                                                    traces.front().column};
    throw Error("compact model must declare states before use", declaration.first,
                declaration.second);
  }

  const auto find_root = [&](std::string name) {
    while (parent.at(name) != name) name = parent.at(name);
    return name;
  };
  const auto require_state = [&](std::string_view name, std::size_t line,
                                 std::size_t column) {
    if (!parent.contains(name)) {
      throw Error("compact model references unknown state '" + std::string(name) + "'",
                  line, column);
    }
  };
  for (const CompactTransitionDeclaration& transition : transitions) {
    require_state(transition.from, transition.line, transition.column);
    require_state(transition.to, transition.line, transition.column);
    const std::string left = find_root(transition.from);
    const std::string right = find_root(transition.to);
    if (left != right) {
      const std::string keep = std::min(left, right);
      const std::string merge = std::max(left, right);
      parent[merge] = keep;
    }
  }
  for (auto& [name, root] : parent) root = find_root(name);

  std::map<std::string, std::string, std::less<>> contexts;
  for (const auto& [name, root] : parent) {
    static_cast<void>(name);
    contexts.try_emplace(root, "compact_" + root);
  }
  const auto context_for = [&](std::string_view state) -> const std::string& {
    return contexts.at(parent.at(std::string(state)));
  };

  struct CompactField {
    DataType type;
    Value value;
    std::size_t line;
    std::size_t column;
  };
  std::map<std::pair<std::string, std::string>, CompactField> fields;
  for (const CompactProcedureDeclaration& procedure : procedures) {
    for (const std::string& state : procedure.active_states) {
      require_state(state, procedure.line, procedure.column);
    }
    for (const CompactFieldInitialization& field : procedure.fields) {
      require_state(field.state, field.line, field.column);
      const auto key = std::pair{parent.at(field.state), field.field};
      const auto [found, inserted] = fields.emplace(
          key, CompactField{field.type, field.value, field.line, field.column});
      if (!inserted &&
          (found->second.type != field.type || !(found->second.value == field.value))) {
        throw Error("compact procedures disagree on default field '" + field.field +
                        "' for state axis @" + context_for(field.state),
                    field.line, field.column);
      }
    }
  }

  std::map<std::string, std::string, std::less<>> first_state;
  for (const auto& [name, root] : parent) first_state.try_emplace(root, name);
  for (const auto& [name, root] : parent) {
    const auto [line, column] = locations.at(name);
    State state;
    state.name = name;
    state.context = context_for(name);
    state.initial = first_state.at(root) == name;
    state.line = line;
    state.column = column;
    for (const auto& [key, field] : fields) {
      if (key.first != root) continue;
      state.fields.push_back(Field{key.second, field.type, field.value, Field::Merge::Reject,
                                   field.line, field.column});
    }
    program.states.push_back(std::move(state));
  }

  const std::function<void(const ExprPtr&)> rewrite_names = [&](const ExprPtr& expression) {
    if (!expression) return;
    if (expression->kind == Expr::Kind::Name) {
      const std::size_t dot = expression->text.find('.');
      if (dot != std::string::npos) {
        const std::string state = expression->text.substr(0, dot);
        if (parent.contains(state)) {
          expression->text = contexts.size() == 1U
                                 ? expression->text.substr(dot + 1U)
                                 : context_for(state) + expression->text.substr(dot);
        }
      }
    }
    rewrite_names(expression->left);
    rewrite_names(expression->right);
    rewrite_names(expression->third);
    for (const ExprPtr& child : expression->children) rewrite_names(child);
    for (const MatchArm& arm : expression->arms) rewrite_names(arm.body);
  };
  for (const CompactTransitionDeclaration& transition : transitions) {
    rewrite_names(transition.condition);
  }

  std::set<std::string, std::less<>> explicit_families;
  for (const CompactTransitionDeclaration& transition : transitions) {
    if (!transition.family.empty()) explicit_families.insert(transition.family);
  }
  std::vector<std::pair<std::string, std::vector<const CompactTransitionDeclaration*>>>
      transition_families;
  std::map<std::string, std::size_t, std::less<>> family_indexes;
  std::map<std::string, std::size_t, std::less<>> generated_ordinals;
  for (const CompactTransitionDeclaration& transition : transitions) {
    std::string family_name = transition.family;
    if (family_name.empty()) {
      const std::string base = transition.from + "_to_" + transition.to;
      family_name = base;
      std::size_t& ordinal = generated_ordinals[base];
      while (explicit_families.contains(family_name) || family_indexes.contains(family_name)) {
        family_name = base + "_" + std::to_string(++ordinal + 1U);
      }
    }
    auto found = family_indexes.find(family_name);
    if (found == family_indexes.end()) {
      const std::size_t index = transition_families.size();
      family_indexes.emplace(family_name, index);
      transition_families.push_back({family_name, {}});
      found = family_indexes.find(family_name);
    }
    transition_families[found->second].second.push_back(&transition);
  }
  for (const CompactProcedureDeclaration& procedure : procedures) {
    if (!procedure.injection.empty() && !family_indexes.contains(procedure.injection)) {
      throw Error("compact procedure inject references undefined transition '" +
                      procedure.injection + "'",
                  procedure.line, procedure.column);
    }
  }

  for (const auto& [family_name, routes] : transition_families) {
    Transition family;
    family.name = family_name;
    family.event = family_name;
    family.line = routes.front()->line;
    family.column = routes.front()->column;
    std::map<std::string, std::size_t, std::less<>> route_names;
    for (std::size_t index = 0; index < routes.size(); ++index) {
      const CompactTransitionDeclaration& compact = *routes[index];
      std::string route_name = compact.from + "_to_" + compact.to;
      const std::size_t ordinal = ++route_names[route_name];
      if (ordinal != 1U) route_name += "_" + std::to_string(ordinal);
      TransitionAlternative route;
      route.name = std::move(route_name);
      route.from.push_back(StateBinding{compact.from, context_for(compact.from),
                                        compact.line, compact.column});
      route.to.push_back(TransitionTarget{
          StateBinding{compact.to, context_for(compact.to), compact.line, compact.column}, {}});
      route.condition = compact.condition;
      if (index == 0) {
        family.case_name = std::move(route.name);
        family.from = std::move(route.from);
        family.to = std::move(route.to);
        family.condition = std::move(route.condition);
      } else {
        family.alternatives.push_back(std::move(route));
      }
    }
    program.transitions.push_back(std::move(family));
  }

  for (const CompactProcedureDeclaration& compact : procedures) {
    ProcedureDeclaration procedure;
    procedure.name = compact.name;
    procedure.initial_context = "compact";
    procedure.line = compact.line;
    procedure.column = compact.column;
    std::set<std::string, std::less<>> selected_contexts;
    for (const std::string& state : compact.active_states) {
      const std::string& context = context_for(state);
      if (!selected_contexts.insert(context).second) continue;
      procedure.initial_states.push_back(
          StateBinding{state, context, compact.line, compact.column});
    }
    if (!program.procedures.emplace(procedure.name, std::move(procedure)).second) {
      throw Error("duplicate procedure declaration '" + compact.name + "'",
                  compact.line, compact.column);
    }
  }

  for (const CompactTraceDeclaration& compact : traces) {
    std::string procedure_name = compact.procedure;
    if (procedure_name == "procedure") {
      if (procedures.size() != 1U) {
        throw Error("trace @procedure requires exactly one compact procedure",
                    compact.line, compact.column);
      }
      procedure_name = procedures.front().name;
    }
    const auto procedure = std::find_if(
        procedures.begin(), procedures.end(), [&](const CompactProcedureDeclaration& item) {
          return item.name == procedure_name;
        });
    if (procedure == procedures.end()) {
      throw Error("compact trace references unknown compact procedure '" + procedure_name +
                      "'",
                  compact.line, compact.column);
    }
    TraceDeclaration trace;
    trace.name = procedure_name;
    trace.root_context = "compact";
    trace.line = compact.line;
    trace.column = compact.column;
    trace.has_capture = true;
    trace.capture_mode = TraceCaptureMode::Closed;
    trace.captured_procedures.insert(procedure_name);
    if (!procedure->injection.empty()) {
      trace.has_replay = true;
      EventBatch batch;
      batch.events.push_back(Event{procedure->injection, {}});
      trace.events.rounds.push_back(std::move(batch));
      trace.replay_paths.push_back({""});
      trace.replay_procedures.push_back({procedure_name});
      trace.replay_transitions.push_back({procedure->injection});
    }
    program.traces.push_back(std::move(trace));
  }
}

std::string capture_instance_name(const CaptureBinding& binding) {
  if (binding.session == "default") return binding.trace;
  return binding.trace + "/" + binding.session;
}

void aggregate_distributed_captures(Program::Impl& program) {
  struct Aggregate {
    std::set<std::pair<std::string, std::string>> states;
    std::set<std::string, std::less<>> transitions;
    std::set<std::string, std::less<>> procedures;
  };
  std::map<CaptureBinding, Aggregate> aggregates;
  for (const State& state : program.states) {
    for (const CaptureBinding& binding : state.captures) {
      aggregates[binding].states.emplace(state.context, state.name);
    }
  }
  for (const Transition& transition : program.transitions) {
    for (const CaptureBinding& binding : transition.captures) {
      aggregates[binding].transitions.insert(transition.name);
    }
  }
  for (const auto& [name, procedure] : program.procedures) {
    for (const CaptureBinding& binding : procedure.captures) {
      aggregates[binding].procedures.insert(name);
    }
  }

  for (const auto& [binding, aggregate] : aggregates) {
    const std::string instance_name = capture_instance_name(binding);
    auto instance = std::find_if(
        program.traces.begin(), program.traces.end(),
        [&](const TraceDeclaration& trace) { return trace.name == instance_name; });
    if (instance == program.traces.end()) {
      TraceDeclaration generated;
      const auto templ = std::find_if(
          program.traces.begin(), program.traces.end(),
          [&](const TraceDeclaration& trace) { return trace.name == binding.trace; });
      if (templ != program.traces.end()) generated = *templ;
      generated.name = instance_name;
      if (!generated.has_capture) {
        generated.has_capture = true;
        generated.capture_mode = TraceCaptureMode::Projected;
      }
      program.traces.push_back(std::move(generated));
      instance = std::prev(program.traces.end());
    } else if (!instance->has_capture) {
      instance->has_capture = true;
      instance->capture_mode = TraceCaptureMode::Projected;
    }
    for (const auto& [context, state] : aggregate.states) {
      const bool duplicate = std::any_of(
          instance->capture.begin(), instance->capture.end(),
          [&](const StateBinding& item) {
            return item.context == context && item.state == state;
          });
      if (!duplicate) instance->capture.push_back(StateBinding{state, context});
    }
    instance->paths.insert(aggregate.transitions.begin(),
                           aggregate.transitions.end());
    instance->captured_procedures.insert(aggregate.procedures.begin(),
                                         aggregate.procedures.end());
  }
}

std::shared_ptr<Program::Impl> Parser::program() {
  auto result = std::make_shared<Program::Impl>();
  std::vector<CompactStateDeclaration> compact_states;
  std::vector<CompactTransitionDeclaration> compact_transitions;
  std::vector<CompactProcedureDeclaration> compact_procedures;
  std::vector<CompactTraceDeclaration> compact_traces;
  while (!at(TokenKind::End)) {
    if (at(TokenKind::Newline)) {
      take();
    } else if (at("record") || at("variant") || at("enum") || at("newtype") ||
               at("name")) {
      TypeDefinition definition = type_definition();
      if (types_.contains(definition.name)) {
        fail(peek(), "duplicate type '" + definition.name + "'");
      }
      types_.emplace(definition.name, std::move(definition));
    } else if (at("port")) {
      result->action_ports.push_back(action_port_declaration());
    } else if (at("function")) {
      FunctionDeclaration function = function_declaration();
      if (!result->functions.emplace(function.name, std::move(function)).second) {
        fail(peek(), "duplicate function declaration");
      }
    } else if (at("state")) {
      if (peek(2).text == "," || peek(2).text == ";") {
        compact_states.push_back(compact_state());
      } else {
        ParsedStateDeclaration declaration = state();
        result->state_schemas.push_back(std::move(declaration.schema));
        result->states.insert(
            result->states.end(),
            std::make_move_iterator(declaration.lowered_states.begin()),
            std::make_move_iterator(declaration.lowered_states.end()));
      }
    } else if (at("trans")) {
      compact_transitions.push_back(compact_transition());
    } else if (at("transition")) {
      result->transitions.push_back(transition());
    } else if (at("procedure")) {
      if (peek(2).text != "@") {
        compact_procedures.push_back(compact_procedure());
      } else {
        ProcedureDeclaration declaration = procedure();
        result->transitions.insert(
            result->transitions.end(),
            std::make_move_iterator(declaration.anonymous_transitions.begin()),
            std::make_move_iterator(declaration.anonymous_transitions.end()));
        declaration.anonymous_transitions.clear();
        if (!result->procedures.emplace(declaration.name, std::move(declaration)).second) {
          fail(peek(), "duplicate procedure declaration");
        }
      }
    } else if (at("trace")) {
      if (peek(1).text == "@") {
        compact_traces.push_back(compact_trace());
      } else {
        result->traces.push_back(trace(result->transitions));
      }
    } else if (at("Claim")) {
      result->claims.push_back(claim());
    } else {
      fail(peek(), "expected type, name, port, function, state, trans, transition, procedure, trace, or Claim declaration");
    }
  }
  lower_compact(*result, compact_states, compact_transitions, compact_procedures,
                compact_traces);
  aggregate_distributed_captures(*result);
  result->types = types_;
  return result;
}

const State& find_state(const Program::Impl& program, std::string_view name) {
  if (!program.state_index.empty()) {
    const auto indexed = program.state_index.find(name);
    if (indexed == program.state_index.end()) {
      throw Error("unknown state '" + std::string(name) + "'");
    }
    return program.states.at(program.state_positions_by_id.at(indexed->second));
  }
  const auto found = std::find_if(program.states.begin(), program.states.end(),
                                  [&](const State& state) { return state.name == name; });
  if (found == program.states.end()) {
    throw Error("unknown state '" + std::string(name) + "'");
  }
  return *found;
}

const State& find_state(const Program::Impl& program, StateId state_id) {
  if (state_id >= program.state_positions_by_id.size()) {
    throw Error("invalid encoded StateId");
  }
  return program.states.at(program.state_positions_by_id[state_id]);
}

DataType value_type(const Value& value) {
  switch (value.kind()) {
    case Value::Kind::Bool: return bool_type();
    case Value::Kind::Int: return int_type();
    case Value::Kind::Rational: return rational_type();
    case Value::Kind::String: return string_type();
    case Value::Kind::StringSet:
      return DataType(DataType::Kind::Set, string_type());
    case Value::Kind::List:
      if (value.as_list().values.empty()) throw Error("cannot infer empty list type");
      return DataType(DataType::Kind::List, value_type(value.as_list().values.front()));
    case Value::Kind::Set:
      if (value.as_set().values.empty()) throw Error("cannot infer empty set type");
      return DataType(DataType::Kind::Set, value_type(value.as_set().values.front()));
    case Value::Kind::Map:
      if (value.as_map().entries.empty()) throw Error("cannot infer empty map type");
      return DataType(DataType::Kind::Map, value_type(value.as_map().entries.front().first),
                      value_type(value.as_map().entries.front().second));
    case Value::Kind::Bag:
      if (value.as_bag().entries.empty()) throw Error("cannot infer empty bag type");
      return DataType(DataType::Kind::Bag, value_type(value.as_bag().entries.front().first));
    case Value::Kind::Record:
      return DataType(DataType::Kind::Named, value.as_record().type_id);
    case Value::Kind::Variant:
      if (value.as_variant().type_id.starts_with("option<") ||
          value.as_variant().type_id.starts_with("result<")) {
        throw Error("structural variant type needs its declared context");
      }
      return DataType(DataType::Kind::Named, value.as_variant().type_id);
    case Value::Kind::Newtype:
      return DataType(DataType::Kind::Named, value.as_newtype().type_id);
    case Value::Kind::Name:
      return DataType(DataType::Kind::Named, value.as_name().type_id);
    case Value::Kind::Tuple: {
      std::vector<DataType> elements;
      elements.reserve(value.as_tuple().fields.size());
      for (const Value& field : value.as_tuple().fields) elements.push_back(value_type(field));
      return DataType(DataType::Kind::Tuple, std::move(elements));
    }
    case Value::Kind::Relation: {
      if (value.as_relation().rows.empty()) throw Error("cannot infer empty relation type");
      std::vector<DataType> elements;
      for (const Value& field : value.as_relation().rows.front().fields) {
        elements.push_back(value_type(field));
      }
      return DataType(DataType::Kind::Relation, std::move(elements));
    }
  }
  throw Error("invalid value kind");
}

bool value_matches_type(const Value& value, const DataType& type,
                        const TypeRegistry& types) {
  switch (type.kind) {
    case DataType::Kind::Bool: return value.kind() == Value::Kind::Bool;
    case DataType::Kind::Int: return value.kind() == Value::Kind::Int;
    case DataType::Kind::Rational: return value.kind() == Value::Kind::Rational;
    case DataType::Kind::String: return value.kind() == Value::Kind::String;
    case DataType::Kind::List:
      if (value.kind() != Value::Kind::List) return false;
      return std::all_of(value.as_list().values.begin(), value.as_list().values.end(),
                         [&](const Value& item) { return value_matches_type(item, *type.first, types); });
    case DataType::Kind::Set:
      if (value.kind() == Value::Kind::StringSet) {
        return type.first->kind == DataType::Kind::String;
      }
      if (value.kind() != Value::Kind::Set) return false;
      return std::all_of(value.as_set().values.begin(), value.as_set().values.end(),
                         [&](const Value& item) { return value_matches_type(item, *type.first, types); });
    case DataType::Kind::Map:
      if (value.kind() != Value::Kind::Map) return false;
      return std::all_of(value.as_map().entries.begin(), value.as_map().entries.end(),
                         [&](const auto& entry) {
                           return value_matches_type(entry.first, *type.first, types) &&
                                  value_matches_type(entry.second, *type.second, types);
                         });
    case DataType::Kind::Bag:
      if (value.kind() != Value::Kind::Bag) return false;
      return std::all_of(value.as_bag().entries.begin(), value.as_bag().entries.end(),
                         [&](const auto& entry) {
                           return value_matches_type(entry.first, *type.first, types);
                         });
    case DataType::Kind::Named: {
      const auto definition = types.find(type.name);
      if (definition == types.end()) return false;
      if (definition->second.kind == TypeDefinition::Kind::Record) {
        if (value.kind() != Value::Kind::Record || value.as_record().type_id != type.name ||
            value.as_record().fields.size() != definition->second.fields.size()) return false;
        for (const TypeField& field : definition->second.fields) {
          const auto found = std::lower_bound(
              value.as_record().fields.begin(), value.as_record().fields.end(), field.name,
              [](const auto& item, const std::string& name) { return item.first < name; });
          if (found == value.as_record().fields.end() || found->first != field.name ||
              !value_matches_type(found->second, field.type, types)) return false;
        }
        return true;
      }
      if (definition->second.kind == TypeDefinition::Kind::Newtype) {
        return value.kind() == Value::Kind::Newtype &&
               value.as_newtype().type_id == type.name &&
               value_matches_type(value.as_newtype().payload.front(),
                                  *definition->second.underlying, types);
      }
      if (definition->second.kind == TypeDefinition::Kind::Name) {
        return value.kind() == Value::Kind::Name &&
               value.as_name().type_id == type.name;
      }
      if (value.kind() != Value::Kind::Variant || value.as_variant().type_id != type.name) {
        return false;
      }
      const auto constructor = std::find_if(
          definition->second.constructors.begin(), definition->second.constructors.end(),
          [&](const VariantConstructor& item) {
            return item.name == value.as_variant().constructor;
          });
      if (constructor == definition->second.constructors.end()) return false;
      if (!constructor->payload) return value.as_variant().payload.empty();
      return value.as_variant().payload.size() == 1U &&
             value_matches_type(value.as_variant().payload.front(), *constructor->payload, types);
    }
    case DataType::Kind::Option:
      if (value.kind() != Value::Kind::Variant ||
          value.as_variant().type_id != type_identity(type)) return false;
      if (value.as_variant().constructor == "none") return value.as_variant().payload.empty();
      return value.as_variant().constructor == "some" && value.as_variant().payload.size() == 1U &&
             value_matches_type(value.as_variant().payload.front(), *type.first, types);
    case DataType::Kind::Result:
      if (value.kind() != Value::Kind::Variant ||
          value.as_variant().type_id != type_identity(type) ||
          value.as_variant().payload.size() != 1U) return false;
      if (value.as_variant().constructor == "ok") {
        return value_matches_type(value.as_variant().payload.front(), *type.first, types);
      }
      return value.as_variant().constructor == "err" &&
             value_matches_type(value.as_variant().payload.front(), *type.second, types);
    case DataType::Kind::Tuple:
      if (value.kind() != Value::Kind::Tuple ||
          value.as_tuple().fields.size() != type.elements.size()) return false;
      for (std::size_t index = 0; index < type.elements.size(); ++index) {
        if (!value_matches_type(value.as_tuple().fields[index], type.elements[index], types)) {
          return false;
        }
      }
      return true;
    case DataType::Kind::Relation:
      if (value.kind() != Value::Kind::Relation ||
          value.as_relation().arity != type.elements.size()) return false;
      for (const ValueTuple& row : value.as_relation().rows) {
        if (!value_matches_type(Value(row),
                                DataType(DataType::Kind::Tuple, type.elements), types)) {
          return false;
        }
      }
      return true;
  }
  return false;
}

const Field& find_field(const State& state, std::string_view name) {
  const auto found = std::find_if(state.fields.begin(), state.fields.end(),
                                  [&](const Field& field) { return field.name == name; });
  if (found == state.fields.end()) {
    throw Error("state '" + state.name + "' has no field '" + std::string(name) + "'");
  }
  return *found;
}

using TypeEnvironment = std::map<std::string, DataType, std::less<>>;
using ActionPortRegistry =
    std::map<std::string, std::vector<DataType>, std::less<>>;

#include "semantics.cpp"

DataType infer_type(const ExprPtr& expr, const TypeEnvironment& state,
                    const TypeEnvironment& event, TypeEnvironment locals,
                    const TypeRegistry& types);

DataType infer_type_impl(const ExprPtr& expr, const TypeEnvironment& state,
                         const TypeEnvironment& event, TypeEnvironment locals,
                         const TypeRegistry& types) {
  if (!expr) throw Error("missing expression");
  switch (expr->kind) {
    case Expr::Kind::Literal: return value_type(*expr->literal);
    case Expr::Kind::Name: {
      if (expr->text == "round") return int_type();
      std::string path = expr->text;
      bool before = false;
      if (path.starts_with("before.")) {
        before = true;
        path.erase(0, 7);
      }
      std::size_t cursor = 0;
      auto next_component = [&]() {
        const std::size_t dot = path.find('.', cursor);
        std::string component = path.substr(cursor, dot == std::string::npos ? dot : dot - cursor);
        cursor = dot == std::string::npos ? path.size() : dot + 1U;
        return component;
      };
      std::string root;
      DataType current;
      bool found_root = false;
      root = next_component();
      if (!before) {
        if (const auto found = locals.find(root); found != locals.end()) {
          current = found->second;
          found_root = true;
        } else if (const auto found = event.find(root); found != event.end()) {
          current = found->second;
          found_root = true;
        }
      }
      if (!found_root) {
        // State keys may themselves be qualified (for example
        // scheduler.phase). Prefer the longest matching key before treating
        // the remaining components as record projections.
        cursor = 0;
        std::size_t matched = 0;
        for (const auto& [key, type] : state) {
          if (path == key || (path.starts_with(key) && path.size() > key.size() &&
                              path[key.size()] == '.')) {
            if (key.size() > matched) {
              matched = key.size();
              root = key;
              current = type;
              found_root = true;
            }
          }
        }
        if (found_root) cursor = matched == path.size() ? path.size() : matched + 1U;
      }
      if (!found_root) throw Error("unknown value '" + root + "'");
      while (cursor < path.size()) {
        const std::string field_name = next_component();
        if (current.kind == DataType::Kind::Tuple) {
          std::size_t index = 0;
          const auto parsed = std::from_chars(field_name.data(),
                                              field_name.data() + field_name.size(), index);
          if (parsed.ec != std::errc{} || parsed.ptr != field_name.data() + field_name.size() ||
              index >= current.elements.size()) {
            throw Error("tuple index '" + field_name + "' is out of range");
          }
          // Copy out before replacing current: the selected element is owned
          // by current.elements and would otherwise be invalidated mid-copy.
          DataType selected = current.elements[index];
          current = std::move(selected);
          continue;
        }
        if (current.kind != DataType::Kind::Named) {
          throw Error("field access requires a record or tuple value");
        }
        const TypeDefinition& definition = types.at(current.name);
        if (definition.kind != TypeDefinition::Kind::Record) {
          throw Error("field access requires a record value");
        }
        const auto field = std::find_if(definition.fields.begin(), definition.fields.end(),
                                        [&](const TypeField& item) {
                                          return item.name == field_name;
                                        });
        if (field == definition.fields.end()) {
          throw Error("record '" + definition.name + "' has no field '" + field_name + "'");
        }
        current = field->type;
      }
      return current;
    }
    case Expr::Kind::Unary: {
      const DataType operand = infer_type(expr->left, state, event, std::move(locals), types);
      if (expr->text == "not") {
        if (operand.kind != DataType::Kind::Bool) throw Error("not requires bool");
        return bool_type();
      }
      if (operand.kind != DataType::Kind::Int && operand.kind != DataType::Kind::Rational) {
        throw Error("numeric negation requires int or rational");
      }
      return operand;
    }
    case Expr::Kind::Binary: {
      const DataType left = infer_type(expr->left, state, event, locals, types);
      const DataType right = infer_type(expr->right, state, event, std::move(locals), types);
      if (expr->text == "and" || expr->text == "or" || expr->text == "->") {
        if (left.kind != DataType::Kind::Bool || right.kind != DataType::Kind::Bool) {
          throw Error("logical operators need bool operands");
        }
        return bool_type();
      }
      if (expr->text == "+" || expr->text == "-" || expr->text == "*" ||
          expr->text == "/") {
        const bool left_numeric = left.kind == DataType::Kind::Int ||
                                  left.kind == DataType::Kind::Rational;
        const bool right_numeric = right.kind == DataType::Kind::Int ||
                                   right.kind == DataType::Kind::Rational;
        if (!left_numeric || !right_numeric) {
          throw Error("arithmetic operators need exact numeric operands");
        }
        if (expr->text != "/" && left.kind == DataType::Kind::Int &&
            right.kind == DataType::Kind::Int) return int_type();
        return rational_type();
      }
      throw Error("unknown binary operator '" + expr->text + "'");
    }
    case Expr::Kind::RelationMatch: {
      const DataType left = infer_type(expr->left, state, event, locals, types);
      const DataType right = infer_type(expr->right, state, event, std::move(locals), types);
      return verify_relation_match(*expr, left, right);
    }
    case Expr::Kind::Exists:
    case Expr::Kind::ForAll: {
      const DataType domain = infer_type(expr->left, state, event, locals, types);
      if (domain.kind == DataType::Kind::Set) {
        locals.insert_or_assign(expr->text, *domain.first);
      } else if (domain.kind == DataType::Kind::Relation) {
        expr->direct_relation_binding = domain.direct_relation_row;
        locals.insert_or_assign(
            expr->text, domain.direct_relation_row
                            ? domain.elements.front()
                            : DataType(DataType::Kind::Tuple, domain.elements));
      } else {
        throw Error("quantifier needs a finite set or relation domain");
      }
      if (infer_type(expr->right, state, event, std::move(locals), types).kind !=
          DataType::Kind::Bool) {
        throw Error("quantifier predicate must be bool");
      }
      return bool_type();
    }
    case Expr::Kind::Count:
      if (const DataType collection = infer_type(expr->left, state, event, std::move(locals), types);
          collection.kind != DataType::Kind::List && collection.kind != DataType::Kind::Set &&
          collection.kind != DataType::Kind::Map && collection.kind != DataType::Kind::Bag &&
          collection.kind != DataType::Kind::Relation) {
        throw Error("count needs a finite collection");
      }
      return int_type();
    case Expr::Kind::Select: {
      const DataType domain = infer_type(expr->left, state, event, locals, types);
      if (domain.kind != DataType::Kind::Relation) {
        throw Error("select needs a finite relation domain");
      }
      expr->direct_relation_binding = domain.direct_relation_row;
      const DataType row_type = domain.direct_relation_row
                                    ? domain.elements.front()
                                    : DataType(DataType::Kind::Tuple, domain.elements);
      locals.insert_or_assign(expr->text, row_type);
      if (infer_type(expr->right, state, event, locals, types).kind !=
          DataType::Kind::Bool) {
        throw Error("select predicate must be bool");
      }
      if (expr->children.empty()) throw Error("select requires a lexicographic score");
      for (const ExprPtr& score : expr->children) {
        const DataType score_type = infer_type(score, state, event, locals, types);
        const bool logical_name = score_type.kind == DataType::Kind::Named &&
            types.at(score_type.name).kind == TypeDefinition::Kind::Name;
        if (score_type.kind != DataType::Kind::Bool &&
            score_type.kind != DataType::Kind::Int &&
            score_type.kind != DataType::Kind::Rational &&
            score_type.kind != DataType::Kind::String && !logical_name) {
          throw Error("select lex score must be bool, exact numeric, string, or name");
        }
      }
      DataType result(DataType::Kind::Option, row_type);
      expr->resolved_type = result;
      return result;
    }
    case Expr::Kind::OptionLiteral: {
      if (expr->children.empty()) {
        throw Error("empty option [] requires an expected [T] type");
      }
      if (expr->children.size() != 1U) throw Error("option literal has too many values");
      DataType result(DataType::Kind::Option,
                      infer_type(expr->children.front(), state, event, locals, types));
      expr->resolved_type = result;
      return result;
    }
    case Expr::Kind::NameConstruct: {
      if (expr->children.size() != 1U || expr->type_argument ||
          expr->children.front()->kind != Expr::Kind::Literal ||
          !expr->children.front()->literal ||
          expr->children.front()->literal->kind() != Value::Kind::String) {
        throw Error("name constructor '" + expr->text + "' requires one static atom");
      }
      const auto definition = types.find(expr->text);
      if (definition == types.end() || definition->second.kind != TypeDefinition::Kind::Name) {
        throw Error("unknown name type '" + expr->text + "'");
      }
      DataType result(DataType::Kind::Named, expr->text);
      expr->resolved_type = result;
      return result;
    }
    case Expr::Kind::SetInsert:
    case Expr::Kind::SetErase: {
      const DataType collection = infer_type(expr->left, state, event, locals, types);
      if (collection.kind != DataType::Kind::Set ||
          infer_type(expr->right, state, event, std::move(locals), types) != *collection.first) {
        throw Error("insert/erase item type must match finite set element type");
      }
      return collection;
    }
    case Expr::Kind::RecordConstruct: {
      const auto definition = types.find(expr->text);
      if (definition == types.end() || definition->second.kind != TypeDefinition::Kind::Record) {
        throw Error("unknown record constructor '" + expr->text + "'");
      }
      if (expr->names.size() != definition->second.fields.size()) {
        throw Error("record constructor '" + expr->text + "' has missing fields");
      }
      std::unordered_set<std::string> seen;
      for (std::size_t index = 0; index < expr->names.size(); ++index) {
        if (!seen.insert(expr->names[index]).second) throw Error("duplicate record field");
        const auto field = std::find_if(
            definition->second.fields.begin(), definition->second.fields.end(),
            [&](const TypeField& item) { return item.name == expr->names[index]; });
        if (field == definition->second.fields.end()) {
          throw Error("unknown field '" + expr->names[index] + "' in record '" + expr->text + "'");
        }
        if (expr->children[index]->kind == Expr::Kind::OptionLiteral &&
            expr->children[index]->children.empty()) {
          if (field->type.kind != DataType::Kind::Option) {
            throw Error("empty option [] requires an expected [T] type");
          }
          expr->children[index]->resolved_type = field->type;
        } else if (infer_type(expr->children[index], state, event, locals, types) != field->type) {
          throw Error("wrong value type for record field '" + field->name + "'");
        }
      }
      DataType result(DataType::Kind::Named, expr->text);
      expr->resolved_type = result;
      return result;
    }
    case Expr::Kind::Construct: {
      if (active_functions != nullptr) {
        const auto function = active_functions->find(expr->text);
        if (function != active_functions->end()) {
          if (expr->type_argument || expr->children.size() != function->second.parameters.size()) {
            throw Error("function '" + expr->text + "' has the wrong arguments");
          }
          for (std::size_t index = 0; index < expr->children.size(); ++index) {
            if (infer_type(expr->children[index], state, event, locals, types) !=
                function->second.parameters[index].type) {
              throw Error("function '" + expr->text + "' argument has the wrong type");
            }
          }
          expr->resolved_type = function->second.result;
          return function->second.result;
        }
      }
      if (expr->text == "tuple") {
        if (expr->children.empty() || expr->type_argument) {
          throw Error("tuple(...) requires at least one value and no type argument");
        }
        std::vector<DataType> elements;
        elements.reserve(expr->children.size());
        for (const ExprPtr& child : expr->children) {
          elements.push_back(infer_type(child, state, event, locals, types));
        }
        DataType result(DataType::Kind::Tuple, std::move(elements));
        expr->resolved_type = result;
        return result;
      }
      const bool relation_operation =
          expr->text == "project" || expr->text == "join" ||
          expr->text == "compose" || expr->text == "inverse" ||
          expr->text == "closure" || expr->text == "union" ||
          expr->text == "intersection" || expr->text == "difference";
      if (relation_operation) {
        if (expr->type_argument) throw Error("relation operation cannot take a type argument");
        const auto argument_type = [&](std::size_t index) {
          if (index >= expr->children.size()) throw Error("relation operation has too few arguments");
          return infer_type(expr->children[index], state, event, locals, types);
        };
        const auto literal_index = [&](std::size_t index) {
          if (index >= expr->children.size() ||
              expr->children[index]->kind != Expr::Kind::Literal ||
              !expr->children[index]->literal ||
              expr->children[index]->literal->kind() != Value::Kind::Int ||
              expr->children[index]->literal->as_exact_int().is_negative() ||
              !expr->children[index]->literal->as_exact_int().fits_int64()) {
            throw Error("relation column index must be a non-negative int literal");
          }
          return static_cast<std::size_t>(expr->children[index]->literal->as_int());
        };
        DataType result;
        if (expr->text == "project") {
          const DataType relation = argument_type(0);
          if (relation.kind != DataType::Kind::Relation || expr->children.size() < 2U) {
            throw Error("project(relation, column...) requires a relation and columns");
          }
          std::set<std::size_t> seen;
          std::vector<DataType> projected;
          for (std::size_t index = 1; index < expr->children.size(); ++index) {
            const std::size_t column = literal_index(index);
            if (column >= relation.elements.size()) throw Error("project column is out of range");
            if (!seen.insert(column).second) throw Error("project repeats a column");
            projected.push_back(relation.elements[column]);
          }
          result = DataType(DataType::Kind::Relation, std::move(projected));
        } else if (expr->text == "inverse") {
          const DataType relation = argument_type(0);
          if (expr->children.size() != 1U || relation.kind != DataType::Kind::Relation ||
              relation.elements.size() != 2U) {
            throw Error("inverse requires one binary relation");
          }
          result = DataType(DataType::Kind::Relation,
                            std::vector<DataType>{relation.elements[1], relation.elements[0]});
        } else if (expr->text == "closure") {
          const DataType relation = argument_type(0);
          if (expr->children.size() != 1U || relation.kind != DataType::Kind::Relation ||
              relation.elements.size() != 2U ||
              relation.elements[0] != relation.elements[1]) {
            throw Error("closure requires one homogeneous binary relation");
          }
          result = relation;
        } else if (expr->text == "compose") {
          const DataType left = argument_type(0);
          const DataType right = argument_type(1);
          if (expr->children.size() != 2U || left.kind != DataType::Kind::Relation ||
              right.kind != DataType::Kind::Relation || left.elements.size() != 2U ||
              right.elements.size() != 2U || left.elements[1] != right.elements[0]) {
            throw Error("compose requires compatible binary relations");
          }
          result = DataType(DataType::Kind::Relation,
                            std::vector<DataType>{left.elements[0], right.elements[1]});
        } else if (expr->text == "join") {
          if (expr->children.size() != 4U) {
            throw Error("join(left, leftColumn, right, rightColumn) needs four arguments");
          }
          const DataType left = argument_type(0);
          const std::size_t left_column = literal_index(1);
          const DataType right = argument_type(2);
          const std::size_t right_column = literal_index(3);
          if (left.kind != DataType::Kind::Relation || right.kind != DataType::Kind::Relation ||
              left_column >= left.elements.size() || right_column >= right.elements.size() ||
              left.elements[left_column] != right.elements[right_column]) {
            throw Error("join columns are out of range or have different types");
          }
          std::vector<DataType> joined = left.elements;
          joined.insert(joined.end(), right.elements.begin(), right.elements.end());
          result = DataType(DataType::Kind::Relation, std::move(joined));
        } else {
          const DataType left = argument_type(0);
          const DataType right = argument_type(1);
          if (expr->children.size() != 2U || left.kind != DataType::Kind::Relation ||
              left != right) {
            throw Error(expr->text + " requires two relations of the same type");
          }
          result = left;
        }
        expr->resolved_type = result;
        return result;
      }
      if (expr->text == "none") {
        if (!expr->children.empty() || !expr->type_argument) {
          throw Error("none<T>() requires one type argument and no value");
        }
        DataType result(DataType::Kind::Option, *expr->type_argument);
        expr->resolved_type = result;
        return result;
      }
      if (expr->text == "some") {
        if (expr->children.size() != 1U || expr->type_argument) {
          throw Error("some(value) takes exactly one value");
        }
        DataType result(DataType::Kind::Option,
                        infer_type(expr->children.front(), state, event, locals, types));
        expr->resolved_type = result;
        return result;
      }
      if (expr->text == "ok" || expr->text == "err") {
        if (expr->children.size() != 1U || !expr->type_argument) {
          throw Error(expr->text + "<T>(value) requires the opposite result type");
        }
        const DataType payload = infer_type(expr->children.front(), state, event, locals, types);
        DataType result = expr->text == "ok"
                              ? DataType(DataType::Kind::Result, payload, *expr->type_argument)
                              : DataType(DataType::Kind::Result, *expr->type_argument, payload);
        expr->resolved_type = result;
        return result;
      }
      if (const auto definition = types.find(expr->text);
          definition != types.end() && definition->second.kind == TypeDefinition::Kind::Newtype) {
        if (expr->children.size() != 1U || expr->type_argument ||
            infer_type(expr->children.front(), state, event, locals, types) !=
                *definition->second.underlying) {
          throw Error("newtype constructor '" + expr->text + "' has the wrong payload");
        }
        DataType result(DataType::Kind::Named, expr->text);
        expr->resolved_type = result;
        return result;
      }
      const std::size_t dot = expr->text.rfind('.');
      if (dot == std::string::npos) throw Error("unknown constructor '" + expr->text + "'");
      const std::string type_name = expr->text.substr(0, dot);
      const std::string constructor_name = expr->text.substr(dot + 1U);
      const auto definition = types.find(type_name);
      if (definition == types.end() || definition->second.kind != TypeDefinition::Kind::Variant) {
        throw Error("unknown variant type '" + type_name + "'");
      }
      const auto constructor = std::find_if(
          definition->second.constructors.begin(), definition->second.constructors.end(),
          [&](const VariantConstructor& item) { return item.name == constructor_name; });
      if (constructor == definition->second.constructors.end()) {
        throw Error("unknown constructor '" + expr->text + "'");
      }
      if (static_cast<bool>(constructor->payload) != (expr->children.size() == 1U) ||
          expr->children.size() > 1U || expr->type_argument) {
        throw Error("constructor '" + expr->text + "' has the wrong arity");
      }
      if (constructor->payload &&
          infer_type(expr->children.front(), state, event, locals, types) !=
              *constructor->payload) {
        throw Error("constructor '" + expr->text + "' has the wrong payload type");
      }
      DataType result(DataType::Kind::Named, type_name);
      expr->resolved_type = result;
      return result;
    }
    case Expr::Kind::Match: {
      const DataType scrutinee = infer_type(expr->left, state, event, locals, types);
      std::map<std::string, std::optional<DataType>, std::less<>> constructors;
      if (scrutinee.kind == DataType::Kind::Option) {
        constructors.emplace("none", std::nullopt);
        constructors.emplace("some", *scrutinee.first);
      } else if (scrutinee.kind == DataType::Kind::Result) {
        constructors.emplace("ok", *scrutinee.first);
        constructors.emplace("err", *scrutinee.second);
      } else if (scrutinee.kind == DataType::Kind::Named) {
        const TypeDefinition& definition = types.at(scrutinee.name);
        if (definition.kind != TypeDefinition::Kind::Variant) {
          throw Error("match requires a variant, enum, option, or result");
        }
        for (const VariantConstructor& constructor : definition.constructors) {
          constructors.emplace(constructor.name, constructor.payload);
        }
      } else {
        throw Error("match requires a variant, enum, option, or result");
      }
      std::set<std::string, std::less<>> covered;
      bool wildcard = false;
      std::optional<DataType> result_type;
      for (std::size_t index = 0; index < expr->arms.size(); ++index) {
        const MatchArm& arm = expr->arms[index];
        TypeEnvironment arm_locals = locals;
        if (arm.wildcard) {
          if (wildcard || index + 1U != expr->arms.size()) {
            throw Error("match wildcard must be the unique final arm");
          }
          wildcard = true;
        } else {
          const std::size_t dot = arm.constructor.rfind('.');
          const std::string constructor_name =
              dot == std::string::npos ? arm.constructor : arm.constructor.substr(dot + 1U);
          if (dot != std::string::npos && scrutinee.kind == DataType::Kind::Named &&
              arm.constructor.substr(0, dot) != scrutinee.name) {
            throw Error("pattern constructor belongs to a different variant");
          }
          const auto constructor = constructors.find(constructor_name);
          if (constructor == constructors.end()) {
            throw Error("unknown pattern constructor '" + arm.constructor + "'");
          }
          if (!covered.insert(constructor_name).second) {
            throw Error("duplicate pattern constructor '" + constructor_name + "'");
          }
          if (constructor->second) {
            if (arm.binding.empty()) throw Error("payload pattern requires a binding");
            arm_locals.insert_or_assign(arm.binding, *constructor->second);
          } else if (!arm.binding.empty()) {
            throw Error("payload-free constructor cannot bind a value");
          }
        }
        const DataType arm_type = infer_type(arm.body, state, event, std::move(arm_locals), types);
        if (!result_type) result_type = arm_type;
        else if (*result_type != arm_type) throw Error("match arms have different result types");
      }
      if (!result_type) throw Error("match requires at least one arm");
      if (!wildcard && covered.size() != constructors.size()) {
        throw Error("non-exhaustive match expression");
      }
      expr->resolved_type = *result_type;
      return *result_type;
    }
  }
  throw Error("invalid expression");
}

DataType infer_type(const ExprPtr& expr, const TypeEnvironment& state,
                    const TypeEnvironment& event, TypeEnvironment locals,
                    const TypeRegistry& types) {
  try {
    return infer_type_impl(expr, state, event, std::move(locals), types);
  } catch (const Error& error) {
    if (error.line() == 0 && expr && expr->line != 0) {
      throw Error(error.what(), expr->line, expr->column);
    }
    throw;
  }
}

void verify_action(const std::shared_ptr<ActionExpr>& action, const TypeEnvironment& state,
                   const TypeEnvironment& event, std::unordered_set<std::string>& labels,
                   const TypeRegistry& types, const ActionPortRegistry& ports) {
  if (!action) return;
  if (action->kind != ActionExpr::Kind::Call) {
    for (const auto& child : action->children) {
      verify_action(child, state, event, labels, types, ports);
    }
    return;
  }
  if (!labels.insert(action->label).second) {
    throw Error("duplicate action label '" + action->label + "'");
  }
  if (!ports.empty()) {
    const auto found = ports.find(action->function);
    if (found == ports.end()) {
      throw Error("action calls undeclared typed port '" + action->function + "'");
    }
    if (found->second.size() != action->arguments.size()) {
      throw Error("action arguments do not match typed port '" + action->function + "'");
    }
    for (std::size_t index = 0; index < action->arguments.size(); ++index) {
      const ExprPtr& argument = action->arguments[index];
      const DataType& expected = found->second[index];
      if (argument->kind == Expr::Kind::OptionLiteral && argument->children.empty()) {
        if (expected.kind != DataType::Kind::Option) {
          throw Error("empty option [] requires an expected [T] type");
        }
        argument->resolved_type = expected;
      } else if (infer_type(argument, state, event, {}, types) != expected) {
        throw Error("action arguments do not match typed port '" + action->function + "'");
      }
    }
  } else {
    for (const ExprPtr& argument : action->arguments) {
      (void)infer_type(argument, state, event, {}, types);
    }
  }
  if (const auto parameter = event.find(action->context);
      parameter != event.end() && parameter->second.kind != DataType::Kind::String) {
    throw Error("action context event field must be string");
  }
  if (action->context.starts_with("before.")) {
    const std::string field = action->context.substr(action->context.find('.') + 1);
    const auto found = state.find(field);
    if (found == state.end()) throw Error("unknown context field '" + field + "'");
    if (found->second.kind != DataType::Kind::String) {
      throw Error("action context field must be string");
    }
  }
}

void collect_reads(const ExprPtr& expr, const TypeEnvironment& state,
                   std::set<std::string, std::less<>> shadowed,
                   std::set<std::string, std::less<>>& reads) {
  if (!expr) return;
  if (expr->kind == Expr::Kind::Name) {
    std::string path = expr->text.starts_with("before.") ? expr->text.substr(7) : expr->text;
    std::string best;
    for (const auto& [key, type] : state) {
      static_cast<void>(type);
      if ((path == key || (path.starts_with(key) && path.size() > key.size() &&
                           path[key.size()] == '.')) &&
          key.size() > best.size()) {
        best = key;
      }
    }
    if (!best.empty() && !shadowed.contains(best)) reads.insert(std::move(best));
    return;
  }
  if (expr->kind == Expr::Kind::Exists || expr->kind == Expr::Kind::ForAll) {
    collect_reads(expr->left, state, shadowed, reads);
    shadowed.insert(expr->text);
    collect_reads(expr->right, state, std::move(shadowed), reads);
    return;
  }
  if (expr->kind == Expr::Kind::Select) {
    collect_reads(expr->left, state, shadowed, reads);
    shadowed.insert(expr->text);
    collect_reads(expr->right, state, shadowed, reads);
    for (const ExprPtr& score : expr->children) {
      collect_reads(score, state, shadowed, reads);
    }
    return;
  }
  if (expr->kind == Expr::Kind::Match) {
    collect_reads(expr->left, state, shadowed, reads);
    for (const MatchArm& arm : expr->arms) {
      auto arm_shadowed = shadowed;
      if (!arm.binding.empty()) arm_shadowed.insert(arm.binding);
      collect_reads(arm.body, state, std::move(arm_shadowed), reads);
    }
    return;
  }
  collect_reads(expr->left, state, shadowed, reads);
  collect_reads(expr->right, state, shadowed, reads);
  collect_reads(expr->third, state, shadowed, reads);
  for (const ExprPtr& child : expr->children) {
    collect_reads(child, state, shadowed, reads);
  }
}

void verify_temporal_expression(const TemporalExprPtr& expression,
                                const TypeEnvironment& state,
                                const TypeRegistry& types) {
  if (!expression) throw Error("missing temporal expression");
  if (expression->kind == TemporalExpr::Kind::Atom) {
    if (!expression->atom ||
        infer_type(expression->atom, state, {}, {}, types).kind !=
            DataType::Kind::Bool) {
      throw Error("temporal atom must be bool", expression->line,
                  expression->column);
    }
    return;
  }
  if (!expression->left) {
    throw Error("temporal operator is missing an operand", expression->line,
                expression->column);
  }
  verify_temporal_expression(expression->left, state, types);
  if (expression->kind == TemporalExpr::Kind::TraceRelationMatch ||
      expression->kind == TemporalExpr::Kind::And ||
      expression->kind == TemporalExpr::Kind::Or ||
      expression->kind == TemporalExpr::Kind::Until ||
      expression->kind == TemporalExpr::Kind::Since) {
    if (!expression->right) {
      throw Error("binary temporal operator is missing an operand",
                  expression->line, expression->column);
    }
    verify_temporal_expression(expression->right, state, types);
  }
  if (expression->kind == TemporalExpr::Kind::TraceRelationMatch &&
      expression->relation != "happens_before") {
    throw Error("unknown trace relation '" + expression->relation + "'",
                expression->line, expression->column);
  }
}

std::string state_key(std::string_view context, std::string_view field) {
  return context.empty() ? std::string(field)
                         : std::string(context) + "." + std::string(field);
}

std::map<std::string, Value, std::less<>> local_state_values(
    const std::map<std::string, Value, std::less<>>& values,
    std::string_view context, const State& state) {
  std::map<std::string, Value, std::less<>> result;
  for (const Field& field : state.fields) {
    auto found = values.find(state_key(context, field.name));
    if (found == values.end()) found = values.find(field.name);
    if (found == values.end()) {
      throw Error("missing field '" + state_key(context, field.name) + "'");
    }
    result.emplace(field.name, found->second);
  }
  return result;
}

std::string binding_set_text(const std::vector<StateBinding>& bindings) {
  std::string result = "{";
  for (std::size_t index = 0; index < bindings.size(); ++index) {
    if (index != 0) result += ", ";
    result += bindings[index].state;
    if (!bindings[index].context.empty()) result += "@" + bindings[index].context;
  }
  result += "}";
  return result;
}

std::string target_set_text(const std::vector<TransitionTarget>& targets) {
  std::vector<StateBinding> bindings;
  bindings.reserve(targets.size());
  for (const TransitionTarget& target : targets) bindings.push_back(target.binding);
  return binding_set_text(bindings);
}

void collect_action_reads(const std::shared_ptr<ActionExpr>& action,
                          const TypeEnvironment& state,
                          const std::set<std::string, std::less<>>& shadowed,
                          std::set<std::string, std::less<>>& reads) {
  if (!action) return;
  if (action->kind != ActionExpr::Kind::Call) {
    for (const auto& child : action->children) {
      collect_action_reads(child, state, shadowed, reads);
    }
    return;
  }
  for (const ExprPtr& argument : action->arguments) {
    collect_reads(argument, state, shadowed, reads);
  }
  if (action->context.starts_with("before.")) {
    reads.insert(action->context.substr(action->context.find('.') + 1));
  } else if (!shadowed.contains(action->context) && state.contains(action->context)) {
    reads.insert(action->context);
  }
}

void inspect_function_body(const ExprPtr& expr, const FunctionRegistry& functions,
                           std::set<std::string, std::less<>>& calls) {
  if (!expr) return;
  if (expr->kind == Expr::Kind::Name &&
      (expr->text == "round" || expr->text.starts_with("before."))) {
    throw Error("pure function cannot observe automaton state or logical round",
                expr->line, expr->column);
  }
  if (expr->kind == Expr::Kind::Construct && functions.contains(expr->text)) {
    calls.insert(expr->text);
  }
  inspect_function_body(expr->left, functions, calls);
  inspect_function_body(expr->right, functions, calls);
  inspect_function_body(expr->third, functions, calls);
  for (const ExprPtr& child : expr->children) inspect_function_body(child, functions, calls);
  for (const MatchArm& arm : expr->arms) inspect_function_body(arm.body, functions, calls);
}

void verify_program(Program::Impl& program) {
  if (program.states.empty()) throw Error("program must define at least one state");
  FunctionScope function_scope(program.functions);
  static const std::set<std::string, std::less<>> reserved_functions{
      "tuple", "project", "join", "compose", "inverse", "closure",
      "union", "intersection", "difference", "none", "some", "ok", "err"};
  std::map<std::string, std::set<std::string, std::less<>>, std::less<>> call_graph;
  for (const auto& [name, function] : program.functions) {
    if (program.types.contains(name) || reserved_functions.contains(name)) {
      throw Error("function name '" + name + "' conflicts with a type or builtin",
                  function.line, function.column);
    }
    TypeEnvironment parameters;
    for (const Parameter& parameter : function.parameters) {
      if (!parameters.emplace(parameter.name, parameter.type).second) {
        throw Error("duplicate function parameter '" + parameter.name + "'",
                    parameter.line, parameter.column);
      }
    }
    inspect_function_body(function.body, program.functions, call_graph[name]);
    if (infer_type(function.body, {}, {}, parameters, program.types) != function.result) {
      throw Error("function '" + name + "' body has the wrong result type",
                  function.line, function.column);
    }
  }
  std::set<std::string, std::less<>> visiting;
  std::set<std::string, std::less<>> visited;
  std::function<void(const std::string&)> reject_recursive = [&](const std::string& name) {
    if (visiting.contains(name)) throw Error("recursive function cycle contains '" + name + "'");
    if (visited.contains(name)) return;
    visiting.insert(name);
    for (const std::string& called : call_graph[name]) reject_recursive(called);
    visiting.erase(name);
    visited.insert(name);
  };
  for (const auto& [name, function] : program.functions) {
    static_cast<void>(function);
    reject_recursive(name);
  }
  std::unordered_set<std::string> names;
  ActionPortRegistry action_ports;
  for (const ActionPortDeclaration& port : program.action_ports) {
    if (!action_ports.emplace(port.name, port.parameters).second) {
      throw Error("duplicate typed action port '" + port.name + "'", port.line, port.column);
    }
  }
  std::map<std::string, std::size_t, std::less<>> states_per_context;
  std::map<std::string, std::size_t, std::less<>> initials_per_context;
  for (const State& state : program.states) {
    if (!names.insert(state.name).second) {
      throw Error("duplicate state '" + state.name + "'", state.line, state.column);
    }
    ++states_per_context[state.context];
    initials_per_context[state.context] += state.initial ? 1U : 0U;
    std::unordered_set<std::string> fields;
    for (const Field& field : state.fields) {
      if (!fields.insert(field.name).second) {
        throw Error("duplicate field '" + field.name + "' in state '" + state.name + "'",
                    field.line, field.column);
      }
    }
  }
  for (const auto& [context, count] : states_per_context) {
    static_cast<void>(count);
    if (initials_per_context[context] != 1U) {
      throw Error("context '" + context + "' must have exactly one initial state",
                  program.states.front().line, program.states.front().column);
    }
  }
  program.context_index.clear();
  program.context_names.clear();
  for (const auto& [context, count] : states_per_context) {
    static_cast<void>(count);
    const ContextId id = program.context_names.size();
    program.context_index.emplace(context, id);
    program.context_names.push_back(context);
  }
  program.state_index.clear();
  program.state_positions_by_id.clear();
  for (const State& state : program.states) {
    program.state_index.emplace(state.name, invalid_dense_id);
  }
  StateId next_state_id = 0;
  for (auto& [name, id] : program.state_index) {
    static_cast<void>(name);
    id = next_state_id++;
  }
  program.state_positions_by_id.resize(program.states.size());
  for (std::size_t position = 0; position < program.states.size(); ++position) {
    State& state = program.states[position];
    state.context_id = program.context_index.at(state.context);
    state.state_id = program.state_index.at(state.name);
    program.state_positions_by_id[state.state_id] = position;
  }
  program.raw_key_map = RawKeyMap{};
  for (const std::string& context : program.context_names) {
    static_cast<void>(program.raw_key_map.add(RawSlotKind::Control, context));
  }
  std::set<std::string, std::less<>> raw_value_paths;
  for (const State& state : program.states) {
    for (const Field& field : state.fields) {
      raw_value_paths.insert(state_key(state.context, field.name));
    }
  }
  for (const std::string& path : raw_value_paths) {
    static_cast<void>(program.raw_key_map.add(RawSlotKind::Value, path));
  }
  for (auto& [name, procedure] : program.procedures) {
    static_cast<void>(name);
    std::set<std::string, std::less<>> contexts;
    for (StateBinding& binding : procedure.initial_states) {
      const State& state = find_state(program, binding.state);
      if (binding.context.empty()) binding.context = state.context;
      binding.context_id = state.context_id;
      binding.state_id = state.state_id;
      if (binding.context != state.context) {
        throw Error("procedure initial state '" + state.name + "' belongs to @" +
                        state.context + ", not @" + binding.context,
                    binding.line, binding.column);
      }
      if (!contexts.insert(binding.context).second) {
        throw Error("procedure initial combination repeats @" + binding.context,
                    binding.line, binding.column);
      }
    }
  }

  names.clear();
  std::map<std::string, std::vector<Parameter>, std::less<>> event_schemas;
  for (Transition& transition : program.transitions) {
    if (!names.insert(transition.name).second) {
      throw Error("duplicate transition '" + transition.name + "'", transition.line,
                  transition.column);
    }
    if (transition.from.empty() || transition.to.empty()) {
      throw Error("transition '" + transition.name + "' needs from and to", transition.line,
                  transition.column);
    }
    // Expand source-side wildcard patterns to a finite, statically known set
    // of executable alternatives. Target patterns remain forbidden.
    std::vector<TransitionAlternative> raw_routes;
    raw_routes.push_back(TransitionAlternative{transition.case_name,
                                               transition.from, transition.to,
                                               transition.condition, transition.action,
                                               transition.obligation, {}, {}});
    raw_routes.insert(raw_routes.end(), transition.alternatives.begin(),
                      transition.alternatives.end());
    std::map<std::string, std::pair<std::size_t, std::size_t>, std::less<>> case_names;
    for (const TransitionAlternative& route : raw_routes) {
      if (!route.name.empty()) {
        const auto location = std::pair{route.from.front().line, route.from.front().column};
        const auto [found, inserted] = case_names.emplace(route.name, location);
        if (!inserted && found->second != location) {
          throw Error("duplicate case name '" + transition.name + "." + route.name + "'",
                      transition.line, transition.column);
        }
      }
    }
    std::vector<TransitionAlternative> expanded_routes;
    for (const TransitionAlternative& raw : raw_routes) {
      TransitionAlternative recursive = raw;
      const auto expand_source_ancestors = [&](std::vector<StateBinding>& bindings) {
        std::vector<StateBinding> expanded;
        for (const StateBinding& binding : bindings) {
          if (binding.state != "_") {
            const State& state = find_state(program, binding.state);
            for (const std::string& ancestor : state.ancestors) {
              if (std::none_of(bindings.begin(), bindings.end(),
                               [&](const StateBinding& item) {
                                 return item.state == ancestor;
                               }) &&
                  std::none_of(expanded.begin(), expanded.end(),
                               [&](const StateBinding& item) {
                                 return item.state == ancestor;
                               })) {
                expanded.push_back(StateBinding{ancestor, {}, binding.line, binding.column});
              }
            }
          }
          expanded.push_back(binding);
        }
        bindings = std::move(expanded);
      };
      expand_source_ancestors(recursive.from);
      std::vector<TransitionTarget> recursive_targets;
      for (const TransitionTarget& target : recursive.to) {
        const State& state = find_state(program, target.binding.state);
        for (const std::string& ancestor : state.ancestors) {
          if (std::none_of(recursive.to.begin(), recursive.to.end(),
                           [&](const TransitionTarget& item) {
                             return item.binding.state == ancestor;
                           }) &&
              std::none_of(recursive_targets.begin(), recursive_targets.end(),
                           [&](const TransitionTarget& item) {
                             return item.binding.state == ancestor;
                           })) {
            recursive_targets.push_back(TransitionTarget{
                StateBinding{ancestor, {}, target.binding.line,
                             target.binding.column}, {}});
          }
        }
        recursive_targets.push_back(target);
      }
      recursive.to = std::move(recursive_targets);
      std::vector<std::vector<StateBinding>> sources(1);
      for (const StateBinding& binding : recursive.from) {
        std::vector<StateBinding> choices;
        if (binding.state != "_") {
          choices.push_back(binding);
        } else {
          if (binding.context.empty()) {
            throw Error("wildcard state pattern requires an explicit @ context",
                        binding.line, binding.column);
          }
          for (const State& state : program.states) {
            if (state.context == binding.context) {
              choices.push_back(StateBinding{state.name, binding.context,
                                             binding.line, binding.column});
            }
          }
        }
        std::vector<std::vector<StateBinding>> next;
        for (const auto& source : sources) {
          for (const StateBinding& choice : choices) {
            auto item = source;
            item.push_back(choice);
            next.push_back(std::move(item));
          }
        }
        sources = std::move(next);
      }
      for (auto& source : sources) {
        expanded_routes.push_back(TransitionAlternative{
            recursive.name, std::move(source), recursive.to,
            recursive.condition, recursive.action,
            recursive.obligation, {}, {}});
      }
    }
    transition.from = std::move(expanded_routes.front().from);
    transition.to = std::move(expanded_routes.front().to);
    transition.case_name = std::move(expanded_routes.front().name);
    transition.condition = std::move(expanded_routes.front().condition);
    transition.action = std::move(expanded_routes.front().action);
    transition.obligation = std::move(expanded_routes.front().obligation);
    transition.alternatives.assign(
        std::make_move_iterator(expanded_routes.begin() + 1),
        std::make_move_iterator(expanded_routes.end()));
    struct RouteRef {
      std::vector<StateBinding>* sources;
      std::vector<TransitionTarget>* targets;
      ExprPtr* condition;
      std::shared_ptr<ActionExpr>* action;
      TemporalExprPtr* obligation;
      std::set<std::string, std::less<>>* reads;
      std::set<std::string, std::less<>>* writes;
    };
    std::vector<RouteRef> routes;
    routes.push_back(RouteRef{&transition.from, &transition.to,
                              &transition.condition, &transition.action,
                              &transition.obligation,
                              &transition.reads, &transition.writes});
    for (TransitionAlternative& alternative : transition.alternatives) {
      routes.push_back(RouteRef{&alternative.from, &alternative.to,
                                &alternative.condition, &alternative.action,
                                &alternative.obligation,
                                &alternative.reads, &alternative.writes});
    }
    const bool legacy_single = std::all_of(
        routes.begin(), routes.end(), [](const auto& route) {
          return route.sources->size() == 1U && route.targets->size() == 1U;
        });
    TypeEnvironment state_types;
    for (RouteRef& route : routes) {
      auto* sources = route.sources;
      auto* targets = route.targets;
      std::set<std::string, std::less<>> source_contexts;
      for (StateBinding& binding : *sources) {
        const State& source = find_state(program, binding.state);
        if (binding.context.empty()) binding.context = source.context;
        binding.context_id = source.context_id;
        binding.state_id = source.state_id;
        if (binding.context != source.context) {
          throw Error("state '" + source.name + "' belongs to @" + source.context +
                          ", not @" + binding.context,
                      binding.line, binding.column);
        }
        if (!source_contexts.insert(binding.context).second) {
          throw Error("transition source repeats context @" + binding.context,
                      binding.line, binding.column);
        }
        for (const Field& field : source.fields) {
          const std::string key = state_key(binding.context, field.name);
          const auto [found, inserted] = state_types.emplace(key, field.type);
          if (!inserted && found->second != field.type) {
            throw Error("alternative states disagree on field type '" + key + "'");
          }
          if (legacy_single) state_types.emplace(field.name, field.type);
        }
      }
      std::set<std::string, std::less<>> target_contexts;
      for (TransitionTarget& target : *targets) {
        const State& state = find_state(program, target.binding.state);
        if (target.binding.context.empty()) target.binding.context = state.context;
        target.binding.context_id = state.context_id;
        target.binding.state_id = state.state_id;
        if (target.binding.context != state.context) {
          throw Error("state '" + state.name + "' belongs to @" + state.context +
                          ", not @" + target.binding.context,
                      target.binding.line, target.binding.column);
        }
        if (!target_contexts.insert(target.binding.context).second) {
          throw Error("transition target repeats context @" + target.binding.context,
                      target.binding.line, target.binding.column);
        }
      }
    }
    std::unordered_set<std::string> parameters;
    TypeEnvironment event_types;
    for (const Parameter& parameter : transition.parameters) {
      if (!parameters.insert(parameter.name).second) {
        throw Error("duplicate event parameter '" + parameter.name + "'", parameter.line,
                    parameter.column);
      }
      event_types.emplace(parameter.name, parameter.type);
    }
    const auto [schema, inserted] = event_schemas.emplace(transition.event, transition.parameters);
    if (!inserted) {
      const auto& previous = schema->second;
      if (previous.size() != transition.parameters.size()) {
        throw Error("event '" + transition.event + "' has inconsistent schemas",
                    transition.line, transition.column);
      }
      for (std::size_t index = 0; index < previous.size(); ++index) {
        if (previous[index].name != transition.parameters[index].name ||
            previous[index].type != transition.parameters[index].type) {
          throw Error("event '" + transition.event + "' has inconsistent schemas",
                      transition.line, transition.column);
        }
      }
    }
    const auto route_state_types = [&](const RouteRef& route) {
      TypeEnvironment result;
      for (const StateBinding& binding : *route.sources) {
        const State& source = find_state(program, binding.state);
        for (const Field& field : source.fields) {
          result.emplace(state_key(binding.context, field.name), field.type);
          if (legacy_single) result.emplace(field.name, field.type);
        }
      }
      return result;
    };
    TypeEnvironment optimization_types;
    if (transition.optimized_score) {
      bool scope_found = false;
      for (const State& state : program.states) {
        if (state.context != transition.optimization_scope) continue;
        scope_found = true;
        for (const Field& field : state.fields) {
          const std::string qualified =
              state_key(transition.optimization_scope, field.name);
          const auto [qualified_type, inserted] =
              optimization_types.emplace(qualified, field.type);
          if (!inserted && qualified_type->second != field.type) {
            throw Error("states in optimization scope @" +
                            transition.optimization_scope +
                            " disagree on field type '" + field.name + "'",
                        transition.line, transition.column);
          }
          const auto [local_type, local_inserted] =
              optimization_types.emplace(field.name, field.type);
          if (!local_inserted && local_type->second != field.type) {
            throw Error("states in optimization scope @" +
                            transition.optimization_scope +
                            " disagree on field type '" + field.name + "'",
                        transition.line, transition.column);
          }
        }
      }
      if (!scope_found) {
        throw Error("transition optimization references unknown scope @" +
                        transition.optimization_scope,
                    transition.line, transition.column);
      }
      const DataType score_type = infer_type(
          transition.optimized_score, optimization_types, event_types, {}, program.types);
      if (score_type.kind != DataType::Kind::Int &&
          score_type.kind != DataType::Kind::Rational) {
        throw Error("optimized_score in transition '" + transition.name +
                        "' must be int or rational",
                    transition.optimized_score->line,
                    transition.optimized_score->column);
      }
    } else if (!transition.optimization_scope.empty()) {
      throw Error("transition @ scope requires optimized_score",
                  transition.line, transition.column);
    }
    for (const RouteRef& route : routes) {
      const TypeEnvironment local_types = route_state_types(route);
      if (infer_type(*route.condition, local_types, event_types, {}, program.types).kind !=
          DataType::Kind::Bool) {
        throw Error("where clause in transition '" + transition.name + "' must be bool",
                    (*route.condition)->line, (*route.condition)->column);
      }
      if (*route.obligation) {
        TypeEnvironment obligation_types = local_types;
        for (const TransitionTarget& target : *route.targets) {
          const State& target_state = find_state(program, target.binding.state);
          for (const Field& field : target_state.fields) {
            const std::string qualified =
                state_key(target.binding.context, field.name);
            const auto [found, inserted] =
                obligation_types.emplace(qualified, field.type);
            if (!inserted && found->second != field.type) {
              throw Error("transition ensure sees incompatible field type '" +
                              qualified + "'",
                          transition.line, transition.column);
            }
            if (legacy_single) obligation_types.emplace(field.name, field.type);
          }
        }
        verify_temporal_expression(*route.obligation, obligation_types,
                                   program.types);
      }
    }
    for (const RouteRef& route : routes) {
      const TypeEnvironment local_types = route_state_types(route);
      for (const TransitionTarget& target : *route.targets) {
       const State& target_state = find_state(program, target.binding.state);
       std::unordered_set<std::string> assigned;
       for (const Assignment& assignment : target.assignments) {
        const Field& field = find_field(target_state, assignment.field);
        if (!assigned.insert(assignment.field).second) {
          throw Error("field '" + assignment.field + "' is assigned twice", assignment.line,
                      assignment.column);
        }
        if (assignment.value->kind == Expr::Kind::OptionLiteral &&
            assignment.value->children.empty()) {
          if (field.type.kind != DataType::Kind::Option) {
            throw Error("empty option [] requires an expected [T] type",
                        assignment.line, assignment.column);
          }
          assignment.value->resolved_type = field.type;
        } else if (infer_type(assignment.value, local_types, event_types, {}, program.types) !=
                   field.type) {
          throw Error("assignment to '" + assignment.field + "' has the wrong type",
                      assignment.line, assignment.column);
        }
       }
      }
    }
    if (!legacy_single) {
      std::function<void(const std::shared_ptr<ActionExpr>&)> require_context =
          [&](const std::shared_ptr<ActionExpr>& action) {
            if (!action) return;
            if (action->kind == ActionExpr::Kind::Call && action->context.empty()) {
              throw Error("actions in a composite transition require explicit @ context",
                          transition.line, transition.column);
            }
            for (const auto& child : action->children) require_context(child);
          };
      for (const RouteRef& route : routes) require_context(*route.action);
    }
    for (const RouteRef& route : routes) {
      const TypeEnvironment local_types = route_state_types(route);
      std::unordered_set<std::string> labels;
      try {
        verify_action(*route.action, local_types, event_types, labels, program.types,
                      action_ports);
      } catch (const Error& error) {
        if (error.line() != 0) throw;
        throw Error(error.what(), transition.line, transition.column);
      }
    }

    std::set<std::string, std::less<>> shadowed;
    for (const Parameter& parameter : transition.parameters) shadowed.insert(parameter.name);
    for (const RouteRef& route : routes) {
     const TypeEnvironment local_types = route_state_types(route);
     collect_reads(*route.condition, local_types, shadowed, *route.reads);
     for (const TransitionTarget& target : *route.targets) {
      const State& target_state = find_state(program, target.binding.state);
      for (const Assignment& assignment : target.assignments) {
        route.writes->insert(legacy_single
                                 ? assignment.field
                                 : state_key(target.binding.context, assignment.field));
        collect_reads(assignment.value, local_types, shadowed, *route.reads);
      }
      const auto matching_source = std::find_if(
          route.sources->begin(), route.sources->end(), [&](const StateBinding& source) {
            return source.context == target.binding.context &&
                   source.state == target.binding.state;
          });
      if (matching_source == route.sources->end()) {
        for (const Field& field : target_state.fields) {
          route.writes->insert(legacy_single
                                   ? field.name
                                   : state_key(target.binding.context, field.name));
        }
      }
     }
     collect_action_reads(*route.action, local_types, shadowed, *route.reads);
     if (transition.optimized_score) {
       collect_reads(transition.optimized_score, optimization_types, shadowed,
                     *route.reads);
     }
    }
  }
  program.transition_index.clear();
  program.event_index.clear();
  program.route_state_index.clear();
  program.dense_route_state_index.clear();
  program.dense_route_state_index.resize(program.transitions.size());
  for (std::size_t index = 0; index < program.transitions.size(); ++index) {
    const Transition& transition = program.transitions[index];
    program.transition_index.emplace(transition.name, index);
    program.event_index[transition.event].push_back(index);
    std::vector<const std::vector<StateBinding>*> sources{&transition.from};
    for (const TransitionAlternative& alternative : transition.alternatives) {
      sources.push_back(&alternative.from);
    }
    for (std::size_t route_index = 0; route_index < sources.size(); ++route_index) {
      std::vector<std::pair<std::string, std::string>> signature;
      for (const StateBinding& binding : *sources[route_index]) {
        signature.emplace_back(binding.context, binding.state);
      }
      std::sort(signature.begin(), signature.end());
      std::vector<std::string> contexts;
      std::vector<std::string> states;
      for (const auto& [context, state] : signature) {
        contexts.push_back(context);
        states.push_back(state);
      }
      auto& buckets = program.route_state_index[transition.name];
      auto bucket = std::find_if(
          buckets.begin(), buckets.end(), [&](const RouteStateIndexBucket& item) {
            return item.contexts == contexts;
          });
      if (bucket == buckets.end()) {
        buckets.push_back(RouteStateIndexBucket{std::move(contexts), {}});
        bucket = std::prev(buckets.end());
      }
      bucket->routes[states].push_back(route_index);

      std::vector<std::pair<ContextId, StateId>> dense_signature;
      for (const StateBinding& binding : *sources[route_index]) {
        dense_signature.emplace_back(binding.context_id, binding.state_id);
      }
      std::sort(dense_signature.begin(), dense_signature.end());
      std::vector<ContextId> dense_contexts;
      std::vector<StateId> dense_states;
      for (const auto& [context_id, state_id] : dense_signature) {
        dense_contexts.push_back(context_id);
        dense_states.push_back(state_id);
      }
      auto& dense_buckets = program.dense_route_state_index[index];
      auto dense_bucket = std::find_if(
          dense_buckets.begin(), dense_buckets.end(),
          [&](const DenseRouteStateIndexBucket& item) {
            return item.contexts == dense_contexts;
          });
      if (dense_bucket == dense_buckets.end()) {
        dense_buckets.push_back(
            DenseRouteStateIndexBucket{std::move(dense_contexts), {}});
        dense_bucket = std::prev(dense_buckets.end());
      }
      dense_bucket->routes[dense_states].push_back(route_index);
    }
  }
  for (const State& state : program.states) {
    TypeEnvironment state_types;
    for (const Field& field : state.fields) state_types.emplace(field.name, field.type);
    for (const ExprPtr& invariant : state.invariants) {
      if (infer_type(invariant, state_types, {}, {}, program.types).kind != DataType::Kind::Bool) {
        throw Error("invariant in state '" + state.name + "' must be bool", invariant->line,
                    invariant->column);
      }
    }
  }

  names.clear();
  std::set<std::string, std::less<>> qualified_paths;
  for (const Transition& transition : program.transitions) {
    if (!transition.case_name.empty()) {
      qualified_paths.insert(transition.name + "." + transition.case_name);
    }
    for (const TransitionAlternative& alternative : transition.alternatives) {
      if (!alternative.name.empty()) {
        qualified_paths.insert(transition.name + "." + alternative.name);
      }
    }
  }
  for (TraceDeclaration& trace : program.traces) {
    if (!names.insert(trace.name).second) {
      throw Error("duplicate trace '" + trace.name + "'", trace.line, trace.column);
    }
    for (StateBinding& binding : trace.capture) {
      const State& state = find_state(program, binding.state);
      if (binding.context != state.context) {
        throw Error("trace capture '" + binding.state + "' belongs to @" + state.context,
                    binding.line, binding.column);
      }
    }
    for (const std::string& path : trace.paths) {
      const bool base_transition = std::any_of(
          program.transitions.begin(), program.transitions.end(),
          [&](const Transition& transition) { return transition.name == path; });
      if (!base_transition && !qualified_paths.contains(path)) {
        throw Error("trace captures unknown transition path '" + path + "'",
                    trace.line, trace.column);
      }
    }
    for (const std::string& procedure : trace.captured_procedures) {
      if (!program.procedures.contains(procedure)) {
        throw Error("capture filter references unknown procedure '" + procedure + "'",
                    trace.line, trace.column);
      }
    }
    for (const auto& round : trace.replay_procedures) {
      for (const std::string& procedure : round) {
        if (!program.procedures.contains(procedure)) {
          throw Error("replay references unknown procedure '" + procedure + "'",
                      trace.line, trace.column);
        }
      }
    }
  }
  names.clear();
  TypeEnvironment claim_types;
  const bool single_context = states_per_context.size() == 1U;
  for (const State& state : program.states) {
    for (const Field& field : state.fields) {
      const std::string key = state_key(state.context, field.name);
      const auto [found, inserted] = claim_types.emplace(key, field.type);
      if (!inserted && found->second != field.type) {
        throw Error("states in context @" + state.context +
                    " disagree on claim field type '" + field.name + "'");
      }
      if (single_context) claim_types.emplace(field.name, field.type);
    }
  }
  for (const auto& [procedure, declaration] : program.procedures) {
    static_cast<void>(declaration);
    for (const State& state : program.states) {
      for (const Field& field : state.fields) {
        claim_types.emplace(procedure + "." + state_key(state.context, field.name),
                            field.type);
        if (single_context) {
          claim_types.emplace(procedure + "." + field.name, field.type);
        }
      }
    }
  }
  for (ClaimDeclaration& claim : program.claims) {
    if (!names.insert(claim.name).second) {
      throw Error("duplicate Claim '" + claim.name + "'", claim.line, claim.column);
    }
    const State* claimed_state = nullptr;
    if (claim.target_kind == ClaimDeclaration::TargetKind::Trace) {
      if (std::none_of(program.traces.begin(), program.traces.end(),
                       [&](const TraceDeclaration& trace) {
                         return trace.name == claim.target;
                       })) {
        throw Error("Claim references unknown trace '" + claim.target + "'",
                    claim.line, claim.column);
      }
    } else if (claim.target_kind == ClaimDeclaration::TargetKind::State) {
      claimed_state = &find_state(program, claim.target);
    } else if (!program.procedures.contains(claim.target)) {
      throw Error("Claim references unknown procedure '" + claim.target + "'",
                  claim.line, claim.column);
    }
    std::set<std::string, std::less<>> selected_contexts;
    for (const std::string& context : claim.contexts) {
      if (!program.context_index.contains(context)) {
        throw Error("Claim selects unknown context @" + context,
                    claim.line, claim.column);
      }
      if (!selected_contexts.insert(context).second) {
        throw Error("Claim selects context @" + context + " more than once",
                    claim.line, claim.column);
      }
    }
    if (claimed_state != nullptr && !claim.contexts.empty() &&
        !selected_contexts.contains(claimed_state->context)) {
      throw Error("state Claim scope omits @" + claimed_state->context,
                  claim.line, claim.column);
    }
    if (claim.target_kind == ClaimDeclaration::TargetKind::Procedure &&
        !claim.contexts.empty()) {
      std::set<std::string, std::less<>> procedure_contexts;
      for (const StateBinding& binding :
           program.procedures.at(claim.target).initial_states) {
        procedure_contexts.insert(binding.context);
      }
      for (const std::string& context : claim.contexts) {
        if (!procedure_contexts.contains(context)) {
          throw Error("procedure Claim scope @" + context +
                          " is not in its initial state combination",
                      claim.line, claim.column);
        }
      }
    }
    if (claim.count_at_most) {
      const bool base_transition = std::any_of(
          program.transitions.begin(), program.transitions.end(),
          [&](const Transition& transition) { return transition.name == claim.transition; });
      if (!base_transition && !qualified_paths.contains(claim.transition)) {
        throw Error("Claim references unknown transition '" + claim.transition + "'",
                    claim.line, claim.column);
      }
    } else verify_temporal_expression(claim.property, claim_types, program.types);
  }
}

struct Environment {
  const std::map<std::string, Value, std::less<>>& state;
  const Event* event{nullptr};
  std::map<std::string, Value, std::less<>> locals;
  std::uint64_t round{0};
  const std::map<std::string, Value, std::less<>>* before_state{nullptr};
};

Value evaluate(const ExprPtr& expr, Environment& environment);

bool is_exact_numeric(const Value& value) {
  return value.kind() == Value::Kind::Int || value.kind() == Value::Kind::Rational;
}

Rational as_exact_rational(const Value& value) {
  if (value.kind() == Value::Kind::Rational) return value.as_rational();
  if (value.kind() == Value::Kind::Int) {
    return Rational(value.as_exact_int(), ExactInt(1));
  }
  throw Error("expected exact numeric value");
}

bool equal_values(const Value& left, const Value& right) {
  if (left.kind() == right.kind()) return left == right;
  if (is_exact_numeric(left) && is_exact_numeric(right)) {
    return compare(as_exact_rational(left), as_exact_rational(right)) == 0;
  }
  return left == right;
}

int compare_values(const Value& left, const Value& right) {
  if (left.kind() == Value::Kind::Int && right.kind() == Value::Kind::Int) {
    return compare(left.as_exact_int(), right.as_exact_int());
  }
  if (left.kind() == Value::Kind::Rational && right.kind() == Value::Kind::Rational) {
    return compare(left.as_rational(), right.as_rational());
  }
  if (is_exact_numeric(left) && is_exact_numeric(right)) {
    return compare(as_exact_rational(left), as_exact_rational(right));
  }
  if (left.kind() != right.kind()) throw Error("comparison operands have different types");
  switch (left.kind()) {
    case Value::Kind::Bool:
      return static_cast<int>(left.as_bool()) - static_cast<int>(right.as_bool());
    case Value::Kind::Int:
      return compare(left.as_exact_int(), right.as_exact_int());
    case Value::Kind::Rational:
      return compare(left.as_rational(), right.as_rational());
    case Value::Kind::String:
      return left.as_string() < right.as_string() ? -1
             : left.as_string() > right.as_string() ? 1 : 0;
    case Value::Kind::Name:
      if (left.as_name().type_id != right.as_name().type_id) {
        throw Error("comparison operands have different name types");
      }
      return left.as_name().atom < right.as_name().atom ? -1
             : left.as_name().atom > right.as_name().atom ? 1 : 0;
    case Value::Kind::StringSet:
    case Value::Kind::List:
    case Value::Kind::Set:
    case Value::Kind::Map:
    case Value::Kind::Bag:
    case Value::Kind::Record:
    case Value::Kind::Variant:
    case Value::Kind::Newtype:
    case Value::Kind::Tuple:
    case Value::Kind::Relation:
      throw Error("composite values only support equality in ordered expressions");
  }
  throw Error("invalid comparison");
}

template <class LeftIterator, class RightIterator, class CompareItem>
int lexicographic_order(LeftIterator left, LeftIterator left_end,
                        RightIterator right, RightIterator right_end,
                        CompareItem compare_item) {
  while (left != left_end && right != right_end) {
    const int order = compare_item(*left, *right);
    if (order != 0) return order;
    ++left;
    ++right;
  }
  if (left == left_end && right == right_end) return 0;
  return left == left_end ? -1 : 1;
}

int canonical_value_order(const Value& left, const Value& right) {
  if (left.kind() != right.kind()) {
    return static_cast<int>(left.kind()) < static_cast<int>(right.kind()) ? -1 : 1;
  }
  if (left.kind() == Value::Kind::StringSet) {
    const auto& lhs = left.as_string_set().values;
    const auto& rhs = right.as_string_set().values;
    return lexicographic_order(lhs.begin(), lhs.end(), rhs.begin(), rhs.end(),
                               [](const std::string& a, const std::string& b) {
                                 return a < b ? -1 : a > b ? 1 : 0;
                               });
  }
  if (left.kind() == Value::Kind::List) {
    const auto& lhs = left.as_list().values;
    const auto& rhs = right.as_list().values;
    return lexicographic_order(lhs.begin(), lhs.end(), rhs.begin(), rhs.end(),
                               canonical_value_order);
  }
  if (left.kind() == Value::Kind::Set) {
    const auto& lhs = left.as_set().values;
    const auto& rhs = right.as_set().values;
    return lexicographic_order(lhs.begin(), lhs.end(), rhs.begin(), rhs.end(),
                               canonical_value_order);
  }
  if (left.kind() == Value::Kind::Map) {
    const auto& lhs = left.as_map().entries;
    const auto& rhs = right.as_map().entries;
    return lexicographic_order(lhs.begin(), lhs.end(), rhs.begin(), rhs.end(),
                               [](const auto& a, const auto& b) {
                                 const int key = canonical_value_order(a.first, b.first);
                                 return key != 0 ? key : canonical_value_order(a.second, b.second);
                               });
  }
  if (left.kind() == Value::Kind::Bag) {
    const auto& lhs = left.as_bag().entries;
    const auto& rhs = right.as_bag().entries;
    return lexicographic_order(lhs.begin(), lhs.end(), rhs.begin(), rhs.end(),
                               [](const auto& a, const auto& b) {
                                 const int key = canonical_value_order(a.first, b.first);
                                 if (key != 0) return key;
                                 return a.second < b.second ? -1 : a.second > b.second ? 1 : 0;
                               });
  }
  if (left.kind() == Value::Kind::Record) {
    const auto& lhs = left.as_record();
    const auto& rhs = right.as_record();
    if (lhs.type_id != rhs.type_id) return lhs.type_id < rhs.type_id ? -1 : 1;
    return lexicographic_order(lhs.fields.begin(), lhs.fields.end(),
                               rhs.fields.begin(), rhs.fields.end(),
                               [](const auto& a, const auto& b) {
                                 if (a.first != b.first) return a.first < b.first ? -1 : 1;
                                 return canonical_value_order(a.second, b.second);
                               });
  }
  if (left.kind() == Value::Kind::Variant) {
    const auto& lhs = left.as_variant();
    const auto& rhs = right.as_variant();
    if (lhs.type_id != rhs.type_id) return lhs.type_id < rhs.type_id ? -1 : 1;
    if (lhs.constructor != rhs.constructor) return lhs.constructor < rhs.constructor ? -1 : 1;
    return lexicographic_order(lhs.payload.begin(), lhs.payload.end(),
                               rhs.payload.begin(), rhs.payload.end(), canonical_value_order);
  }
  if (left.kind() == Value::Kind::Newtype) {
    const auto& lhs = left.as_newtype();
    const auto& rhs = right.as_newtype();
    if (lhs.type_id != rhs.type_id) return lhs.type_id < rhs.type_id ? -1 : 1;
    return canonical_value_order(lhs.payload.front(), rhs.payload.front());
  }
  if (left.kind() == Value::Kind::Name) {
    const auto& lhs = left.as_name();
    const auto& rhs = right.as_name();
    if (lhs.type_id != rhs.type_id) return lhs.type_id < rhs.type_id ? -1 : 1;
    return lhs.atom < rhs.atom ? -1 : lhs.atom > rhs.atom ? 1 : 0;
  }
  if (left.kind() == Value::Kind::Tuple) {
    const auto& lhs = left.as_tuple().fields;
    const auto& rhs = right.as_tuple().fields;
    return lexicographic_order(lhs.begin(), lhs.end(), rhs.begin(), rhs.end(),
                               canonical_value_order);
  }
  if (left.kind() == Value::Kind::Relation) {
    const auto& lhs = left.as_relation();
    const auto& rhs = right.as_relation();
    if (lhs.arity != rhs.arity) return lhs.arity < rhs.arity ? -1 : 1;
    return lexicographic_order(
        lhs.rows.begin(), lhs.rows.end(), rhs.rows.begin(), rhs.rows.end(),
        [](const ValueTuple& a, const ValueTuple& b) {
          return lexicographic_order(a.fields.begin(), a.fields.end(),
                                     b.fields.begin(), b.fields.end(),
                                     canonical_value_order);
        });
  }
  return compare_values(left, right);
}

bool event_less(const Event& left, const Event& right) {
  if (left.name != right.name) return left.name < right.name;
  auto lhs = left.fields.begin();
  auto rhs = right.fields.begin();
  while (lhs != left.fields.end() && rhs != right.fields.end()) {
    if (lhs->first != rhs->first) return lhs->first < rhs->first;
    const int value_order = canonical_value_order(lhs->second, rhs->second);
    if (value_order != 0) return value_order < 0;
    ++lhs;
    ++rhs;
  }
  return lhs == left.fields.end() && rhs != right.fields.end();
}

bool transition_input_less(const TransitionInput& left,
                           const TransitionInput& right) {
  if (left.transition != right.transition) return left.transition < right.transition;
  return event_less(Event{left.transition, left.fields},
                    Event{right.transition, right.fields});
}

Value resolve_name(const std::string& name, const Environment& environment) {
  if (name == "round") {
    if (environment.round > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
      throw Error("simulation round exceeds int range");
    }
    return Value(static_cast<std::int64_t>(environment.round));
  }
  std::string path = name;
  bool before = false;
  if (path.starts_with("before.")) {
    before = true;
    path.erase(0, 7);
  }
  std::size_t cursor = 0;
  auto next_component = [&]() {
    const std::size_t dot = path.find('.', cursor);
    std::string component = path.substr(cursor, dot == std::string::npos ? dot : dot - cursor);
    cursor = dot == std::string::npos ? path.size() : dot + 1U;
    return component;
  };
  std::string root;
  Value value(false);
  bool found_root = false;
  root = next_component();
  if (!before) {
    if (const auto local = environment.locals.find(root);
        local != environment.locals.end()) {
      value = local->second;
      found_root = true;
    } else if (environment.event != nullptr) {
      const auto found = environment.event->fields.find(root);
      if (found != environment.event->fields.end()) {
        value = found->second;
        found_root = true;
      }
    }
  }
  if (!found_root) {
    const auto& state_values = before && environment.before_state != nullptr
                                   ? *environment.before_state
                                   : environment.state;
    cursor = 0;
    std::size_t matched = 0;
    for (const auto& [key, item] : state_values) {
      if (path == key || (path.starts_with(key) && path.size() > key.size() &&
                          path[key.size()] == '.')) {
        if (key.size() > matched) {
          matched = key.size();
          root = key;
          value = item;
          found_root = true;
        }
      }
    }
    if (found_root) cursor = matched == path.size() ? path.size() : matched + 1U;
  }
  if (!found_root) throw Error("unknown value '" + root + "'");
  while (cursor < path.size()) {
    const std::string field_name = next_component();
    if (value.kind() == Value::Kind::Tuple) {
      std::size_t index = 0;
      const auto parsed = std::from_chars(field_name.data(),
                                          field_name.data() + field_name.size(), index);
      if (parsed.ec != std::errc{} || parsed.ptr != field_name.data() + field_name.size() ||
          index >= value.as_tuple().fields.size()) {
        throw Error("tuple index '" + field_name + "' is out of range");
      }
      Value selected = value.as_tuple().fields[index];
      value = std::move(selected);
      continue;
    }
    const auto field = std::lower_bound(
        value.as_record().fields.begin(), value.as_record().fields.end(), field_name,
        [](const auto& item, const std::string& target) { return item.first < target; });
    if (field == value.as_record().fields.end() || field->first != field_name) {
      throw Error("record has no field '" + field_name + "'");
    }
    Value selected = field->second;
    value = std::move(selected);
  }
  return value;
}

Value evaluate(const ExprPtr& expr, Environment& environment) {
  if (!expr) throw Error("missing expression");
  switch (expr->kind) {
    case Expr::Kind::Literal: return *expr->literal;
    case Expr::Kind::Name: return resolve_name(expr->text, environment);
    case Expr::Kind::Unary: {
      const Value operand = evaluate(expr->left, environment);
      if (expr->text == "not") return Value(!operand.as_bool());
      if (expr->text == "-") {
        if (operand.kind() == Value::Kind::Int) return Value(-operand.as_exact_int());
        return Value(-operand.as_rational());
      }
      throw Error("unknown unary operator '" + expr->text + "'");
    }
    case Expr::Kind::Binary: {
      if (expr->text == "and") {
        const bool left = evaluate(expr->left, environment).as_bool();
        return Value(left && evaluate(expr->right, environment).as_bool());
      }
      if (expr->text == "or") {
        const bool left = evaluate(expr->left, environment).as_bool();
        return Value(left || evaluate(expr->right, environment).as_bool());
      }
      if (expr->text == "->") {
        const bool left = evaluate(expr->left, environment).as_bool();
        return Value(!left || evaluate(expr->right, environment).as_bool());
      }
      const Value left = evaluate(expr->left, environment);
      const Value right = evaluate(expr->right, environment);
      if (expr->text == "+" || expr->text == "-" || expr->text == "*" ||
          expr->text == "/") {
        if (expr->text != "/" && left.kind() == Value::Kind::Int &&
            right.kind() == Value::Kind::Int) {
          if (expr->text == "+") return Value(left.as_exact_int() + right.as_exact_int());
          if (expr->text == "-") return Value(left.as_exact_int() - right.as_exact_int());
          return Value(left.as_exact_int() * right.as_exact_int());
        }
        const Rational lhs = as_exact_rational(left);
        const Rational rhs = as_exact_rational(right);
        if (expr->text == "+") return Value(lhs + rhs);
        if (expr->text == "-") return Value(lhs - rhs);
        if (expr->text == "*") return Value(lhs * rhs);
        return Value(lhs / rhs);
      }
      throw Error("unknown binary operator '" + expr->text + "'");
    }
    case Expr::Kind::RelationMatch: {
      // This is the single executable path for typed relation predicates.
      // Runtime guards, invariants, functions and ClaimMonitor leaves all call
      // evaluate(), so none may implement equality/order/membership separately.
      const Value left = evaluate(expr->left, environment);
      const Value right = evaluate(expr->right, environment);
      return evaluate_relation_match(*expr, left, right);
    }
    case Expr::Kind::Exists:
    case Expr::Kind::ForAll: {
      const Value domain_value = evaluate(expr->left, environment);
      const auto previous = environment.locals.find(expr->text);
      const std::optional<Value> saved = previous == environment.locals.end()
                                             ? std::nullopt
                                             : std::optional<Value>(previous->second);
      const auto test_item = [&](const Value& item) {
        environment.locals.insert_or_assign(expr->text, item);
        return evaluate(expr->right, environment).as_bool();
      };
      const bool existential = expr->kind == Expr::Kind::Exists;
      bool result = !existential;
      std::size_t work = 0;
      const auto accept = [&](const Value& item) {
        if (++work > relation_work_limit) throw Error("quantifier exceeds work budget");
        const bool matches = test_item(item);
        if ((existential && matches) || (!existential && !matches)) {
          result = existential;
          return true;
        }
        return false;
      };
      if (domain_value.kind() == Value::Kind::StringSet) {
        for (const std::string& item : domain_value.as_string_set().values) {
          if (accept(Value(item))) break;
        }
      } else if (domain_value.kind() == Value::Kind::Set) {
        for (const Value& item : domain_value.as_set().values) {
          if (accept(item)) break;
        }
      } else {
        for (const ValueTuple& row : domain_value.as_relation().rows) {
          const Value item = expr->direct_relation_binding ? row.fields.front() : Value(row);
          if (accept(item)) break;
        }
      }
      if (saved) environment.locals.insert_or_assign(expr->text, *saved);
      else environment.locals.erase(expr->text);
      return Value(result);
    }
    case Expr::Kind::Select: {
      const Value domain = evaluate(expr->left, environment);
      const auto previous = environment.locals.find(expr->text);
      const std::optional<Value> saved = previous == environment.locals.end()
                                             ? std::nullopt
                                             : std::optional<Value>(previous->second);
      std::optional<ValueTuple> selected;
      std::vector<Value> selected_score;
      for (const ValueTuple& row : domain.as_relation().rows) {
        const Value item = expr->direct_relation_binding ? row.fields.front() : Value(row);
        environment.locals.insert_or_assign(expr->text, item);
        if (!evaluate(expr->right, environment).as_bool()) continue;
        std::vector<Value> score;
        score.reserve(expr->children.size());
        for (const ExprPtr& component : expr->children) {
          score.push_back(evaluate(component, environment));
        }
        if (!selected) {
          selected = row;
          selected_score = std::move(score);
          continue;
        }
        int order = 0;
        for (std::size_t index = 0; index < score.size(); ++index) {
          order = compare_values(score[index], selected_score[index]);
          if (order != 0) break;
        }
        if (order == 0 && row != *selected) {
          throw Error("select lex score is ambiguous for distinct relation rows");
        }
        if (order < 0) {
          selected = row;
          selected_score = std::move(score);
        }
      }
      if (saved) environment.locals.insert_or_assign(expr->text, *saved);
      else environment.locals.erase(expr->text);
      const std::string identity = type_identity(*expr->resolved_type);
      if (!selected) return Value(ValueVariant{identity, "none", {}});
      const Value selected_value = expr->direct_relation_binding
                                       ? selected->fields.front()
                                       : Value(std::move(*selected));
      return Value(ValueVariant{identity, "some", {selected_value}});
    }
    case Expr::Kind::Count: {
      const Value collection = evaluate(expr->left, environment);
      std::size_t size = 0;
      switch (collection.kind()) {
        case Value::Kind::StringSet: size = collection.as_string_set().values.size(); break;
        case Value::Kind::List: size = collection.as_list().values.size(); break;
        case Value::Kind::Set: size = collection.as_set().values.size(); break;
        case Value::Kind::Map: size = collection.as_map().entries.size(); break;
        case Value::Kind::Bag: size = collection.as_bag().entries.size(); break;
        case Value::Kind::Relation: size = collection.as_relation().rows.size(); break;
        case Value::Kind::Bool:
        case Value::Kind::Int:
        case Value::Kind::Rational:
        case Value::Kind::String:
        case Value::Kind::Record:
        case Value::Kind::Variant:
        case Value::Kind::Newtype: throw Error("count needs a finite collection");
        case Value::Kind::Name: throw Error("count needs a finite collection");
        case Value::Kind::Tuple: throw Error("count needs a finite collection");
      }
      if (size > static_cast<std::size_t>(std::numeric_limits<std::int64_t>::max())) {
        throw Error("set size exceeds int range");
      }
      return Value(static_cast<std::int64_t>(size));
    }
    case Expr::Kind::SetInsert:
    case Expr::Kind::SetErase: {
      const Value collection = evaluate(expr->left, environment);
      const Value item = evaluate(expr->right, environment);
      if (collection.kind() == Value::Kind::StringSet) {
        StringSet result = collection.as_string_set();
        if (expr->kind == Expr::Kind::SetInsert) result.values.insert(item.as_string());
        else result.values.erase(item.as_string());
        return Value(std::move(result));
      }
      ValueSet result = collection.as_set();
      if (expr->kind == Expr::Kind::SetInsert) {
        result.values.push_back(item);
      } else {
        result.values.erase(
            std::remove_if(result.values.begin(), result.values.end(), [&](const Value& value) {
              return canonical_compare(value, item) == 0;
            }),
            result.values.end());
      }
      return Value(std::move(result));
    }
    case Expr::Kind::RecordConstruct: {
      ValueRecord record{expr->text, {}};
      for (std::size_t index = 0; index < expr->names.size(); ++index) {
        record.fields.emplace_back(expr->names[index], evaluate(expr->children[index], environment));
      }
      return Value(std::move(record));
    }
    case Expr::Kind::OptionLiteral: {
      if (!expr->resolved_type) throw Error("unverified option literal");
      const std::string identity = type_identity(*expr->resolved_type);
      if (expr->children.empty()) return Value(ValueVariant{identity, "none", {}});
      return Value(ValueVariant{identity, "some",
                                {evaluate(expr->children.front(), environment)}});
    }
    case Expr::Kind::NameConstruct:
      if (!expr->resolved_type || expr->children.size() != 1U ||
          !expr->children.front()->literal) {
        throw Error("unverified name constructor");
      }
      return Value(ValueName{expr->text,
                             expr->children.front()->literal->as_string()});
    case Expr::Kind::Construct: {
      if (!expr->resolved_type) throw Error("unverified constructor expression");
      if (active_functions != nullptr) {
        const auto function = active_functions->find(expr->text);
        if (function != active_functions->end()) {
          static const std::map<std::string, Value, std::less<>> empty_state;
          Environment nested{empty_state, nullptr, {}, 0};
          for (std::size_t index = 0; index < expr->children.size(); ++index) {
            nested.locals.emplace(function->second.parameters[index].name,
                                  evaluate(expr->children[index], environment));
          }
          return evaluate(function->second.body, nested);
        }
      }
      if (expr->text == "tuple") {
        ValueTuple tuple;
        for (const ExprPtr& child : expr->children) {
          tuple.fields.push_back(evaluate(child, environment));
        }
        return Value(std::move(tuple));
      }
      const bool relation_operation =
          expr->text == "project" || expr->text == "join" ||
          expr->text == "compose" || expr->text == "inverse" ||
          expr->text == "closure" || expr->text == "union" ||
          expr->text == "intersection" || expr->text == "difference";
      if (relation_operation) {
        const auto column = [&](std::size_t index) {
          return static_cast<std::size_t>(expr->children[index]->literal->as_int());
        };
        const auto row_less = [](const ValueTuple& left, const ValueTuple& right) {
          for (std::size_t index = 0; index < left.fields.size(); ++index) {
            const int order = canonical_compare(left.fields[index], right.fields[index]);
            if (order != 0) return order < 0;
          }
          return false;
        };
        if (expr->text == "project") {
          const ValueRelation input = evaluate(expr->children[0], environment).as_relation();
          ValueRelation output{expr->children.size() - 1U, {}};
          output.rows.reserve(input.rows.size());
          for (const ValueTuple& row : input.rows) {
            ValueTuple projected;
            for (std::size_t index = 1; index < expr->children.size(); ++index) {
              projected.fields.push_back(row.fields[column(index)]);
            }
            output.rows.push_back(std::move(projected));
          }
          return Value(std::move(output));
        }
        if (expr->text == "inverse") {
          const ValueRelation input = evaluate(expr->children[0], environment).as_relation();
          ValueRelation output{2, {}};
          output.rows.reserve(input.rows.size());
          for (const ValueTuple& row : input.rows) {
            output.rows.push_back(ValueTuple{{row.fields[1], row.fields[0]}});
          }
          return Value(std::move(output));
        }
        if (expr->text == "union" || expr->text == "intersection" ||
            expr->text == "difference") {
          const ValueRelation left = evaluate(expr->children[0], environment).as_relation();
          const ValueRelation right = evaluate(expr->children[1], environment).as_relation();
          ValueRelation output{left.arity, {}};
          if (expr->text == "union") {
            output.rows = left.rows;
            output.rows.insert(output.rows.end(), right.rows.begin(), right.rows.end());
          } else {
            for (const ValueTuple& row : left.rows) {
              const bool present = std::binary_search(right.rows.begin(), right.rows.end(), row,
                                                      row_less);
              if ((expr->text == "intersection" && present) ||
                  (expr->text == "difference" && !present)) {
                output.rows.push_back(row);
              }
            }
          }
          return Value(std::move(output));
        }
        if (expr->text == "join" || expr->text == "compose") {
          const ValueRelation left = evaluate(expr->children[0], environment).as_relation();
          const std::size_t left_column = expr->text == "join" ? column(1) : 1U;
          const std::size_t right_argument = expr->text == "join" ? 2U : 1U;
          const ValueRelation right =
              evaluate(expr->children[right_argument], environment).as_relation();
          const std::size_t right_column = expr->text == "join" ? column(3) : 0U;
          ValueRelation output{expr->text == "join" ? left.arity + right.arity : 2U, {}};
          std::set<ValueTuple, decltype(row_less)> unique_rows(row_less);
          std::size_t work = 0;
          for (const ValueTuple& lhs : left.rows) {
            for (const ValueTuple& rhs : right.rows) {
              if (++work > relation_work_limit) throw Error("relation join exceeds work budget");
              if (!equal_values(lhs.fields[left_column], rhs.fields[right_column])) continue;
              ValueTuple row;
              if (expr->text == "join") {
                row.fields = lhs.fields;
                row.fields.insert(row.fields.end(), rhs.fields.begin(), rhs.fields.end());
              } else {
                row.fields = {lhs.fields[0], rhs.fields[1]};
              }
              unique_rows.insert(std::move(row));
              if (unique_rows.size() > relation_row_limit) {
                throw Error("relation join/compose exceeds row budget");
              }
            }
          }
          output.rows.assign(unique_rows.begin(), unique_rows.end());
          return Value(std::move(output));
        }
        ValueRelation current = evaluate(expr->children[0], environment).as_relation();
        std::size_t work = 0;
        for (;;) {
          std::vector<ValueTuple> additions;
          for (const ValueTuple& left : current.rows) {
            for (const ValueTuple& right : current.rows) {
              if (++work > relation_work_limit) throw Error("relation closure exceeds work budget");
              if (!equal_values(left.fields[1], right.fields[0])) continue;
              ValueTuple candidate{{left.fields[0], right.fields[1]}};
              if (!std::binary_search(current.rows.begin(), current.rows.end(), candidate,
                                      row_less)) {
                additions.push_back(std::move(candidate));
              }
            }
          }
          if (additions.empty()) return Value(std::move(current));
          current.rows.insert(current.rows.end(), additions.begin(), additions.end());
          const Value normalized(std::move(current));
          current = normalized.as_relation();
          if (current.rows.size() > relation_row_limit) {
            throw Error("relation closure exceeds row budget");
          }
        }
      }
      const std::string identity = type_identity(*expr->resolved_type);
      if (expr->text == "none") return Value(ValueVariant{identity, "none", {}});
      if (expr->text == "some" || expr->text == "ok" || expr->text == "err") {
        return Value(ValueVariant{identity, expr->text,
                                  {evaluate(expr->children.front(), environment)}});
      }
      if (expr->text.find('.') == std::string::npos) {
        return Value(ValueNewtype{expr->text,
                                  {evaluate(expr->children.front(), environment)}});
      }
      const std::size_t dot = expr->text.rfind('.');
      ValueVariant variant{expr->text.substr(0, dot), expr->text.substr(dot + 1U), {}};
      if (!expr->children.empty()) {
        variant.payload.push_back(evaluate(expr->children.front(), environment));
      }
      return Value(std::move(variant));
    }
    case Expr::Kind::Match: {
      const Value scrutinee = evaluate(expr->left, environment);
      const ValueVariant& variant = scrutinee.as_variant();
      for (const MatchArm& arm : expr->arms) {
        const std::size_t dot = arm.constructor.rfind('.');
        const std::string constructor =
            dot == std::string::npos ? arm.constructor : arm.constructor.substr(dot + 1U);
        if (!arm.wildcard && constructor != variant.constructor) continue;
        std::optional<Value> previous;
        if (!arm.binding.empty()) {
          const auto found = environment.locals.find(arm.binding);
          if (found != environment.locals.end()) previous = found->second;
          environment.locals.insert_or_assign(arm.binding, variant.payload.front());
        }
        Value result = evaluate(arm.body, environment);
        if (!arm.binding.empty()) {
          if (previous) environment.locals.insert_or_assign(arm.binding, *previous);
          else environment.locals.erase(arm.binding);
        }
        return result;
      }
      throw Error("verified match had no runtime arm");
    }
  }
  throw Error("invalid expression");
}

std::map<std::string, Value, std::less<>> initial_values(const State& state) {
  std::map<std::string, Value, std::less<>> result;
  for (const Field& field : state.fields) result.emplace(field.name, field.initial);
  return result;
}

void verify_invariants(const State& state,
                       const std::map<std::string, Value, std::less<>>& values,
                       std::uint64_t round) {
  Environment environment{values, nullptr, {}, round};
  for (std::size_t index = 0; index < state.invariants.size(); ++index) {
    const PropertyDecision decision = evaluate_instant_property(
        state.invariants[index], environment,
        PropertyUse{state.name + ".invariant[" + std::to_string(index) + "]",
                    PropertyScope::State, PropertyTrigger::Successor,
                    PropertyFailure::Reject});
    if (decision.disposition == PropertyDisposition::Reject) {
      throw Error("invariant failed in state '" + state.name + "'");
    }
    if (decision.disposition != PropertyDisposition::Admit) {
      throw Error("invalid state invariant Property disposition");
    }
  }
}

struct Fragment {
  std::vector<std::size_t> entries;
  std::vector<std::size_t> exits;
};

std::string resolve_context(const std::string& context, const Environment& environment,
                            const State& state) {
  if (context.empty()) return state.context;
  if (context == state.context) return context;
  try {
    const Value value = resolve_name(context, environment);
    return value.as_string();
  } catch (const Error&) {
    return context;
  }
}

Fragment build_plan(const std::shared_ptr<ActionExpr>& action, Environment& environment,
                    const State& state, ActionPlan& plan,
                    std::unordered_set<std::string>& labels) {
  if (action->kind == ActionExpr::Kind::Call) {
    if (!labels.insert(action->label).second) {
      throw Error("duplicate action label '" + action->label + "'");
    }
    ActionCall call;
    call.label = action->label;
    call.function = action->function;
    call.context = resolve_context(action->context, environment, state);
    for (const ExprPtr& argument : action->arguments) {
      call.arguments.push_back(evaluate(argument, environment));
    }
    const std::size_t index = plan.calls.size();
    plan.calls.push_back(std::move(call));
    return {{index}, {index}};
  }

  if (action->children.empty()) return {};
  if (action->kind == ActionExpr::Kind::Parallel) {
    Fragment result;
    for (const auto& child : action->children) {
      Fragment fragment = build_plan(child, environment, state, plan, labels);
      result.entries.insert(result.entries.end(), fragment.entries.begin(), fragment.entries.end());
      result.exits.insert(result.exits.end(), fragment.exits.begin(), fragment.exits.end());
    }
    return result;
  }

  Fragment result = build_plan(action->children.front(), environment, state, plan, labels);
  for (std::size_t index = 1; index < action->children.size(); ++index) {
    Fragment next = build_plan(action->children[index], environment, state, plan, labels);
    for (const std::size_t before : result.exits) {
      for (const std::size_t after : next.entries) {
        plan.dependencies.emplace_back(before, after);
      }
    }
    result.exits = std::move(next.exits);
  }
  return result;
}

void validate_event(const Transition& transition, const Event& event,
                    const TypeRegistry& types) {
  if (event.fields.size() != transition.parameters.size()) {
    throw Error("event '" + event.name + "' has the wrong number of fields");
  }
  for (const Parameter& parameter : transition.parameters) {
    const auto found = event.fields.find(parameter.name);
    if (found == event.fields.end()) {
      throw Error("event '" + event.name + "' is missing field '" + parameter.name + "'");
    }
    if (!value_matches_type(found->second, parameter.type, types)) {
      throw Error("event field '" + parameter.name + "' has the wrong type");
    }
  }
}

std::string escape_string(std::string_view value) {
  std::string result = "\"";
  for (char ch : value) {
    switch (ch) {
      case '\\': result += "\\\\"; break;
      case '"': result += "\\\""; break;
      case '\n': result += "\\n"; break;
      case '\r': result += "\\r"; break;
      case '\t': result += "\\t"; break;
      default: result.push_back(ch); break;
    }
  }
  result.push_back('"');
  return result;
}

}  // namespace

Value::Value(bool value) : kind_(Kind::Bool), bool_value_(value) {}
Value::Value(std::int64_t value) : kind_(Kind::Int), int_value_(value) {}
Value::Value(ExactInt value) : kind_(Kind::Int), int_value_(std::move(value)) {}
Value::Value(Rational value) : kind_(Kind::Rational), rational_value_(std::move(value)) {}
Value::Value(std::string value) : kind_(Kind::String), string_value_(std::move(value)) {}
Value::Value(const char* value) : Value(std::string(value)) {}
Value::Value(StringSet value) : kind_(Kind::StringSet), set_value_(std::move(value)) {}
Value::Value(ValueList value)
    : kind_(Kind::List), list_value_(std::make_shared<const ValueList>(std::move(value))) {}
Value::Value(ValueSet value) : kind_(Kind::Set) {
  std::sort(value.values.begin(), value.values.end(),
            [](const Value& left, const Value& right) {
              return canonical_compare(left, right) < 0;
            });
  value.values.erase(
      std::unique(value.values.begin(), value.values.end(),
                  [](const Value& left, const Value& right) {
                    return canonical_compare(left, right) == 0;
                  }),
      value.values.end());
  generic_set_value_ = std::make_shared<const ValueSet>(std::move(value));
}
Value::Value(ValueMap value) : kind_(Kind::Map) {
  std::sort(value.entries.begin(), value.entries.end(), [](const auto& left, const auto& right) {
    return canonical_compare(left.first, right.first) < 0;
  });
  const auto duplicate = std::adjacent_find(
      value.entries.begin(), value.entries.end(), [](const auto& left, const auto& right) {
        return canonical_compare(left.first, right.first) == 0;
      });
  if (duplicate != value.entries.end()) throw Error("duplicate generic map key");
  map_value_ = std::make_shared<const ValueMap>(std::move(value));
}
Value::Value(ValueBag value) : kind_(Kind::Bag) {
  std::sort(value.entries.begin(), value.entries.end(), [](const auto& left, const auto& right) {
    return canonical_compare(left.first, right.first) < 0;
  });
  ValueBag normalized;
  for (auto& [item, count] : value.entries) {
    if (count == 0) throw Error("generic bag count must be positive");
    if (!normalized.entries.empty() &&
        canonical_compare(normalized.entries.back().first, item) == 0) {
      if (count > std::numeric_limits<std::uint64_t>::max() -
                      normalized.entries.back().second) {
        throw Error("generic bag count overflow");
      }
      normalized.entries.back().second += count;
    } else {
      normalized.entries.emplace_back(std::move(item), count);
    }
  }
  bag_value_ = std::make_shared<const ValueBag>(std::move(normalized));
}
Value::Value(ValueRecord value) : kind_(Kind::Record) {
  if (value.type_id.empty()) throw Error("record type identity must not be empty");
  std::sort(value.fields.begin(), value.fields.end(),
            [](const auto& left, const auto& right) { return left.first < right.first; });
  if (std::any_of(value.fields.begin(), value.fields.end(),
                  [](const auto& field) { return field.first.empty(); })) {
    throw Error("record field name must not be empty");
  }
  if (std::adjacent_find(value.fields.begin(), value.fields.end(),
                         [](const auto& left, const auto& right) {
                           return left.first == right.first;
                         }) != value.fields.end()) {
    throw Error("duplicate record field");
  }
  record_value_ = std::make_shared<const ValueRecord>(std::move(value));
}
Value::Value(ValueVariant value) : kind_(Kind::Variant) {
  if (value.type_id.empty() || value.constructor.empty()) {
    throw Error("variant identity must not be empty");
  }
  if (value.payload.size() > 1U) throw Error("variant supports zero or one payload");
  variant_value_ = std::make_shared<const ValueVariant>(std::move(value));
}
Value::Value(ValueNewtype value) : kind_(Kind::Newtype) {
  if (value.type_id.empty()) throw Error("newtype identity must not be empty");
  if (value.payload.size() != 1U) throw Error("newtype requires exactly one payload");
  newtype_value_ = std::make_shared<const ValueNewtype>(std::move(value));
}
Value::Value(ValueName value) : kind_(Kind::Name) {
  const auto valid_atom = [](std::string_view atom) {
    if (atom.empty()) return false;
    const auto first = static_cast<unsigned char>(atom.front());
    if (!(first == '_' || (first >= 'A' && first <= 'Z') ||
          (first >= 'a' && first <= 'z'))) return false;
    return std::all_of(atom.begin() + 1, atom.end(), [](char ch) {
      const auto item = static_cast<unsigned char>(ch);
      return item == '_' || (item >= 'A' && item <= 'Z') ||
             (item >= 'a' && item <= 'z') || (item >= '0' && item <= '9');
    });
  };
  if (!valid_atom(value.type_id) || !valid_atom(value.atom)) {
    throw Error("name type and atom must be canonical identifiers");
  }
  name_value_ = std::make_shared<const ValueName>(std::move(value));
}
Value::Value(ValueTuple value) : kind_(Kind::Tuple) {
  if (value.fields.empty() || value.fields.size() > relation_arity_limit) {
    throw Error("tuple has invalid arity");
  }
  tuple_value_ = std::make_shared<const ValueTuple>(std::move(value));
}
Value::Value(ValueRelation value) : kind_(Kind::Relation) {
  if (value.arity == 0) throw Error("relation arity must be positive");
  for (const ValueTuple& row : value.rows) {
    if (row.fields.size() != value.arity) throw Error("relation row has the wrong arity");
  }
  const auto row_less = [](const ValueTuple& left, const ValueTuple& right) {
    for (std::size_t index = 0; index < left.fields.size(); ++index) {
      const int order = canonical_compare(left.fields[index], right.fields[index]);
      if (order != 0) return order < 0;
    }
    return false;
  };
  const auto row_equal = [&](const ValueTuple& left, const ValueTuple& right) {
    return !row_less(left, right) && !row_less(right, left);
  };
  std::sort(value.rows.begin(), value.rows.end(), row_less);
  value.rows.erase(std::unique(value.rows.begin(), value.rows.end(), row_equal), value.rows.end());
  if (value.arity > relation_arity_limit) throw Error("relation exceeds arity limit");
  if (value.rows.size() > relation_row_limit) throw Error("relation exceeds row budget");
  relation_value_ = std::make_shared<const ValueRelation>(std::move(value));
}

Value::Kind Value::kind() const noexcept { return kind_; }
bool Value::as_bool() const {
  if (kind_ != Kind::Bool) throw Error("expected bool value");
  return bool_value_;
}
std::int64_t Value::as_int() const {
  if (kind_ != Kind::Int) throw Error("expected int value");
  return int_value_.to_int64();
}
const ExactInt& Value::as_exact_int() const {
  if (kind_ != Kind::Int) throw Error("expected int value");
  return int_value_;
}
const Rational& Value::as_rational() const {
  if (kind_ != Kind::Rational) throw Error("expected rational value");
  return rational_value_;
}
const std::string& Value::as_string() const {
  if (kind_ != Kind::String) throw Error("expected string value");
  return string_value_;
}
const StringSet& Value::as_string_set() const {
  if (kind_ != Kind::StringSet) throw Error("expected set<string> value");
  return set_value_;
}
const ValueList& Value::as_list() const {
  if (kind_ != Kind::List) throw Error("expected list value");
  return *list_value_;
}
const ValueSet& Value::as_set() const {
  if (kind_ != Kind::Set) throw Error("expected generic set value");
  return *generic_set_value_;
}
const ValueMap& Value::as_map() const {
  if (kind_ != Kind::Map) throw Error("expected map value");
  return *map_value_;
}
const ValueBag& Value::as_bag() const {
  if (kind_ != Kind::Bag) throw Error("expected bag value");
  return *bag_value_;
}
const ValueRecord& Value::as_record() const {
  if (kind_ != Kind::Record) throw Error("expected record value");
  return *record_value_;
}
const ValueVariant& Value::as_variant() const {
  if (kind_ != Kind::Variant) throw Error("expected variant value");
  return *variant_value_;
}
const ValueNewtype& Value::as_newtype() const {
  if (kind_ != Kind::Newtype) throw Error("expected newtype value");
  return *newtype_value_;
}
const ValueName& Value::as_name() const {
  if (kind_ != Kind::Name) throw Error("expected name value");
  return *name_value_;
}
const ValueTuple& Value::as_tuple() const {
  if (kind_ != Kind::Tuple) throw Error("expected tuple value");
  return *tuple_value_;
}
const ValueRelation& Value::as_relation() const {
  if (kind_ != Kind::Relation) throw Error("expected relation value");
  return *relation_value_;
}

bool operator==(const Value& left, const Value& right) {
  if (left.kind_ != right.kind_) return false;
  switch (left.kind_) {
    case Value::Kind::Bool: return left.bool_value_ == right.bool_value_;
    case Value::Kind::Int: return left.int_value_ == right.int_value_;
    case Value::Kind::Rational: return left.rational_value_ == right.rational_value_;
    case Value::Kind::String: return left.string_value_ == right.string_value_;
    case Value::Kind::StringSet: return left.set_value_ == right.set_value_;
    case Value::Kind::List: return *left.list_value_ == *right.list_value_;
    case Value::Kind::Set: return *left.generic_set_value_ == *right.generic_set_value_;
    case Value::Kind::Map: return *left.map_value_ == *right.map_value_;
    case Value::Kind::Bag: return *left.bag_value_ == *right.bag_value_;
    case Value::Kind::Record: return *left.record_value_ == *right.record_value_;
    case Value::Kind::Variant: return *left.variant_value_ == *right.variant_value_;
    case Value::Kind::Newtype: return *left.newtype_value_ == *right.newtype_value_;
    case Value::Kind::Name: return *left.name_value_ == *right.name_value_;
    case Value::Kind::Tuple: return *left.tuple_value_ == *right.tuple_value_;
    case Value::Kind::Relation: return *left.relation_value_ == *right.relation_value_;
  }
  return false;
}

int canonical_compare(const Value& left, const Value& right) {
  return canonical_value_order(left, right);
}

Error::Error(std::string message, std::size_t line, std::size_t column)
    : std::runtime_error(std::move(message)), line_(line), column_(column) {}

Program::Program() = default;
Program::Program(std::shared_ptr<const Impl> impl) : impl_(std::move(impl)) {}
bool Program::empty() const noexcept { return !impl_; }
const std::shared_ptr<const Program::Impl>& Program::implementation() const noexcept { return impl_; }

Program parse(std::string_view source) {
  Parser parser(lex(source));
  auto implementation = parser.program();
  verify_program(*implementation);
  FunctionScope function_scope(implementation->functions);

  for (const State& state : implementation->states) {
    if (state.initial) verify_invariants(state, initial_values(state), 0);
  }
  return Program(std::move(implementation));
}

#include "runtime.cpp"
#include "solver.cpp"
std::string value_text(const Value& value) {
  switch (value.kind()) {
    case Value::Kind::Bool: return value.as_bool() ? "true" : "false";
    case Value::Kind::Int: return value.as_exact_int().text();
    case Value::Kind::Rational: return value.as_rational().text();
    case Value::Kind::String: return escape_string(value.as_string());
    case Value::Kind::StringSet: {
      std::string result = "{";
      bool first = true;
      for (const std::string& item : value.as_string_set().values) {
        if (!first) result += ", ";
        first = false;
        result += escape_string(item);
      }
      result += "}";
      return result;
    }
    case Value::Kind::List: {
      std::string result = "list[";
      for (std::size_t index = 0; index < value.as_list().values.size(); ++index) {
        if (index != 0) result += ", ";
        result += value_text(value.as_list().values[index]);
      }
      return result + "]";
    }
    case Value::Kind::Set: {
      std::string result = "{";
      for (std::size_t index = 0; index < value.as_set().values.size(); ++index) {
        if (index != 0) result += ", ";
        result += value_text(value.as_set().values[index]);
      }
      return result + "}";
    }
    case Value::Kind::Map: {
      std::string result = "{";
      for (std::size_t index = 0; index < value.as_map().entries.size(); ++index) {
        if (index != 0) result += ", ";
        result += value_text(value.as_map().entries[index].first) + ": " +
                  value_text(value.as_map().entries[index].second);
      }
      return result + "}";
    }
    case Value::Kind::Bag: {
      std::string result = "bag{";
      for (std::size_t index = 0; index < value.as_bag().entries.size(); ++index) {
        if (index != 0) result += ", ";
        result += value_text(value.as_bag().entries[index].first) + ": " +
                  std::to_string(value.as_bag().entries[index].second);
      }
      return result + "}";
    }
    case Value::Kind::Record: {
      std::string result = value.as_record().type_id + "{";
      for (std::size_t index = 0; index < value.as_record().fields.size(); ++index) {
        if (index != 0) result += ", ";
        result += value.as_record().fields[index].first + ": " +
                  value_text(value.as_record().fields[index].second);
      }
      return result + "}";
    }
    case Value::Kind::Variant: {
      if (value.as_variant().type_id.starts_with("option<")) {
        if (value.as_variant().constructor == "none") return "[]";
        return "[" + value_text(value.as_variant().payload.front()) + "]";
      }
      std::string result = value.as_variant().type_id + "." + value.as_variant().constructor;
      if (!value.as_variant().payload.empty()) {
        result += "(" + value_text(value.as_variant().payload.front()) + ")";
      }
      return result;
    }
    case Value::Kind::Newtype:
      return value.as_newtype().type_id + "(" +
             value_text(value.as_newtype().payload.front()) + ")";
    case Value::Kind::Name:
      return value.as_name().type_id + "(" + value.as_name().atom + ")";
    case Value::Kind::Tuple: {
      std::string result = "(";
      for (std::size_t index = 0; index < value.as_tuple().fields.size(); ++index) {
        if (index != 0) result += ", ";
        result += value_text(value.as_tuple().fields[index]);
      }
      return result + ")";
    }
    case Value::Kind::Relation: {
      std::string result = "relation{";
      for (std::size_t index = 0; index < value.as_relation().rows.size(); ++index) {
        if (index != 0) result += ", ";
        const ValueTuple& row = value.as_relation().rows[index];
        result += row.fields.size() == 1U ? value_text(row.fields.front())
                                         : value_text(Value(row));
      }
      return result + "}";
    }
  }
  throw Error("invalid value kind");
}

std::string result_text(const StepResult& result) {
  std::ostringstream out;
  out << "round " << result.round << '\n';
  if (!result.procedure.empty()) {
    out << "procedure " << result.procedure
        << " revision " << result.procedure_revision << '\n';
  }
  out << "decision " << result.id << '\n';
  out << "input "
      << (result.input.kind == OccurrenceInputKind::Transition
              ? "transition "
              : "event ")
      << result.input.symbol;
  if (result.input.event != result.input.symbol) {
    out << " as event " << result.input.event;
  }
  if (!result.input.target_procedure.empty()) {
    out << " @ " << result.input.target_procedure;
  }
  if (!result.input.target_context.empty()) {
    out << "/" << result.input.target_context;
  }
  out << " {";
  bool first_input = true;
  for (const auto& [name, value] : result.input.fields) {
    if (!first_input) out << ", ";
    first_input = false;
    out << name << " = " << value_text(value);
  }
  out << "}\n";
  out << "transition " << result.transition << '\n';
  if (result.optimized_score) {
    out << "optimized_score " << value_text(*result.optimized_score)
        << " @ " << result.optimization_scope << '\n';
  }
  out << "reads {";
  bool first_read = true;
  for (const std::string& field : result.reads) {
    if (!first_read) out << ", ";
    first_read = false;
    out << field;
  }
  out << "}\n";
  out << "writes {";
  bool first_write = true;
  for (const std::string& field : result.writes) {
    if (!first_write) out << ", ";
    first_write = false;
    out << field;
  }
  out << "}\n";
  out << "after {";
  bool first_predecessor = true;
  for (const std::string& predecessor : result.causal_predecessors) {
    if (!first_predecessor) out << ", ";
    first_predecessor = false;
    out << predecessor;
  }
  out << "}\n";
  out << "before " << result.from_state << " {\n";
  for (const auto& [name, value] : result.before_state) {
    out << "  " << name << " = " << value_text(value) << '\n';
  }
  out << "}\n";
  out << "state " << result.from_state << " -> " << result.to_state << " {\n";
  for (const auto& [name, value] : result.state) {
    out << "  " << name << " = " << value_text(value) << '\n';
  }
  out << "}\n";
  out << "actions {\n";
  for (std::size_t index = 0; index < result.actions.calls.size(); ++index) {
    const ActionCall& call = result.actions.calls[index];
    out << "  " << index << " " << call.label << ": $" << call.function << "(";
    for (std::size_t argument = 0; argument < call.arguments.size(); ++argument) {
      if (argument != 0) out << ", ";
      out << value_text(call.arguments[argument]);
    }
    out << ")";
    if (!call.context.empty()) out << " @ " << call.context;
    out << '\n';
  }
  out << "}\n";
  out << "requires {\n";
  for (const auto& [before, after] : result.actions.dependencies) {
    out << "  " << result.actions.calls[after].label << " <- "
        << result.actions.calls[before].label << '\n';
  }
  out << "}\n";
  return out.str();
}

#include "backend.cpp"

namespace {

SourcePosition position_at(std::string_view source, std::size_t offset) {
  if (offset > source.size()) throw Error("source position is outside the document");
  SourcePosition position{1, 1, offset};
  for (std::size_t index = 0; index < offset; ++index) {
    if (source[index] == '\n') {
      ++position.line;
      position.column = 1;
    } else {
      ++position.column;
    }
  }
  return position;
}

SourcePosition position_from_line_column(std::string_view source, std::size_t line,
                                         std::size_t column) {
  if (line == 0 || column == 0) return position_at(source, 0);
  std::size_t current_line = 1;
  std::size_t offset = 0;
  while (current_line < line && offset < source.size()) {
    if (source[offset++] == '\n') ++current_line;
  }
  if (current_line != line) return position_at(source, source.size());
  const std::size_t line_start = offset;
  while (offset < source.size() && source[offset] != '\n' &&
         offset - line_start + 1 < column) {
    ++offset;
  }
  return position_at(source, offset);
}

Diagnostic error_diagnostic(std::string_view source, const Error& error,
                            std::string code) {
  const SourcePosition start = position_from_line_column(
      source, error.line() == 0 ? 1 : error.line(), error.column() == 0 ? 1 : error.column());
  const std::size_t length =
      start.offset < source.size() && source[start.offset] != '\n' ? 1U : 0U;
  const SourcePosition end = position_at(source, start.offset + length);
  return {std::move(code), DiagnosticSeverity::Error, error.what(), {start, end}};
}

}  // namespace

LanguageAnalysis analyze_source(std::string_view source, std::string uri,
                                std::uint64_t version) {
  LanguageAnalysis result;
  result.uri = std::move(uri);
  result.version = version;
  std::vector<Token> tokens;
  try {
    tokens = lex(source, &result.highlights);
  } catch (const Error& error) {
    result.diagnostics.push_back(error_diagnostic(source, error, "DTESSL1001"));
    return result;
  }

  std::shared_ptr<Program::Impl> implementation;
  try {
    Parser parser(std::move(tokens));
    implementation = parser.program();
  } catch (const Error& error) {
    result.diagnostics.push_back(error_diagnostic(source, error, "DTESSL1002"));
    return result;
  }

  try {
    verify_program(*implementation);
    const auto initial = std::find_if(
        implementation->states.begin(), implementation->states.end(),
        [](const State& state) { return state.initial; });
    verify_invariants(*initial, initial_values(*initial), 0);
  } catch (const Error& error) {
    result.diagnostics.push_back(error_diagnostic(source, error, "DTESSL2001"));
  }
  return result;
}

}  // namespace dtessl
