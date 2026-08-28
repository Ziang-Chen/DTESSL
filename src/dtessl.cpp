#include "dtessl/dtessl.hpp"
#include "dtessl/backend.hpp"
#include "dtessl/language_service.hpp"

#include <algorithm>
#include <charconv>
#include <cctype>
#include <functional>
#include <iterator>
#include <limits>
#include <optional>
#include <sstream>
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
      "projected", "Claim", "always", "eventually", "case", "function",
      "procedure", "initial"};
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
        static constexpr std::string_view symbols = "@:$,|(){}[]<>=+-.*~/";
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

struct Field {
  enum class Merge { Reject, Equal, Union };
  std::string name;
  DataType type;
  Value initial;
  Merge merge{Merge::Reject};
  std::size_t line{0};
  std::size_t column{0};
};

struct State {
  std::string name;
  std::string context;
  bool initial{false};
  std::vector<Field> fields;
  std::vector<ExprPtr> invariants;
  std::size_t line{0};
  std::size_t column{0};
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
  std::set<std::string, std::less<>> reads;
  std::set<std::string, std::less<>> writes;
};

struct Transition {
  std::string name;
  std::string event;
  std::vector<Parameter> parameters;
  std::string case_name;
  std::vector<StateBinding> from;
  std::vector<TransitionTarget> to;
  std::vector<TransitionAlternative> alternatives;
  ExprPtr condition{make_literal(Value(true))};
  std::shared_ptr<ActionExpr> action;
  std::set<std::string, std::less<>> reads;
  std::set<std::string, std::less<>> writes;
  std::size_t line{0};
  std::size_t column{0};
};

struct TraceDeclaration {
  std::string name;
  std::string root_context;
  bool has_replay{false};
  bool has_capture{false};
  TraceCaptureMode capture_mode{TraceCaptureMode::Closed};
  std::vector<StateBinding> capture;
  std::set<std::string, std::less<>> paths;
  EventTrace events;
  std::size_t line{0};
  std::size_t column{0};
};

struct ClaimDeclaration {
  enum class Kind { Always, Eventually, CountAtMost };
  std::string name;
  std::string trace;
  Kind kind{Kind::Always};
  ExprPtr predicate;
  std::string transition;
  std::uint64_t limit{0};
  std::size_t line{0};
  std::size_t column{0};
};

struct ProcedureDeclaration {
  std::string name;
  std::string initial_context;
  std::vector<StateBinding> initial_states;
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
  const Token& peek() const {
    static const Token end{TokenKind::End, "<end>", 0, 0};
    return cursor_ < tokens_.size() ? tokens_[cursor_] : end;
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
    if (match("tuple") || match("relation")) {
      const std::string constructor = tokens_[cursor_ - 1].text;
      expect("<");
      std::vector<DataType> elements;
      do {
        elements.push_back(parse_type());
        if (elements.size() > relation_arity_limit) {
          fail(peek(), "tuple/relation type exceeds arity limit");
        }
      } while (match(","));
      expect(">");
      return DataType(constructor == "tuple" ? DataType::Kind::Tuple
                                               : DataType::Kind::Relation,
                      std::move(elements));
    }
    Token name = identifier();
    if (!types_.contains(name.text)) fail(name, "unknown nominal type '" + name.text + "'");
    return DataType(DataType::Kind::Named, std::move(name.text));
  }

