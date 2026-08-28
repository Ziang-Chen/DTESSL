#include "dtessl/dtessl.hpp"
#include "dtessl/backend.hpp"

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
};

[[noreturn]] void fail(const Token& token, const std::string& message) {
  throw Error(message, token.line, token.column);
}

std::vector<Token> lex(std::string_view source) {
  std::vector<Token> result;
  std::vector<std::size_t> indents{0};
  std::size_t offset = 0;
  std::size_t line_number = 1;

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
    const bool blank = first == line.size() || line.substr(first, 2) == "//";
    if (!blank) {
      if (indent > indents.back()) {
        indents.push_back(indent);
        result.push_back({TokenKind::Indent, "<indent>", line_number, 1});
      } else {
        while (indent < indents.back()) {
          indents.pop_back();
          result.push_back({TokenKind::Dedent, "<dedent>", line_number, 1});
        }
        if (indent != indents.back()) {
          throw Error("indentation does not match an outer block", line_number, 1);
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
          result.push_back({TokenKind::Identifier, std::string(line.substr(begin, cursor - begin)),
                            line_number, column});
          continue;
        }
        if (std::isdigit(ch) != 0) {
          const std::size_t begin = cursor++;
          while (cursor < line.size() &&
                 std::isdigit(static_cast<unsigned char>(line[cursor])) != 0) {
            ++cursor;
          }
          result.push_back({TokenKind::Integer, std::string(line.substr(begin, cursor - begin)),
                            line_number, column});
          continue;
        }
        if (ch == '"') {
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
          result.push_back({TokenKind::String, std::move(value), line_number, column});
          continue;
        }

        const std::string_view two = line.substr(cursor, 2);
        if (two == "<=" || two == ">=" || two == "!=" || two == "==") {
          result.push_back({TokenKind::Symbol, std::string(two), line_number, column});
          cursor += 2;
          continue;
        }
        static constexpr std::string_view symbols = "@:$,|(){}<>=+-.";
        if (symbols.find(static_cast<char>(ch)) != std::string_view::npos) {
          result.push_back({TokenKind::Symbol, std::string(1, static_cast<char>(ch)), line_number,
                            column});
          ++cursor;
          continue;
        }
        throw Error("unexpected character", line_number, column);
      }
      result.push_back({TokenKind::Newline, "<newline>", line_number, line.size() + 1});
    }

    if (line_end == std::string_view::npos) {
      break;
    }
    offset = line_end + 1;
    ++line_number;
  }

  while (indents.size() > 1) {
    indents.pop_back();
    result.push_back({TokenKind::Dedent, "<dedent>", line_number, 1});
  }
  result.push_back({TokenKind::End, "<end>", line_number, 1});
  return result;
}

enum class DataType { Bool, Int, String, StringSet };

struct Expr {
  enum class Kind { Literal, Name, Unary, Binary, Exists, Count, SetInsert, SetErase };
  Kind kind{Kind::Literal};
  std::optional<Value> literal;
  std::string text;
  std::shared_ptr<Expr> left;
  std::shared_ptr<Expr> right;
  std::shared_ptr<Expr> third;
};

using ExprPtr = std::shared_ptr<Expr>;

ExprPtr make_literal(Value value) {
  auto expr = std::make_shared<Expr>();
  expr->kind = Expr::Kind::Literal;
  expr->literal = std::move(value);
  return expr;
}

ExprPtr make_name(std::string name) {
  auto expr = std::make_shared<Expr>();
  expr->kind = Expr::Kind::Name;
  expr->text = std::move(name);
  return expr;
}

ExprPtr make_unary(std::string op, ExprPtr operand) {
  auto expr = std::make_shared<Expr>();
  expr->kind = Expr::Kind::Unary;
  expr->text = std::move(op);
  expr->left = std::move(operand);
  return expr;
}

ExprPtr make_binary(std::string op, ExprPtr left, ExprPtr right) {
  auto expr = std::make_shared<Expr>();
  expr->kind = Expr::Kind::Binary;
  expr->text = std::move(op);
  expr->left = std::move(left);
  expr->right = std::move(right);
  return expr;
}

struct Field {
  enum class Merge { Reject, Equal, Union };
  std::string name;
  DataType type;
  Value initial;
  Merge merge{Merge::Reject};
};

struct State {
  std::string name;
  std::string context;
  bool initial{false};
  std::vector<Field> fields;
  std::vector<ExprPtr> invariants;
};