  ExprPtr parse_implication() {
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
        "=", "==", "!=", "<", "<=", ">", ">=", "in", "~"};
    if (!at_end() && operators.contains(peek().text)) {
      const std::string op = take().text;
      return make_binary(op, std::move(left), parse_add());
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
      auto result = parse_implication();
      expect(")");
      return result;
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
    if (match("<")) {
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
};

class Parser {
 public:
  explicit Parser(std::vector<Token> tokens) : tokens_(std::move(tokens)) {}

  std::shared_ptr<Program::Impl> program();

 private:
  const Token& peek() const { return tokens_[cursor_]; }
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
  State state();
  Transition transition();
  TraceDeclaration trace(const std::vector<Transition>& transitions);
  ClaimDeclaration claim();
  ProcedureDeclaration procedure();
  ExprPtr line_expression();
  ExprPtr block_expression();
  std::shared_ptr<ActionExpr> block_action();
  std::vector<Token> take_block_tokens();

  std::vector<Token> tokens_;
  std::size_t cursor_{0};
  TypeRegistry types_;
};

}  // namespace

struct Program::Impl {
  TypeRegistry types;
  FunctionRegistry functions;
  std::vector<ActionPortDeclaration> action_ports;
  std::vector<State> states;
  std::vector<Transition> transitions;
  std::vector<TraceDeclaration> traces;
  std::vector<ClaimDeclaration> claims;
  std::map<std::string, ProcedureDeclaration, std::less<>> procedures;
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
  if (match("tuple") || match("relation")) {
    const std::string constructor = tokens_[cursor_ - 1].text;
    expect("<");
    std::vector<DataType> elements;
    do {
      elements.push_back(type());
      if (elements.size() > relation_arity_limit) {
        fail(peek(), "tuple/relation type exceeds arity limit");
      }
    } while (match(","));
    expect(">");
    return DataType(constructor == "tuple" ? DataType::Kind::Tuple
                                             : DataType::Kind::Relation,
                    std::move(elements));
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
    if (!match("~")) expect("relation");
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

State Parser::state() {
  const Token start = peek();
  expect("state");
  State result;
  result.line = start.line;
  result.column = start.column;
  result.name = identifier();
  if (match("@")) result.context = identifier();
  result.initial = match("initial");
  expect(":");
  newline();
  indent();
  while (!at(TokenKind::Dedent)) {
    if (match("invariant")) {
      expect(":");
      result.invariants.push_back(block_expression());
      continue;
    }
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
    newline();
    result.fields.push_back(std::move(field));
  }
  dedent();
  return result;
}

Transition Parser::transition() {
  const Token start = peek();
  expect("transition");
  Transition result;
  result.line = start.line;
  result.column = start.column;
  result.name = identifier();
  expect("@");
  result.event = identifier();
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
  expect(":");
  newline();
  indent();
  const auto binding = [&]() {
    StateBinding result;
    const Token start_token = peek();
    result.line = start_token.line;
    result.column = start_token.column;
    result.state = identifier();
    if (match("@")) result.context = identifier();
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
  const auto source_patterns = [&]() {
    std::vector<std::vector<StateBinding>> expansions(1);
    expect("(");
    if (!match(")")) {
      do {
        const Token pattern_start = peek();
        std::vector<std::string> choices;
        if (match("{")) {
          do choices.push_back(identifier()); while (match(","));
          expect("}");
        } else if (match("_")) {
          choices.push_back("_");
        } else {
          choices.push_back(identifier());
        }
        std::string context;
        if (match("@")) context = identifier();
        std::vector<std::vector<StateBinding>> next;
        for (const auto& expansion : expansions) {
          for (const std::string& choice : choices) {
            auto item = expansion;
            item.push_back(
                StateBinding{choice, context, pattern_start.line, pattern_start.column});
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
        targets.push_back(TransitionTarget{binding(), {}});
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
      while (!at(TokenKind::Dedent)) {
        if (match("where")) {
          expect(":");
          condition = block_expression();
          continue;
        }
        if (match("set")) {
          expect("@");
          const std::string context = identifier();
          expect(":");
          newline();
          indent();
          const std::vector<Assignment> updates = assignments();
          dedent();
          const auto found = std::find_if(
              targets.begin(), targets.end(), [&](const TransitionTarget& target) {
                return target.binding.context == context;
              });
          if (found == targets.end()) {
            fail(peek(), "set @" + context + " has no matching path target");
          }
          found->assignments.insert(found->assignments.end(), updates.begin(), updates.end());
          continue;
        }
        if (match("do")) {
          expect(":");
          action = block_action();
          continue;
        }
        fail(peek(), "case path requires where, set, or do");
      }
      dedent();
      for (auto& source : sources) {
        if (first_route) {
          result.from = std::move(source);
          result.to = targets;
          result.case_name = case_name;
          result.condition = condition;
          result.action = action;
          first_route = false;
        } else {
          result.alternatives.push_back(TransitionAlternative{
              case_name, std::move(source), targets, condition, action, {}, {}});
        }
      }
      continue;
    }
    if (match("set")) {
      expect("@");
      const std::string context = identifier();
      expect(":");
      newline();
      indent();
      const std::vector<Assignment> updates = assignments();
      dedent();
      auto attach = [&](std::vector<TransitionTarget>& targets) {
        const auto found = std::find_if(targets.begin(), targets.end(),
                                        [&](const TransitionTarget& target) {
                                          return target.binding.context == context;
                                        });
        if (found == targets.end()) {
          fail(peek(), "set @" + context + " has no matching target context");
        }
        found->assignments.insert(found->assignments.end(), updates.begin(), updates.end());
      };
      attach(result.to);
      for (TransitionAlternative& alternative : result.alternatives) attach(alternative.to);
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
    fail(peek(), "expected case, set, from, to, where, or do");
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
  expect("@");
  result.root_context = identifier();
  expect(":");
  newline();
  indent();
  const auto parse_replay = [&]() {
    result.has_replay = true;
    expect(":");
    newline();
    indent();
    const auto schema = [&](std::string_view event_name) -> const std::vector<Parameter>& {
      const auto found = std::find_if(
          transitions.begin(), transitions.end(), [&](const Transition& item) {
            return item.event == event_name;
          });
      if (found == transitions.end()) {
        fail(peek(), "trace references unknown event '" + std::string(event_name) + "'");
      }
      return found->parameters;
    };
    while (!at(TokenKind::Dedent)) {
      EventBatch batch;
      for (;;) {
        Event event;
        event.name = identifier();
        const auto& parameters = schema(event.name);
        expect("(");
        for (std::size_t index = 0; index < parameters.size(); ++index) {
          if (index != 0) expect(",");
          event.fields.emplace(parameters[index].name,
                               initial_value(parameters[index].type));
        }
        expect(")");
        batch.events.push_back(std::move(event));
        if (!match("|")) break;
      }
      result.events.rounds.push_back(std::move(batch));
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
          [&](const StateBinding& item) { return item.context == context; });
      if (duplicate) fail(peek(), "trace captures the same context twice");
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

ProcedureDeclaration Parser::procedure() {
  const Token start = peek();
  expect("procedure");
  ProcedureDeclaration result;
  result.line = start.line;
  result.column = start.column;
  result.name = identifier();
  expect("@");
  result.initial_context = identifier();
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
  result.trace = identifier();
  expect(":");
  newline();
  indent();
  if (match("always")) {
    result.kind = ClaimDeclaration::Kind::Always;
    expect(":");
    result.predicate = block_expression();
  } else if (match("eventually")) {
    result.kind = ClaimDeclaration::Kind::Eventually;
    expect(":");
    result.predicate = block_expression();
  } else if (match("count")) {
    result.kind = ClaimDeclaration::Kind::CountAtMost;
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
    fail(peek(), "Claim requires always:, eventually:, or count Transition <= N");
  }
  dedent();
  return result;
}

std::shared_ptr<Program::Impl> Parser::program() {
  auto result = std::make_shared<Program::Impl>();
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
      result->states.push_back(state());
    } else if (at("transition")) {
      result->transitions.push_back(transition());
    } else if (at("procedure")) {
      ProcedureDeclaration declaration = procedure();
      if (!result->procedures.emplace(declaration.name, std::move(declaration)).second) {
        fail(peek(), "duplicate procedure declaration");
      }
    } else if (at("trace")) {
      result->traces.push_back(trace(result->transitions));
    } else if (at("Claim")) {
      result->claims.push_back(claim());
    } else {
      fail(peek(), "expected type, name, port, function, state, transition, procedure, trace, or Claim declaration");
    }
  }
  result->types = types_;
  return result;
}

const State& find_state(const Program::Impl& program, std::string_view name) {
  const auto found = std::find_if(program.states.begin(), program.states.end(),
                                  [&](const State& state) { return state.name == name; });
  if (found == program.states.end()) {
    throw Error("unknown state '" + std::string(name) + "'");
  }
  return *found;
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
      if (expr->text == "in" || expr->text == "~") {
        const bool set_member = right.kind == DataType::Kind::Set && left == *right.first;
        const bool relation_row = right.kind == DataType::Kind::Relation &&
            (right.direct_relation_row
                 ? left == right.elements.front()
                 : left == DataType(DataType::Kind::Tuple, right.elements));
        expr->direct_relation_binding = right.kind == DataType::Kind::Relation &&
                                        right.direct_relation_row;
        if (!set_member && !relation_row) {
          throw Error("membership item type does not match finite domain row type");
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
      const bool mixed_numeric =
          (left.kind == DataType::Kind::Int || left.kind == DataType::Kind::Rational) &&
          (right.kind == DataType::Kind::Int || right.kind == DataType::Kind::Rational);
      if (left != right && !mixed_numeric) throw Error("comparison operands have different types");
      if ((expr->text == "<" || expr->text == "<=" || expr->text == ">" ||
           expr->text == ">=") &&
          left.kind != DataType::Kind::Int && left.kind != DataType::Kind::Rational &&
          left.kind != DataType::Kind::String) {
        throw Error("ordered comparison needs exact numeric or string operands");
      }
      return bool_type();
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
  for (auto& [name, procedure] : program.procedures) {
    static_cast<void>(name);
    std::set<std::string, std::less<>> contexts;
    for (StateBinding& binding : procedure.initial_states) {
      const State& state = find_state(program, binding.state);
      if (binding.context.empty()) binding.context = state.context;
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
                                               {}, {}});
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
      std::vector<std::vector<StateBinding>> sources(1);
      for (const StateBinding& binding : raw.from) {
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
        expanded_routes.push_back(TransitionAlternative{raw.name, std::move(source), raw.to,
                                                        raw.condition, raw.action,
                                                        {}, {}});
      }
    }
    transition.from = std::move(expanded_routes.front().from);
    transition.to = std::move(expanded_routes.front().to);
    transition.case_name = std::move(expanded_routes.front().name);
    transition.condition = std::move(expanded_routes.front().condition);
    transition.action = std::move(expanded_routes.front().action);
    transition.alternatives.assign(
        std::make_move_iterator(expanded_routes.begin() + 1),
        std::make_move_iterator(expanded_routes.end()));
    struct RouteRef {
      std::vector<StateBinding>* sources;
      std::vector<TransitionTarget>* targets;
      ExprPtr* condition;
      std::shared_ptr<ActionExpr>* action;
      std::set<std::string, std::less<>>* reads;
      std::set<std::string, std::less<>>* writes;
    };
    std::vector<RouteRef> routes;
    routes.push_back(RouteRef{&transition.from, &transition.to,
                              &transition.condition, &transition.action,
                              &transition.reads, &transition.writes});
    for (TransitionAlternative& alternative : transition.alternatives) {
      routes.push_back(RouteRef{&alternative.from, &alternative.to,
                                &alternative.condition, &alternative.action,
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
    for (const RouteRef& route : routes) {
      const TypeEnvironment local_types = route_state_types(route);
      if (infer_type(*route.condition, local_types, event_types, {}, program.types).kind !=
          DataType::Kind::Bool) {
        throw Error("where clause in transition '" + transition.name + "' must be bool",
                    (*route.condition)->line, (*route.condition)->column);
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
      if (!qualified_paths.contains(path)) {
        throw Error("trace captures unknown transition path '" + path + "'",
                    trace.line, trace.column);
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
  for (ClaimDeclaration& claim : program.claims) {
    if (!names.insert(claim.name).second) {
      throw Error("duplicate Claim '" + claim.name + "'", claim.line, claim.column);
    }
    if (std::none_of(program.traces.begin(), program.traces.end(),
                     [&](const TraceDeclaration& trace) { return trace.name == claim.trace; })) {
      throw Error("Claim references unknown trace '" + claim.trace + "'",
                  claim.line, claim.column);
    }
    if (claim.kind == ClaimDeclaration::Kind::CountAtMost) {
      const bool base_transition = std::any_of(
          program.transitions.begin(), program.transitions.end(),
          [&](const Transition& transition) { return transition.name == claim.transition; });
      if (!base_transition && !qualified_paths.contains(claim.transition)) {
        throw Error("Claim references unknown transition '" + claim.transition + "'",
                    claim.line, claim.column);
      }
    } else if (infer_type(claim.predicate, claim_types, {}, {}, program.types).kind !=
               DataType::Kind::Bool) {
      throw Error("Claim predicate must be bool", claim.line, claim.column);
    }
  }
}

struct Environment {
  const std::map<std::string, Value, std::less<>>& state;
  const Event* event{nullptr};
  std::map<std::string, Value, std::less<>> locals;
  std::uint64_t round{0};
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
    cursor = 0;
    std::size_t matched = 0;
    for (const auto& [key, item] : environment.state) {
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
      if (expr->text == "=" || expr->text == "==") return Value(equal_values(left, right));
      if (expr->text == "!=") return Value(!equal_values(left, right));
      if (expr->text == "in" || expr->text == "~") {
        if (right.kind() == Value::Kind::StringSet) {
          return Value(right.as_string_set().values.contains(left.as_string()));
        }
        if (right.kind() == Value::Kind::Relation) {
          const auto& rows = right.as_relation().rows;
          const ValueTuple target = expr->direct_relation_binding
                                        ? ValueTuple{{left}}
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
      const int order = compare_values(left, right);
      if (expr->text == "<") return Value(order < 0);
      if (expr->text == "<=") return Value(order <= 0);
      if (expr->text == ">") return Value(order > 0);
      if (expr->text == ">=") return Value(order >= 0);
      throw Error("unknown binary operator '" + expr->text + "'");
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
  for (const ExprPtr& invariant : state.invariants) {
    if (!evaluate(invariant, environment).as_bool()) {
      throw Error("invariant failed in state '" + state.name + "'");
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

Engine::Engine(Program program) : Engine(std::move(program), {}) {}

Engine::Engine(Program program,
               std::map<std::string, std::string, std::less<>> initial_states)
    : program_(std::move(program)) {
  if (program_.empty()) throw Error("cannot construct an engine from an empty program");
  const Program::Impl& implementation = *program_.implementation();
  FunctionScope function_scope(implementation.functions);
  const std::size_t initial_count = static_cast<std::size_t>(std::count_if(
      implementation.states.begin(), implementation.states.end(),
      [](const State& state) { return state.initial; }));
  for (const State& state : implementation.states) {
    if (!state.initial) continue;
    active_states_.emplace(state.context, state.name);
    for (const auto& [field, value] : initial_values(state)) {
      values_.emplace(initial_count == 1U ? field : state_key(state.context, field), value);
    }
  }
  for (const auto& [context, state_name] : initial_states) {
    const State& state = find_state(implementation, state_name);
    if (state.context != context) {
      throw Error("initial override '" + state_name + "' does not belong to @" + context);
    }
    const State& previous = find_state(implementation, active_states_.at(context));
    for (const Field& field : previous.fields) {
      values_.erase(initial_count == 1U ? field.name : state_key(context, field.name));
    }
    active_states_.insert_or_assign(context, state_name);
    for (const auto& [field, value] : initial_values(state)) {
      values_.insert_or_assign(initial_count == 1U ? field : state_key(context, field), value);
    }
    verify_invariants(state, initial_values(state), 0);
  }
  for (const TraceDeclaration& trace : implementation.traces) {
    if (!trace.has_capture) continue;
    TraceSnapshot snapshot;
    snapshot.name = trace.name;
    snapshot.root_context = trace.root_context;
    snapshot.mode = trace.capture_mode;
    for (const StateBinding& binding : trace.capture) {
      snapshot.captured_contexts.insert(binding.context);
    }
    snapshot.captured_paths = trace.paths;
    if (trace.capture_mode == TraceCaptureMode::Projected) {
      snapshot.causal_gaps.push_back(
          "projected capture omits decisions outside selected @ contexts");
    }
    captured_traces_.emplace(trace.name, std::move(snapshot));
  }
}

Engine Engine::from_procedure(Program program, std::string_view procedure_name) {
  if (program.empty()) throw Error("cannot start a procedure from an empty program");
  const auto& procedures = program.implementation()->procedures;
  const auto found = procedures.find(procedure_name);
  if (found == procedures.end()) {
    throw Error("unknown procedure '" + std::string(procedure_name) + "'");
  }
  std::map<std::string, std::string, std::less<>> initial_states;
  for (const StateBinding& binding : found->second.initial_states) {
    initial_states.emplace(binding.context, binding.state);
  }
  const std::string initial_context = found->second.initial_context;
  Engine result(std::move(program), std::move(initial_states));
  result.initial_context_ = initial_context;
  return result;
}

ParallelStepResult Engine::step_parallel(const std::vector<Event>& events) {
  if (events.empty()) throw Error("a parallel step needs at least one event");
  if (round_ == std::numeric_limits<std::uint64_t>::max()) {
    throw Error("simulation round overflow");
  }
  const Program::Impl& program = *program_.implementation();
  FunctionScope function_scope(program.functions);
  struct Prepared {
    const Transition* transition;
    const std::string* case_name;
    const std::vector<StateBinding>* from;
    const std::vector<TransitionTarget>* to;
    const std::set<std::string, std::less<>>* read_set;
    const std::set<std::string, std::less<>>* write_set;
    std::map<std::string, Value, std::less<>> writes;
    std::map<std::string, std::string, std::less<>> targets;
    ActionPlan actions;
    std::set<std::string, std::less<>> causal_predecessors;
  };
  std::vector<Prepared> prepared;
  prepared.reserve(events.size());

  std::vector<const Event*> ordered_events;
  ordered_events.reserve(events.size());
  for (const Event& event : events) ordered_events.push_back(&event);
  std::stable_sort(ordered_events.begin(), ordered_events.end(),
                   [](const Event* left, const Event* right) { return event_less(*left, *right); });

  for (const Event* event_pointer : ordered_events) {
    const Event& event = *event_pointer;
    struct Enabled {
      const Transition* transition;
      const std::string* case_name;
      const std::vector<StateBinding>* from;
      const std::vector<TransitionTarget>* to;
      const ExprPtr* condition;
      const std::shared_ptr<ActionExpr>* action;
      const std::set<std::string, std::less<>>* reads;
      const std::set<std::string, std::less<>>* writes;
    };
    std::vector<Enabled> enabled;
    for (const Transition& transition : program.transitions) {
      if (transition.event != event.name) continue;
      struct Route {
        const std::vector<StateBinding>* from;
        const std::string* case_name;
        const std::vector<TransitionTarget>* to;
        const ExprPtr* condition;
        const std::shared_ptr<ActionExpr>* action;
        const std::set<std::string, std::less<>>* reads;
        const std::set<std::string, std::less<>>* writes;
      };
      std::vector<Route> routes;
      routes.push_back(Route{&transition.from, &transition.case_name, &transition.to,
                             &transition.condition, &transition.action,
                             &transition.reads, &transition.writes});
      for (const TransitionAlternative& alternative : transition.alternatives) {
        routes.push_back(Route{&alternative.from, &alternative.name, &alternative.to,
                               &alternative.condition, &alternative.action,
                               &alternative.reads, &alternative.writes});
      }
      for (const Route& route : routes) {
       const bool sources_active = std::all_of(
          route.from->begin(), route.from->end(), [&](const StateBinding& source) {
            const auto active = active_states_.find(source.context);
            return active != active_states_.end() && active->second == source.state;
          });
       if (!sources_active) continue;
       validate_event(transition, event, program.types);
       Environment environment{values_, &event, {}, round_};
       if (evaluate(*route.condition, environment).as_bool()) {
         enabled.push_back(Enabled{&transition, route.case_name, route.from, route.to,
                                   route.condition, route.action,
                                   route.reads, route.writes});
       }
      }
    }
    if (enabled.empty()) {
      throw Error("no transition accepts event '" + event.name + "' from active state set " +
                  current_state());
    }
    if (enabled.size() != 1) {
      throw Error("event '" + event.name +
                  "' enables multiple transitions from active state set " + current_state());
    }

    const Enabled selected = enabled.front();
    const Transition& transition = *selected.transition;
    const State& source = find_state(program, selected.from->front().state);
    Environment environment{values_, &event, {}, round_};
    Prepared decision{&transition, selected.case_name, selected.from, selected.to,
                      selected.reads, selected.writes, {}, {}, {}, {}};
    for (const std::string& field : *selected.reads) {
      const auto writers = last_writers_.find(field);
      if (writers != last_writers_.end()) {
        decision.causal_predecessors.insert(writers->second.begin(), writers->second.end());
      }
    }
    for (const TransitionTarget& target : *selected.to) {
      const State& target_state = find_state(program, target.binding.state);
      const bool legacy_single = selected.from->size() == 1U && selected.to->size() == 1U;
      decision.targets.emplace(target.binding.context, target.binding.state);
      const auto active = active_states_.find(target.binding.context);
      if (active == active_states_.end() || active->second != target.binding.state) {
        for (const auto& [field, value] : initial_values(target_state)) {
          decision.writes.insert_or_assign(
              legacy_single ? field : state_key(target.binding.context, field), value);
        }
      }
      for (const Assignment& assignment : target.assignments) {
        Value value = evaluate(assignment.value, environment);
        const Field& field = find_field(target_state, assignment.field);
        if (!value_matches_type(value, field.type, program.types)) {
          throw Error("assignment to '" + assignment.field + "' has the wrong type");
        }
        decision.writes.insert_or_assign(
            legacy_single ? assignment.field
                          : state_key(target.binding.context, assignment.field),
            std::move(value));
      }
    }
    if (*selected.action) {
      std::unordered_set<std::string> labels;
      build_plan(*selected.action, environment, source, decision.actions, labels);
      std::sort(decision.actions.dependencies.begin(), decision.actions.dependencies.end());
      decision.actions.dependencies.erase(
          std::unique(decision.actions.dependencies.begin(), decision.actions.dependencies.end()),
          decision.actions.dependencies.end());
    }
    prepared.push_back(std::move(decision));
  }

  std::map<std::string, std::string, std::less<>> next_active = active_states_;
  for (const Prepared& decision : prepared) {
    for (const auto& [context, target] : decision.targets) {
      const auto [found, inserted] = next_active.insert_or_assign(context, target);
      static_cast<void>(found);
      static_cast<void>(inserted);
      for (const Prepared& other : prepared) {
        const auto conflicting = other.targets.find(context);
        if (conflicting != other.targets.end() && conflicting->second != target) {
          throw Error("parallel transitions choose different states for @" + context);
        }
      }
    }
  }
  std::map<std::string, Value, std::less<>> next = values_;
  for (const auto& [context, target_state] : next_active) {
    const auto previous = active_states_.find(context);
    if (previous == active_states_.end() || previous->second == target_state) continue;
    const State& old_state = find_state(program, previous->second);
    for (const Field& field : old_state.fields) {
      next.erase(active_states_.size() == 1U ? field.name : state_key(context, field.name));
    }
  }
  std::map<std::string, std::vector<Value>, std::less<>> candidates;
  for (const Prepared& decision : prepared) {
    for (const auto& [field, value] : decision.writes) {
      candidates[field].push_back(value);
    }
  }
  for (const auto& [name, values] : candidates) {
    if (values.size() == 1) {
      next.insert_or_assign(name, values.front());
      continue;
    }
    const std::size_t dot = name.find('.');
    const std::string context =
        dot == std::string::npos && next_active.size() == 1U
            ? next_active.begin()->first
            : (dot == std::string::npos ? "" : name.substr(0, dot));
    const std::string field_name = dot == std::string::npos ? name : name.substr(dot + 1U);
    const Field& field = find_field(find_state(program, next_active.at(context)), field_name);
    if (field.merge == Field::Merge::Reject) {
      throw Error("parallel transitions write the same field '" + name + "'");
    }
    if (field.merge == Field::Merge::Equal) {
      if (!std::all_of(values.begin() + 1, values.end(),
                       [&](const Value& value) { return value == values.front(); })) {
        throw Error("equal merge disagrees for field '" + name + "'");
      }
      next.insert_or_assign(name, values.front());
      continue;
    }
    if (next.at(name).kind() == Value::Kind::StringSet) {
      StringSet merged = next.at(name).as_string_set();
      for (const Value& value : values) {
        const StringSet& candidate = value.as_string_set();
        merged.values.insert(candidate.values.begin(), candidate.values.end());
      }
      next.insert_or_assign(name, Value(std::move(merged)));
      continue;
    }
    ValueSet merged = next.at(name).as_set();
    for (const Value& value : values) {
      const auto& candidate = value.as_set().values;
      merged.values.insert(merged.values.end(), candidate.begin(), candidate.end());
    }
    next.insert_or_assign(name, Value(std::move(merged)));
  }
  for (const auto& [context, state_name] : next_active) {
    const State& state = find_state(program, state_name);
    verify_invariants(state, local_state_values(next, context, state), round_ + 1U);
  }

  ParallelStepResult result;
  result.round = round_ + 1U;
  result.state = next;
  result.transitions.reserve(prepared.size());
  for (std::size_t index = 0; index < prepared.size(); ++index) {
    Prepared& decision = prepared[index];
    StepResult step_result;
    step_result.round = result.round;
    step_result.id = "r" + std::to_string(result.round) + ":" + std::to_string(index);
    step_result.case_name = *decision.case_name;
    step_result.transition = decision.transition->name;
    if (!step_result.case_name.empty()) step_result.transition += "." + step_result.case_name;
    step_result.from_state = binding_set_text(*decision.from);
    step_result.to_state = target_set_text(*decision.to);
    step_result.active_states = next_active;
    step_result.state = next;
    step_result.actions = std::move(decision.actions);
    step_result.reads = *decision.read_set;
    step_result.writes = *decision.write_set;
    step_result.causal_predecessors = std::move(decision.causal_predecessors);
    result.transitions.push_back(std::move(step_result));
  }

  for (const auto& [field, values] : candidates) {
    static_cast<void>(values);
    last_writers_[field].clear();
  }
  for (const StepResult& decision : result.transitions) {
    for (const std::string& field : decision.writes) {
      last_writers_[field].insert(decision.id);
    }
  }

  ++round_;
  active_states_ = std::move(next_active);
  values_ = std::move(next);

  for (auto& [name, trace] : captured_traces_) {
    static_cast<void>(name);
    auto belongs = [&](std::string_view field) {
      if (trace.captured_contexts.empty()) return true;
      return std::any_of(trace.captured_contexts.begin(), trace.captured_contexts.end(),
                         [&](const std::string& context) {
                           return context.empty() || field.starts_with(context + ".") ||
                                  (active_states_.size() == 1U &&
                                   active_states_.begin()->first == context &&
                                   field.find('.') == std::string_view::npos);
                         });
    };
    const auto path_selected = [&](const StepResult& step_result) {
      return trace.captured_paths.empty() ||
             trace.captured_paths.contains(step_result.transition);
    };
    if (trace.mode == TraceCaptureMode::Projected && !trace.captured_paths.empty() &&
        std::none_of(result.transitions.begin(), result.transitions.end(), path_selected)) {
      continue;
    }
    ParallelStepResult captured = result;
    if (trace.mode == TraceCaptureMode::Projected) {
      captured.transitions.erase(
          std::remove_if(captured.transitions.begin(), captured.transitions.end(),
                         [&](const StepResult& step_result) {
                           const bool touches_context = trace.captured_contexts.empty() ||
                               std::any_of(step_result.reads.begin(), step_result.reads.end(), belongs) ||
                               std::any_of(step_result.writes.begin(), step_result.writes.end(), belongs);
                           return !path_selected(step_result) || !touches_context;
                         }),
          captured.transitions.end());
      if (captured.transitions.empty()) continue;
    }
    for (auto it = captured.state.begin(); it != captured.state.end();) {
      if (!belongs(it->first)) it = captured.state.erase(it);
      else ++it;
    }
    for (StepResult& step_result : captured.transitions) {
      for (auto it = step_result.state.begin(); it != step_result.state.end();) {
        if (!belongs(it->first)) it = step_result.state.erase(it);
        else ++it;
      }
    }
    trace.rounds.push_back(std::move(captured));
    trace.final_state = trace.rounds.back().state;
  }
  return result;
}

StepResult Engine::step(const Event& event) {
  ParallelStepResult result = step_parallel(std::vector<Event>{event});
  return std::move(result.transitions.front());
}

std::string Engine::current_state() const {
  if (active_states_.size() == 1U && active_states_.begin()->first.empty()) {
    return active_states_.begin()->second;
  }
  std::string result = "{";
  bool first = true;
  for (const auto& [context, state] : active_states_) {
    if (!first) result += ", ";
    first = false;
    result += state;
    if (!context.empty()) result += "@" + context;
  }
  return result + "}";
}
const std::map<std::string, std::string, std::less<>>& Engine::current_states() const noexcept {
  return active_states_;
}

const std::string& Engine::initial_context() const noexcept { return initial_context_; }
std::uint64_t Engine::current_round() const noexcept { return round_; }
const std::map<std::string, Value, std::less<>>& Engine::values() const { return values_; }

TraceResult run_trace(const Program& program, const EventTrace& trace) {
  if (trace.rounds.size() > event_trace_round_limit) {
    throw Error("DTESSL EventTrace exceeds round limit");
  }
  Engine engine(program);
  TraceResult result;
  result.rounds.reserve(trace.rounds.size());
  for (const EventBatch& batch : trace.rounds) {
    if (batch.events.empty()) throw Error("DTESSL EventTrace contains an empty event batch");
    if (batch.events.size() > event_batch_size_limit) {
      throw Error("DTESSL EventTrace batch exceeds event limit");
    }
    result.rounds.push_back(engine.step_parallel(batch.events));
  }
  result.final_state_name = engine.current_state();
  result.final_state = engine.values();
  return result;
}

TraceResult replay_trace(const Program& program, const EventTrace& trace) {
  const TraceResult first = run_trace(program, trace);
  const TraceResult second = run_trace(program, trace);
  if (first != second) throw Error("DTESSL EventTrace replay diverged");
  return first;
}

std::vector<std::string> declared_traces(const Program& program) {
  if (program.empty()) throw Error("cannot inspect traces of an empty program");
  std::vector<std::string> result;
  for (const TraceDeclaration& trace : program.implementation()->traces) {
    result.push_back(trace.name);
  }
  return result;
}

std::vector<std::string> declared_procedures(const Program& program) {
  if (program.empty()) throw Error("cannot inspect procedures of an empty program");
  std::vector<std::string> result;
  for (const auto& [name, procedure] : program.implementation()->procedures) {
    static_cast<void>(procedure);
    result.push_back(name);
  }
  return result;
}

TraceSnapshot run_named_trace(const Program& program, std::string_view name) {
  if (program.empty()) throw Error("cannot run a trace from an empty program");
  const auto& traces = program.implementation()->traces;
  const auto found = std::find_if(traces.begin(), traces.end(),
                                  [&](const TraceDeclaration& trace) {
                                    return trace.name == name;
                                  });
  if (found == traces.end()) throw Error("unknown trace '" + std::string(name) + "'");
  if (!found->has_replay) {
    throw Error("trace '" + std::string(name) +
                "' has no replay section; inspect its native Engine capture instead");
  }
  struct NamedReplayExecution {
    TraceResult logical;
    TraceSnapshot snapshot;
  };
  const auto execute = [&]() {
    if (found->events.rounds.size() > event_trace_round_limit) {
      throw Error("DTESSL replay exceeds round limit");
    }
    Engine engine(program);
    NamedReplayExecution execution;
    for (const EventBatch& batch : found->events.rounds) {
      if (batch.events.empty()) throw Error("DTESSL replay contains an empty event batch");
      if (batch.events.size() > event_batch_size_limit) {
        throw Error("DTESSL replay batch exceeds event limit");
      }
      execution.logical.rounds.push_back(engine.step_parallel(batch.events));
    }
    execution.logical.final_state_name = engine.current_state();
    execution.logical.final_state = engine.values();
    if (found->has_capture) {
      execution.snapshot = engine.captured_trace(found->name, true);
      return execution;
    }
    execution.snapshot.name = found->name;
    execution.snapshot.root_context = found->root_context;
    execution.snapshot.mode = TraceCaptureMode::Static;
    execution.snapshot.closed = true;
    execution.snapshot.rounds = execution.logical.rounds;
    execution.snapshot.final_state = execution.logical.final_state;
    if (!execution.logical.rounds.empty()) {
      for (const auto& [context, state] :
           execution.logical.rounds.back().transitions.front().active_states) {
        static_cast<void>(state);
        execution.snapshot.captured_contexts.insert(context);
      }
    }
    return execution;
  };
  const NamedReplayExecution execution = execute();
  const NamedReplayExecution replayed = execute();
  if (execution.logical != replayed.logical || execution.snapshot != replayed.snapshot) {
    throw Error("named replay diverged");
  }
  return execution.snapshot;
}

namespace {

std::vector<ClaimEvaluation> evaluate_trace_claims(
    const Program::Impl& program, const TraceSnapshot& trace) {
  FunctionScope function_scope(program.functions);
  std::vector<ClaimEvaluation> results;
  for (const ClaimDeclaration& claim : program.claims) {
    if (claim.trace != trace.name) continue;
    ClaimEvaluation result;
    result.name = claim.name;
    result.trace = trace.name;
    if (claim.kind == ClaimDeclaration::Kind::CountAtMost) {
      std::uint64_t count = 0;
      for (const ParallelStepResult& round : trace.rounds) {
        count += static_cast<std::uint64_t>(std::count_if(
            round.transitions.begin(), round.transitions.end(),
            [&](const StepResult& step) {
              return step.transition == claim.transition ||
                     (claim.transition.find('.') == std::string::npos &&
                      step.transition.starts_with(claim.transition + "."));
            }));
        if (count > claim.limit) {
          result.status = ClaimStatus::Violated;
          result.witness_round = round.round;
          result.detail = "transition count exceeded " + std::to_string(claim.limit);
          break;
        }
      }
      if (result.status != ClaimStatus::Violated) {
        result.status = trace.closed ? ClaimStatus::Satisfied : ClaimStatus::Pending;
        result.detail = trace.closed ? "closed trace stayed within count bound"
                                     : "open trace may still exceed count bound";
      }
    } else {
      bool witness = false;
      for (const ParallelStepResult& round : trace.rounds) {
        Environment environment{round.state, nullptr, {}, round.round};
        const bool value = evaluate(claim.predicate, environment).as_bool();
        const bool decisive = claim.kind == ClaimDeclaration::Kind::Always ? !value : value;
        if (!decisive) continue;
        witness = true;
        result.witness_round = round.round;
        result.status = claim.kind == ClaimDeclaration::Kind::Always
                            ? ClaimStatus::Violated
                            : ClaimStatus::Satisfied;
        result.detail = claim.kind == ClaimDeclaration::Kind::Always
                            ? "predicate is false"
                            : "predicate became true";
        break;
      }
      if (!witness) {
        if (!trace.closed) {
          result.status = ClaimStatus::Pending;
          result.detail = "open trace has no decisive witness yet";
        } else if (claim.kind == ClaimDeclaration::Kind::Always) {
          result.status = ClaimStatus::Satisfied;
          result.detail = "predicate holds throughout the closed trace";
        } else {
          result.status = ClaimStatus::Violated;
          result.detail = "closed trace ended without a witness";
        }
      }
    }
    if (!trace.causal_gaps.empty() && result.status == ClaimStatus::Satisfied) {
      result.status = ClaimStatus::Pending;
      result.detail = "projected trace has causal gaps; satisfaction is not conclusive";
    }
    results.push_back(std::move(result));
  }
  return results;
}

}  // namespace

TraceSnapshot Engine::captured_trace(std::string_view name, bool close) const {
  const auto found = captured_traces_.find(name);
  if (found == captured_traces_.end()) {
    throw Error("unknown dynamic trace '" + std::string(name) + "'");
  }
  TraceSnapshot result = found->second;
  if (close) result.closed = true;
  return result;
}

std::vector<ClaimEvaluation> Engine::evaluate_claims(
    std::string_view trace_name, bool close) const {
  return evaluate_trace_claims(*program_.implementation(), captured_trace(trace_name, close));
}

std::vector<ClaimEvaluation> evaluate_named_trace(
    const Program& program, std::string_view name) {
  return evaluate_trace_claims(*program.implementation(), run_named_trace(program, name));
}

std::string_view claim_status_name(ClaimStatus status) noexcept {
  switch (status) {
    case ClaimStatus::Satisfied: return "satisfied";
    case ClaimStatus::Violated: return "violated";
    case ClaimStatus::Pending: return "pending";
  }
  return "unknown";
}

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
      std::string result = "~{";
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
  out << "decision " << result.id << '\n';
  out << "transition " << result.transition << '\n';
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

namespace {

void collect_features(const ExprPtr& expr, FeatureSet& features) {
  if (!expr) return;
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
          expr->text == "difference") {
        features.insert(LanguageFeature::RelationAlgebra);
      } else {
        features.insert(LanguageFeature::AlgebraicDataTypes);
        features.insert(LanguageFeature::NominalTypes);
      }
      break;
    case Expr::Kind::Match:
      features.insert(LanguageFeature::AlgebraicDataTypes);
      features.insert(LanguageFeature::ExhaustiveMatch);
      break;
    case Expr::Kind::Literal:
    case Expr::Kind::Name:
    case Expr::Kind::Unary:
    case Expr::Kind::Binary:
    case Expr::Kind::Count: break;
  }
  collect_features(expr->left, features);
  collect_features(expr->right, features);
  collect_features(expr->third, features);
  for (const ExprPtr& child : expr->children) collect_features(child, features);
  for (const MatchArm& arm : expr->arms) collect_features(arm.body, features);
}

void collect_action_features(const std::shared_ptr<ActionExpr>& action,
                             FeatureSet& features) {
  if (!action) return;
  features.insert(LanguageFeature::ActionDag);
  for (const ExprPtr& argument : action->arguments) collect_features(argument, features);
  for (const auto& child : action->children) collect_action_features(child, features);
}

void collect_type_features(const DataType& type, FeatureSet& features) {
  if (type.kind == DataType::Kind::List || type.kind == DataType::Kind::Set ||
      type.kind == DataType::Kind::Map || type.kind == DataType::Kind::Bag) {
    features.insert(LanguageFeature::FiniteCollections);
  }
  if (type.kind == DataType::Kind::Tuple || type.kind == DataType::Kind::Relation) {
    features.insert(LanguageFeature::RelationAlgebra);
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
              expr->text == "intersection" || expr->text == "difference")) {
    plans.push_back({expr->text, relation_row_limit, relation_work_limit, true, false});
  }
  collect_search_plans(expr->left, plans);
  collect_search_plans(expr->right, plans);
  collect_search_plans(expr->third, plans);
  for (const ExprPtr& child : expr->children) collect_search_plans(child, plans);
  for (const MatchArm& arm : expr->arms) collect_search_plans(arm.body, plans);
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
  for (const Transition& transition : implementation.transitions) {
    collect_features(transition.condition, result);
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
  if (!implementation.claims.empty()) result.insert(LanguageFeature::TraceClaims);
  if (!implementation.functions.empty()) result.insert(LanguageFeature::PureFunctions);
  if (!implementation.procedures.empty()) result.insert(LanguageFeature::ProcedureEntry);
  return result;
}

std::vector<SearchPlanSummary> search_plans(const Program& program) {
  if (program.empty()) throw Error("cannot inspect an empty program");
  std::vector<SearchPlanSummary> plans;
  const Program::Impl& implementation = *program.implementation();
  for (const State& state : implementation.states) {
    for (const ExprPtr& invariant : state.invariants) collect_search_plans(invariant, plans);
  }
  for (const auto& [name, function] : implementation.functions) {
    static_cast<void>(name);
    collect_search_plans(function.body, plans);
  }
  for (const Transition& transition : implementation.transitions) {
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