struct Parameter {
  std::string name;
  DataType type;
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
};

struct Transition {
  std::string name;
  std::string event;
  std::vector<Parameter> parameters;
  std::string from;
  std::string to;
  std::vector<Assignment> assignments;
  ExprPtr condition{make_literal(Value(true))};
  std::shared_ptr<ActionExpr> action;
  std::set<std::string, std::less<>> reads;
  std::set<std::string, std::less<>> writes;
};

class FlatParser {
 public:
  explicit FlatParser(std::vector<Token> tokens) : tokens_(std::move(tokens)) {}

  ExprPtr expression() { return parse_or(); }

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
      return make_binary(op, std::move(left), parse_add());
    }
    return left;
  }

  ExprPtr parse_add() {
    auto left = parse_unary();
    while (!at_end() && (peek().text == "+" || peek().text == "-")) {
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
      auto result = parse_or();
      expect(")");
      return result;
    }
    if (match("exists")) {
      const std::string variable = identifier().text;
      expect("in");
      auto domain = parse_add();
      expect("where");
      auto predicate = parse_or();
      auto result = std::make_shared<Expr>();
      result->kind = Expr::Kind::Exists;
      result->text = variable;
      result->left = std::move(domain);
      result->right = std::move(predicate);
      return result;
    }
    if (match("count")) {
      expect("(");
      auto result = std::make_shared<Expr>();
      result->kind = Expr::Kind::Count;
      result->left = parse_or();
      expect(")");
      return result;
    }
    if (peek().text == "insert" || peek().text == "erase") {
      const bool inserting = take().text == "insert";
      expect("(");
      auto result = std::make_shared<Expr>();
      result->kind = inserting ? Expr::Kind::SetInsert : Expr::Kind::SetErase;
      result->left = parse_or();
      expect(",");
      result->right = parse_or();
      expect(")");
      return result;
    }
    if (peek().kind == TokenKind::Integer) {
      const Token token = take();
      std::int64_t value = 0;
      const auto parsed = std::from_chars(token.text.data(), token.text.data() + token.text.size(), value);
      if (parsed.ec != std::errc{}) {
        fail(token, "integer is out of range");
      }
      return make_literal(Value(value));
    }
    if (peek().kind == TokenKind::String) {
      return make_literal(Value(take().text));
    }
    if (match("true")) {
      return make_literal(Value(true));
    }
    if (match("false")) {
      return make_literal(Value(false));
    }
    Token name = identifier();
    std::string path = std::move(name.text);
    while (match(".")) {
      path += "." + identifier().text;
    }
    return make_name(std::move(path));
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
        result->arguments.push_back(parse_or());
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
  State state();
  Transition transition();
  ExprPtr line_expression();
  ExprPtr block_expression();
  std::shared_ptr<ActionExpr> block_action();
  std::vector<Token> take_block_tokens();

  std::vector<Token> tokens_;
  std::size_t cursor_{0};
};

}  // namespace

struct Program::Impl {
  std::vector<State> states;
  std::vector<Transition> transitions;
};

namespace {

DataType Parser::type() {
  if (match("bool")) return DataType::Bool;
  if (match("int")) return DataType::Int;
  if (match("string")) return DataType::String;
  if (match("set")) {
    expect("<");
    expect("string");
    expect(">");
    return DataType::StringSet;
  }
  fail(peek(), "expected bool, int, string, or set<string>");
}

Value Parser::initial_value(DataType expected_type) {
  if (expected_type == DataType::Bool) {
    if (match("true")) return Value(true);
    if (match("false")) return Value(false);
    fail(peek(), "expected boolean literal");
  }
  if (expected_type == DataType::Int) {
    bool negative = match("-");
    if (!at(TokenKind::Integer)) fail(peek(), "expected integer literal");
    const Token token = take();
    std::int64_t value = 0;
    const auto parsed = std::from_chars(token.text.data(), token.text.data() + token.text.size(), value);
    if (parsed.ec != std::errc{}) fail(token, "integer is out of range");
    return Value(negative ? -value : value);
  }
  if (expected_type == DataType::String) {
    if (!at(TokenKind::String)) fail(peek(), "expected string literal");
    return Value(take().text);
  }
  expect("{");
  StringSet value;
  if (!match("}")) {
    do {
      if (!at(TokenKind::String)) fail(peek(), "set<string> needs string elements");
      if (!value.values.insert(take().text).second) {
        fail(tokens_[cursor_ - 1], "duplicate set element");
      }
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
  FlatParser parser(std::move(flat));
  auto result = parser.expression();
  parser.expect_end();
  return result;
}

ExprPtr Parser::block_expression() {
  FlatParser parser(take_block_tokens());
  auto result = parser.expression();
  parser.expect_end();
  return result;
}

std::shared_ptr<ActionExpr> Parser::block_action() {
  FlatParser parser(take_block_tokens());
  return parser.action();
}

State Parser::state() {
  expect("state");
  State result;
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
    Field field{"", DataType::Bool, Value(false), Field::Merge::Reject};
    field.name = identifier();
    expect(":");
    field.type = type();
    expect("=");
    field.initial = initial_value(field.type);
    if (match("merge")) {
      if (match("equal")) field.merge = Field::Merge::Equal;
      else if (match("union")) field.merge = Field::Merge::Union;
      else fail(peek(), "expected equal or union merge relation");
      if (field.merge == Field::Merge::Union && field.type != DataType::StringSet) {
        fail(tokens_[cursor_ - 1], "union merge needs set<string>");
      }
    }
    newline();
    result.fields.push_back(std::move(field));
  }
  dedent();
  return result;
}

Transition Parser::transition() {
  expect("transition");
  Transition result;
  result.name = identifier();
  expect("@");
  result.event = identifier();
  expect("(");
  if (!match(")")) {
    do {
      Parameter parameter;
      parameter.name = identifier();
      expect(":");
      parameter.type = type();
      result.parameters.push_back(std::move(parameter));
    } while (match(","));
    expect(")");
  }
  expect(":");
  newline();
  indent();
  while (!at(TokenKind::Dedent)) {
    if (match("from")) {
      result.from = identifier();
      newline();
      continue;
    }
    if (match("to")) {
      result.to = identifier();
      expect(":");
      newline();
      indent();
      while (!at(TokenKind::Dedent)) {
        Assignment assignment;
        assignment.field = identifier();
        expect("=");
        assignment.value = line_expression();
        result.assignments.push_back(std::move(assignment));
      }
      dedent();
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
    fail(peek(), "expected from, to, where, or do");
  }
  dedent();
  return result;
}

std::shared_ptr<Program::Impl> Parser::program() {
  auto result = std::make_shared<Program::Impl>();
  while (!at(TokenKind::End)) {
    if (at(TokenKind::Newline)) {
      take();
    } else if (at("state")) {
      result->states.push_back(state());
    } else if (at("transition")) {
      result->transitions.push_back(transition());
    } else {
      fail(peek(), "expected state or transition");
    }
  }
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
    case Value::Kind::Bool: return DataType::Bool;
    case Value::Kind::Int: return DataType::Int;
    case Value::Kind::String: return DataType::String;
    case Value::Kind::StringSet: return DataType::StringSet;
  }
  throw Error("invalid value kind");
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

DataType infer_type(const ExprPtr& expr, const TypeEnvironment& state,
                    const TypeEnvironment& event, TypeEnvironment locals) {
  if (!expr) throw Error("missing expression");
  switch (expr->kind) {
    case Expr::Kind::Literal: return value_type(*expr->literal);
    case Expr::Kind::Name: {
      if (expr->text == "round") return DataType::Int;
      std::string name = expr->text;
      if (name.starts_with("before.")) {
        name = name.substr(name.find('.') + 1);
        const auto found = state.find(name);
        if (found == state.end()) throw Error("unknown state field '" + name + "'");
        return found->second;
      }
      if (const auto found = locals.find(name); found != locals.end()) return found->second;
      if (const auto found = event.find(name); found != event.end()) return found->second;
      if (const auto found = state.find(name); found != state.end()) return found->second;
      throw Error("unknown value '" + name + "'");
    }
    case Expr::Kind::Unary: {
      const DataType operand = infer_type(expr->left, state, event, std::move(locals));
      const DataType required = expr->text == "not" ? DataType::Bool : DataType::Int;
      if (operand != required) throw Error("wrong operand type for '" + expr->text + "'");
      return required;
    }
    case Expr::Kind::Binary: {
      const DataType left = infer_type(expr->left, state, event, locals);
      const DataType right = infer_type(expr->right, state, event, std::move(locals));
      if (expr->text == "and" || expr->text == "or") {
        if (left != DataType::Bool || right != DataType::Bool) {
          throw Error("logical operators need bool operands");
        }
        return DataType::Bool;
      }
      if (expr->text == "in") {
        if (left != DataType::String || right != DataType::StringSet) {
          throw Error("membership needs string in set<string>");
        }
        return DataType::Bool;
      }
      if (expr->text == "+" || expr->text == "-") {
        if (left != DataType::Int || right != DataType::Int) {
          throw Error("arithmetic operators need int operands");
        }
        return DataType::Int;
      }
      if (left != right) throw Error("comparison operands have different types");
      if ((expr->text == "<" || expr->text == "<=" || expr->text == ">" ||
           expr->text == ">=") &&
          left != DataType::Int && left != DataType::String) {
        throw Error("ordered comparison needs int or string operands");
      }
      return DataType::Bool;
    }
    case Expr::Kind::Exists: {
      if (infer_type(expr->left, state, event, locals) != DataType::StringSet) {
        throw Error("exists currently needs a set<string> domain");
      }
      locals.insert_or_assign(expr->text, DataType::String);
      if (infer_type(expr->right, state, event, std::move(locals)) != DataType::Bool) {
        throw Error("exists predicate must be bool");
      }
      return DataType::Bool;
    }
    case Expr::Kind::Count:
      if (infer_type(expr->left, state, event, std::move(locals)) != DataType::StringSet) {
        throw Error("count currently needs set<string>");
      }
      return DataType::Int;
    case Expr::Kind::SetInsert:
    case Expr::Kind::SetErase:
      if (infer_type(expr->left, state, event, locals) != DataType::StringSet ||
          infer_type(expr->right, state, event, std::move(locals)) != DataType::String) {
        throw Error("insert/erase need (set<string>, string)");
      }
      return DataType::StringSet;
  }
  throw Error("invalid expression");
}

void verify_action(const std::shared_ptr<ActionExpr>& action, const TypeEnvironment& state,
                   const TypeEnvironment& event, std::unordered_set<std::string>& labels) {
  if (!action) return;
  if (action->kind != ActionExpr::Kind::Call) {
    for (const auto& child : action->children) verify_action(child, state, event, labels);
    return;
  }
  if (!labels.insert(action->label).second) {
    throw Error("duplicate action label '" + action->label + "'");
  }
  for (const ExprPtr& argument : action->arguments) {
    static_cast<void>(infer_type(argument, state, event, {}));
  }
  if (const auto parameter = event.find(action->context);
      parameter != event.end() && parameter->second != DataType::String) {
    throw Error("action context event field must be string");
  }
  if (action->context.starts_with("before.")) {
    const std::string field = action->context.substr(action->context.find('.') + 1);
    const auto found = state.find(field);
    if (found == state.end()) throw Error("unknown context field '" + field + "'");
    if (found->second != DataType::String) throw Error("action context field must be string");
  }
}

void collect_reads(const ExprPtr& expr, const TypeEnvironment& state,
                   std::set<std::string, std::less<>> shadowed,
                   std::set<std::string, std::less<>>& reads) {
  if (!expr) return;
  if (expr->kind == Expr::Kind::Name) {
    if (expr->text.starts_with("before.")) {
      reads.insert(expr->text.substr(expr->text.find('.') + 1));
    } else if (!shadowed.contains(expr->text) && state.contains(expr->text)) {
      reads.insert(expr->text);
    }
    return;
  }
  if (expr->kind == Expr::Kind::Exists) {
    collect_reads(expr->left, state, shadowed, reads);
    shadowed.insert(expr->text);
    collect_reads(expr->right, state, std::move(shadowed), reads);
    return;
  }
  collect_reads(expr->left, state, shadowed, reads);
  collect_reads(expr->right, state, shadowed, reads);
  collect_reads(expr->third, state, std::move(shadowed), reads);
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

void verify_program(Program::Impl& program) {
  if (program.states.empty()) throw Error("program must define at least one state");
  std::unordered_set<std::string> names;
  std::size_t initial_count = 0;
  for (const State& state : program.states) {
    if (!names.insert(state.name).second) throw Error("duplicate state '" + state.name + "'");
    initial_count += state.initial ? 1U : 0U;
    std::unordered_set<std::string> fields;
    for (const Field& field : state.fields) {
      if (!fields.insert(field.name).second) {
        throw Error("duplicate field '" + field.name + "' in state '" + state.name + "'");
      }
    }
  }
  if (initial_count != 1) throw Error("program must have exactly one initial state");

  names.clear();
  std::map<std::string, std::vector<Parameter>, std::less<>> event_schemas;
  for (Transition& transition : program.transitions) {
    if (!names.insert(transition.name).second) {
      throw Error("duplicate transition '" + transition.name + "'");
    }
    if (transition.from.empty() || transition.to.empty()) {
      throw Error("transition '" + transition.name + "' needs from and to");
    }
    const State& source = find_state(program, transition.from);
    const State& target = find_state(program, transition.to);
    TypeEnvironment state_types;
    for (const Field& field : source.fields) state_types.emplace(field.name, field.type);
    std::unordered_set<std::string> parameters;
    TypeEnvironment event_types;
    for (const Parameter& parameter : transition.parameters) {
      if (!parameters.insert(parameter.name).second) {
        throw Error("duplicate event parameter '" + parameter.name + "'");
      }
      event_types.emplace(parameter.name, parameter.type);
    }
    const auto [schema, inserted] = event_schemas.emplace(transition.event, transition.parameters);
    if (!inserted) {
      const auto& previous = schema->second;
      if (previous.size() != transition.parameters.size()) {
        throw Error("event '" + transition.event + "' has inconsistent schemas");
      }
      for (std::size_t index = 0; index < previous.size(); ++index) {
        if (previous[index].name != transition.parameters[index].name ||
            previous[index].type != transition.parameters[index].type) {
          throw Error("event '" + transition.event + "' has inconsistent schemas");
        }
      }
    }
    if (infer_type(transition.condition, state_types, event_types, {}) != DataType::Bool) {
      throw Error("where clause in transition '" + transition.name + "' must be bool");
    }
    std::unordered_set<std::string> assigned;
    for (const Assignment& assignment : transition.assignments) {
      const Field& field = find_field(target, assignment.field);
      if (!assigned.insert(assignment.field).second) {
        throw Error("field '" + assignment.field + "' is assigned twice");
      }
      if (infer_type(assignment.value, state_types, event_types, {}) != field.type) {
        throw Error("assignment to '" + assignment.field + "' has the wrong type");
      }
    }
    std::unordered_set<std::string> labels;
    verify_action(transition.action, state_types, event_types, labels);

    std::set<std::string, std::less<>> shadowed;
    for (const Parameter& parameter : transition.parameters) shadowed.insert(parameter.name);
    collect_reads(transition.condition, state_types, shadowed, transition.reads);
    for (const Assignment& assignment : transition.assignments) {
      transition.writes.insert(assignment.field);
      collect_reads(assignment.value, state_types, shadowed, transition.reads);
    }
    if (transition.from != transition.to) {
      for (const Field& field : target.fields) transition.writes.insert(field.name);
    }
    collect_action_reads(transition.action, state_types, shadowed, transition.reads);
  }
  for (const State& state : program.states) {
    TypeEnvironment state_types;
    for (const Field& field : state.fields) state_types.emplace(field.name, field.type);
    for (const ExprPtr& invariant : state.invariants) {
      if (infer_type(invariant, state_types, {}, {}) != DataType::Bool) {
        throw Error("invariant in state '" + state.name + "' must be bool");
      }
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

bool equal_values(const Value& left, const Value& right) { return left == right; }

int compare_values(const Value& left, const Value& right) {
  if (left.kind() != right.kind()) throw Error("comparison operands have different types");
  switch (left.kind()) {
    case Value::Kind::Bool:
      return static_cast<int>(left.as_bool()) - static_cast<int>(right.as_bool());
    case Value::Kind::Int:
      return left.as_int() < right.as_int() ? -1 : left.as_int() > right.as_int() ? 1 : 0;
    case Value::Kind::String:
      return left.as_string() < right.as_string() ? -1
             : left.as_string() > right.as_string() ? 1 : 0;
    case Value::Kind::StringSet:
      throw Error("sets only support equality and membership");
  }
  throw Error("invalid comparison");
}

int canonical_value_order(const Value& left, const Value& right) {
  if (left.kind() != right.kind()) {
    return static_cast<int>(left.kind()) < static_cast<int>(right.kind()) ? -1 : 1;
  }
  if (left.kind() != Value::Kind::StringSet) return compare_values(left, right);
  const auto& lhs = left.as_string_set().values;
  const auto& rhs = right.as_string_set().values;
  if (std::lexicographical_compare(lhs.begin(), lhs.end(), rhs.begin(), rhs.end())) return -1;
  if (std::lexicographical_compare(rhs.begin(), rhs.end(), lhs.begin(), lhs.end())) return 1;
  return 0;
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
  const auto local = environment.locals.find(name);
  if (local != environment.locals.end()) return local->second;
  if (name.starts_with("before.")) {
    const std::string field = name.substr(name.find('.') + 1);
    const auto found = environment.state.find(field);
    if (found == environment.state.end()) throw Error("unknown state field '" + field + "'");
    return found->second;
  }
  if (environment.event != nullptr) {
    const auto found = environment.event->fields.find(name);
    if (found != environment.event->fields.end()) return found->second;
  }
  const auto field = environment.state.find(name);
  if (field != environment.state.end()) return field->second;
  throw Error("unknown value '" + name + "'");
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
        if (operand.as_int() == std::numeric_limits<std::int64_t>::min()) {
          throw Error("integer negation overflow");
        }
        return Value(-operand.as_int());
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
      const Value left = evaluate(expr->left, environment);
      const Value right = evaluate(expr->right, environment);
      if (expr->text == "=" || expr->text == "==") return Value(equal_values(left, right));
      if (expr->text == "!=") return Value(!equal_values(left, right));
      if (expr->text == "in") {
        return Value(right.as_string_set().values.contains(left.as_string()));
      }
      if (expr->text == "+" || expr->text == "-") {
        const std::int64_t lhs = left.as_int();
        const std::int64_t rhs = right.as_int();
        std::int64_t result = 0;
#if defined(__GNUC__) || defined(__clang__)
        const bool overflow = expr->text == "+" ? __builtin_add_overflow(lhs, rhs, &result)
                                                  : __builtin_sub_overflow(lhs, rhs, &result);
        if (overflow) throw Error("integer arithmetic overflow");
#else
        if ((expr->text == "+" && ((rhs > 0 && lhs > std::numeric_limits<std::int64_t>::max() - rhs) ||
                                    (rhs < 0 && lhs < std::numeric_limits<std::int64_t>::min() - rhs))) ||
            (expr->text == "-" && ((rhs < 0 && lhs > std::numeric_limits<std::int64_t>::max() + rhs) ||
                                    (rhs > 0 && lhs < std::numeric_limits<std::int64_t>::min() + rhs)))) {
          throw Error("integer arithmetic overflow");
        }
        result = expr->text == "+" ? lhs + rhs : lhs - rhs;
#endif
        return Value(result);
      }
      const int order = compare_values(left, right);
      if (expr->text == "<") return Value(order < 0);
      if (expr->text == "<=") return Value(order <= 0);
      if (expr->text == ">") return Value(order > 0);
      if (expr->text == ">=") return Value(order >= 0);
      throw Error("unknown binary operator '" + expr->text + "'");
    }
    case Expr::Kind::Exists: {
      const Value domain_value = evaluate(expr->left, environment);
      const StringSet& domain = domain_value.as_string_set();
      const auto previous = environment.locals.find(expr->text);
      const std::optional<Value> saved = previous == environment.locals.end()
                                             ? std::nullopt
                                             : std::optional<Value>(previous->second);
      for (const std::string& item : domain.values) {
        environment.locals.insert_or_assign(expr->text, Value(item));
        if (evaluate(expr->right, environment).as_bool()) {
          if (saved) environment.locals.insert_or_assign(expr->text, *saved);
          else environment.locals.erase(expr->text);
          return Value(true);
        }
      }
      if (saved) environment.locals.insert_or_assign(expr->text, *saved);
      else environment.locals.erase(expr->text);
      return Value(false);
    }
    case Expr::Kind::Count: {
      const std::size_t size = evaluate(expr->left, environment).as_string_set().values.size();
      if (size > static_cast<std::size_t>(std::numeric_limits<std::int64_t>::max())) {
        throw Error("set size exceeds int range");
      }
      return Value(static_cast<std::int64_t>(size));
    }
    case Expr::Kind::SetInsert:
    case Expr::Kind::SetErase: {
      StringSet result = evaluate(expr->left, environment).as_string_set();
      const std::string item = evaluate(expr->right, environment).as_string();
      if (expr->kind == Expr::Kind::SetInsert) result.values.insert(item);
      else result.values.erase(item);
      return Value(std::move(result));
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

void validate_event(const Transition& transition, const Event& event) {
  if (event.fields.size() != transition.parameters.size()) {
    throw Error("event '" + event.name + "' has the wrong number of fields");
  }
  for (const Parameter& parameter : transition.parameters) {
    const auto found = event.fields.find(parameter.name);
    if (found == event.fields.end()) {
      throw Error("event '" + event.name + "' is missing field '" + parameter.name + "'");
    }
    if (value_type(found->second) != parameter.type) {
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
Value::Value(std::string value) : kind_(Kind::String), string_value_(std::move(value)) {}
Value::Value(const char* value) : Value(std::string(value)) {}
Value::Value(StringSet value) : kind_(Kind::StringSet), set_value_(std::move(value)) {}

Value::Kind Value::kind() const noexcept { return kind_; }
bool Value::as_bool() const {
  if (kind_ != Kind::Bool) throw Error("expected bool value");
  return bool_value_;
}
std::int64_t Value::as_int() const {
  if (kind_ != Kind::Int) throw Error("expected int value");
  return int_value_;
}
const std::string& Value::as_string() const {
  if (kind_ != Kind::String) throw Error("expected string value");
  return string_value_;
}
const StringSet& Value::as_string_set() const {
  if (kind_ != Kind::StringSet) throw Error("expected set<string> value");
  return set_value_;
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

  const auto initial = std::find_if(implementation->states.begin(), implementation->states.end(),
                                    [](const State& state) { return state.initial; });
  verify_invariants(*initial, initial_values(*initial), 0);
  return Program(std::move(implementation));
}

Engine::Engine(Program program) : program_(std::move(program)) {
  if (program_.empty()) throw Error("cannot construct an engine from an empty program");
  const Program::Impl& implementation = *program_.implementation();
  const auto initial = std::find_if(implementation.states.begin(), implementation.states.end(),
                                    [](const State& state) { return state.initial; });
  current_state_ = initial->name;
  values_ = initial_values(*initial);
}

ParallelStepResult Engine::step_parallel(const std::vector<Event>& events) {
  if (events.empty()) throw Error("a parallel step needs at least one event");
  if (round_ == std::numeric_limits<std::uint64_t>::max()) {
    throw Error("simulation round overflow");
  }
  const Program::Impl& program = *program_.implementation();
  struct Prepared {
    const Transition* transition;
    std::map<std::string, Value, std::less<>> writes;
    ActionPlan actions;
    std::set<std::string, std::less<>> causal_predecessors;
  };
  std::vector<Prepared> prepared;
  prepared.reserve(events.size());
  std::string target_name;

  std::vector<const Event*> ordered_events;
  ordered_events.reserve(events.size());
  for (const Event& event : events) ordered_events.push_back(&event);
  std::stable_sort(ordered_events.begin(), ordered_events.end(),
                   [](const Event* left, const Event* right) { return event_less(*left, *right); });

  for (const Event* event_pointer : ordered_events) {
    const Event& event = *event_pointer;
    std::vector<const Transition*> enabled;
    for (const Transition& transition : program.transitions) {
      if (transition.from != current_state_ || transition.event != event.name) continue;
      validate_event(transition, event);
      Environment environment{values_, &event, {}, round_};
      if (evaluate(transition.condition, environment).as_bool()) enabled.push_back(&transition);
    }
    if (enabled.empty()) {
      throw Error("no transition accepts event '" + event.name + "' from state '" +
                  current_state_ + "'");
    }
    if (enabled.size() != 1) {
      throw Error("event '" + event.name + "' enables multiple transitions from state '" +
                  current_state_ + "'");
    }

    const Transition& transition = *enabled.front();
    if (target_name.empty()) target_name = transition.to;
    if (target_name != transition.to) {
      throw Error("parallel transitions must enter the same target state");
    }
    const State& source = find_state(program, transition.from);
    const State& target = find_state(program, transition.to);
    Environment environment{values_, &event, {}, round_};
    Prepared decision{&transition, {}, {}, {}};
    for (const std::string& field : transition.reads) {
      const auto writers = last_writers_.find(field);
      if (writers != last_writers_.end()) {
        decision.causal_predecessors.insert(writers->second.begin(), writers->second.end());
      }
    }
    if (transition.from != transition.to) {
      decision.writes = initial_values(target);
    }
    for (const Assignment& assignment : transition.assignments) {
      Value value = evaluate(assignment.value, environment);
      const Field& field = find_field(target, assignment.field);
      if (value_type(value) != field.type) {
        throw Error("assignment to '" + assignment.field + "' has the wrong type");
      }
      decision.writes.insert_or_assign(assignment.field, std::move(value));
    }
    if (transition.action) {
      std::unordered_set<std::string> labels;
      build_plan(transition.action, environment, source, decision.actions, labels);
      std::sort(decision.actions.dependencies.begin(), decision.actions.dependencies.end());
      decision.actions.dependencies.erase(
          std::unique(decision.actions.dependencies.begin(), decision.actions.dependencies.end()),
          decision.actions.dependencies.end());
    }
    prepared.push_back(std::move(decision));
  }

  const State& target = find_state(program, target_name);
  std::map<std::string, Value, std::less<>> next =
      target_name == current_state_ ? values_ : initial_values(target);
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
    const Field& field = find_field(target, name);
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
    StringSet merged = next.at(name).as_string_set();
    for (const Value& value : values) {
      const StringSet& candidate = value.as_string_set();
      merged.values.insert(candidate.values.begin(), candidate.values.end());
    }
    next.insert_or_assign(name, Value(std::move(merged)));
  }
  verify_invariants(target, next, round_ + 1U);

  ParallelStepResult result;
  result.round = round_ + 1U;
  result.state = next;
  result.transitions.reserve(prepared.size());
  for (std::size_t index = 0; index < prepared.size(); ++index) {
    Prepared& decision = prepared[index];
    StepResult step_result;
    step_result.round = result.round;
    step_result.id = "r" + std::to_string(result.round) + ":" + std::to_string(index);
    step_result.transition = decision.transition->name;
    step_result.from_state = decision.transition->from;
    step_result.to_state = decision.transition->to;
    step_result.state = next;
    step_result.actions = std::move(decision.actions);
    step_result.reads = decision.transition->reads;
    step_result.writes = decision.transition->writes;
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
  current_state_ = target_name;
  values_ = std::move(next);
  return result;
}

StepResult Engine::step(const Event& event) {
  ParallelStepResult result = step_parallel(std::vector<Event>{event});
  return std::move(result.transitions.front());
}

std::string Engine::current_state() const { return current_state_; }
std::uint64_t Engine::current_round() const noexcept { return round_; }
const std::map<std::string, Value, std::less<>>& Engine::values() const { return values_; }

std::string value_text(const Value& value) {
  switch (value.kind()) {
    case Value::Kind::Bool: return value.as_bool() ? "true" : "false";
    case Value::Kind::Int: return std::to_string(value.as_int());
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
    case Expr::Kind::SetInsert:
    case Expr::Kind::SetErase: features.insert(LanguageFeature::PureSetUpdate); break;
    case Expr::Kind::Literal:
    case Expr::Kind::Name:
    case Expr::Kind::Unary:
    case Expr::Kind::Binary:
    case Expr::Kind::Count: break;
  }
  collect_features(expr->left, features);
  collect_features(expr->right, features);
  collect_features(expr->third, features);
}

void collect_action_features(const std::shared_ptr<ActionExpr>& action,
                             FeatureSet& features) {
  if (!action) return;
  features.insert(LanguageFeature::ActionDag);
  for (const ExprPtr& argument : action->arguments) collect_features(argument, features);
  for (const auto& child : action->children) collect_action_features(child, features);
}

}  // namespace

FeatureSet required_features(const Program& program) {
  if (program.empty()) throw Error("cannot inspect features of an empty program");
  FeatureSet result{LanguageFeature::TypedState, LanguageFeature::ParallelEventBag};
  const Program::Impl& implementation = *program.implementation();
  for (const State& state : implementation.states) {
    for (const Field& field : state.fields) {
      if (field.type == DataType::StringSet) result.insert(LanguageFeature::FiniteStringSet);
      if (field.merge == Field::Merge::Equal) result.insert(LanguageFeature::EqualMerge);
      if (field.merge == Field::Merge::Union) result.insert(LanguageFeature::UnionMerge);
    }
    for (const ExprPtr& invariant : state.invariants) collect_features(invariant, result);
  }
  for (const Transition& transition : implementation.transitions) {
    collect_features(transition.condition, result);
    for (const Assignment& assignment : transition.assignments) {
      collect_features(assignment.value, result);
    }
    collect_action_features(transition.action, result);
  }
  return result;
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
    case LanguageFeature::FiniteStringSet: return "finite-string-set";
    case LanguageFeature::ExistentialSearch: return "existential-search";
    case LanguageFeature::PureSetUpdate: return "pure-set-update";
    case LanguageFeature::ActionDag: return "action-dag";
    case LanguageFeature::ParallelEventBag: return "parallel-event-bag";
    case LanguageFeature::EqualMerge: return "equal-merge";
    case LanguageFeature::UnionMerge: return "union-merge";
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

}  // namespace dtessl
