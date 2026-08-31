#include "dtessl/dtessl.hpp"
#include "dtessl/backend.hpp"
#include "dtessl/language_service.hpp"
#include "dtessl/solver.hpp"
#include "dtessl/semantic_descriptor.hpp"
#include "dtessl/version.hpp"
#include "line_editor.hpp"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#if defined(_WIN32)
#include <io.h>
#else
#include <unistd.h>
#endif

namespace {

std::string read_file(const std::string& path) {
  std::ifstream input(path, std::ios::binary);
  if (!input) throw dtessl::Error("cannot open '" + path + "'");
  std::ostringstream buffer;
  buffer << input.rdbuf();
  if (!input.good() && !input.eof()) throw dtessl::Error("cannot read '" + path + "'");
  return buffer.str();
}

dtessl::Value parse_value(std::string_view text) {
  if (text == "true") return dtessl::Value(true);
  if (text == "false") return dtessl::Value(false);
  std::size_t digit = (!text.empty() && (text.front() == '-' || text.front() == '+')) ? 1U : 0U;
  const bool integer = digit < text.size() &&
                       std::all_of(text.begin() + static_cast<std::ptrdiff_t>(digit), text.end(),
                                   [](char value) {
                                     return std::isdigit(static_cast<unsigned char>(value)) != 0;
                                   });
  if (integer) {
    return dtessl::Value(dtessl::ExactInt::parse(text));
  }
  return dtessl::Value(std::string(text));
}

dtessl::Event parse_event(char** argv, int start, int end) {
  if (start >= end) throw dtessl::Error("missing event name");
  dtessl::Event event;
  event.name = argv[start];
  for (int index = start + 1; index < end; ++index) {
    const std::string argument = argv[index];
    const std::size_t equals = argument.find('=');
    if (equals == std::string::npos || equals == 0) {
      throw dtessl::Error("event fields must use name=value");
    }
    const std::string name = argument.substr(0, equals);
    if (!event.fields.emplace(name, parse_value(std::string_view(argument).substr(equals + 1))).second) {
      throw dtessl::Error("duplicate event field '" + name + "'");
    }
  }
  return event;
}

dtessl::Event parse_event(const std::vector<std::string>& words, std::size_t start,
                          std::size_t end) {
  if (start >= end || end > words.size()) throw dtessl::Error("missing event name");
  dtessl::Event event;
  event.name = words[start];
  for (std::size_t index = start + 1; index < end; ++index) {
    const std::string& argument = words[index];
    const std::size_t equals = argument.find('=');
    if (equals == std::string::npos || equals == 0) {
      throw dtessl::Error("event fields must use name=value");
    }
    const std::string name = argument.substr(0, equals);
    if (!event.fields.emplace(name, parse_value(std::string_view(argument).substr(equals + 1)))
             .second) {
      throw dtessl::Error("duplicate event field '" + name + "'");
    }
  }
  return event;
}

dtessl::Event parse_event(const std::vector<std::string>& words, std::size_t start) {
  return parse_event(words, start, words.size());
}

std::vector<std::string> split_words(std::string_view line) {
  std::vector<std::string> words;
  std::string word;
  char quote = 0;
  bool escaped = false;
  for (const char item : line) {
    if (escaped) {
      word.push_back(item);
      escaped = false;
    } else if (item == '\\') {
      escaped = true;
    } else if (quote != 0) {
      if (item == quote) quote = 0;
      else word.push_back(item);
    } else if (item == '"' || item == '\'') {
      quote = item;
    } else if (std::isspace(static_cast<unsigned char>(item)) != 0) {
      if (!word.empty()) {
        words.push_back(std::move(word));
        word.clear();
      }
    } else {
      word.push_back(item);
    }
  }
  if (escaped || quote != 0) throw dtessl::Error("unfinished quote or escape");
  if (!word.empty()) words.push_back(std::move(word));
  return words;
}

bool stdout_is_terminal() {
#if defined(_WIN32)
  return _isatty(_fileno(stdout)) != 0;
#else
  return isatty(STDOUT_FILENO) != 0;
#endif
}

const char* ansi_color(dtessl::SyntaxClass syntax) {
  switch (syntax) {
    case dtessl::SyntaxClass::Keyword: return "\033[1;35m";
    case dtessl::SyntaxClass::BuiltinType: return "\033[1;36m";
    case dtessl::SyntaxClass::Identifier: return "\033[0;37m";
    case dtessl::SyntaxClass::Number: return "\033[0;33m";
    case dtessl::SyntaxClass::String: return "\033[0;32m";
    case dtessl::SyntaxClass::Comment: return "\033[0;90m";
    case dtessl::SyntaxClass::Operator: return "\033[1;34m";
    case dtessl::SyntaxClass::Punctuation: return "\033[0;36m";
  }
  return "";
}

std::string highlighted_source(std::string_view source,
                               const std::vector<dtessl::HighlightToken>& tokens,
                               bool color) {
  if (!color) return std::string(source);
  std::string result;
  std::size_t cursor = 0;
  for (const dtessl::HighlightToken& token : tokens) {
    const std::size_t begin = token.range.start.offset;
    const std::size_t end = token.range.end.offset;
    if (begin < cursor || end > source.size()) continue;
    result.append(source.substr(cursor, begin - cursor));
    result += ansi_color(token.syntax);
    result.append(source.substr(begin, end - begin));
    result += "\033[0m";
    cursor = end;
  }
  result.append(source.substr(cursor));
  return result;
}

void print_diagnostics(const dtessl::LanguageAnalysis& analysis, std::ostream& out) {
  if (analysis.diagnostics.empty()) {
    out << "ok\n";
    return;
  }
  for (const dtessl::Diagnostic& diagnostic : analysis.diagnostics) {
    out << (analysis.uri.empty() ? "<memory>" : analysis.uri) << ':'
        << diagnostic.range.start.line << ':' << diagnostic.range.start.column << ": error["
        << diagnostic.code << "]: " << diagnostic.message << '\n';
  }
}

void print_highlights(std::string_view source, const dtessl::LanguageAnalysis& analysis,
                      std::ostream& out) {
  for (const dtessl::HighlightToken& token : analysis.highlights) {
    out << token.range.start.line << ':' << token.range.start.column << '-'
        << token.range.end.line << ':' << token.range.end.column << ' '
        << dtessl::syntax_class_name(token.syntax) << ' ';
    const std::string_view text = source.substr(
        token.range.start.offset, token.range.end.offset - token.range.start.offset);
    for (const char item : text) {
      if (item == '\n') out << "\\n";
      else if (item == '\r') out << "\\r";
      else out << item;
    }
    out << '\n';
  }
}

std::string styled(std::string_view text, std::string_view style, bool color) {
  if (!color) return std::string(text);
  return std::string(style) + std::string(text) + "\033[0m";
}

std::string field_set_text(const std::set<std::string, std::less<>>& fields) {
  std::string result;
  for (const std::string& field : fields) {
    if (!result.empty()) result += ", ";
    result += field;
  }
  return result.empty() ? "∅" : result;
}

std::string action_text(const dtessl::ActionCall& call) {
  std::string result = "$" + call.function + "(";
  for (std::size_t index = 0; index < call.arguments.size(); ++index) {
    if (index != 0) result += ", ";
    result += dtessl::value_text(call.arguments[index]);
  }
  result += ")";
  if (!call.context.empty()) result += " @ " + call.context;
  return result;
}

void print_repl_result(
    const std::map<std::string, dtessl::Value, std::less<>>& before,
    const dtessl::StepResult& result, bool color) {
  static constexpr std::string_view green = "\033[1;32m";
  static constexpr std::string_view cyan = "\033[1;36m";
  static constexpr std::string_view magenta = "\033[1;35m";
  static constexpr std::string_view yellow = "\033[1;33m";
  static constexpr std::string_view dim = "\033[0;90m";

  std::cout << '\n'
            << "╭─ " << styled("✓ Decision accepted", green, color)
            << " ─────────────────────────────────────────────\n"
            << "│ " << styled("round", dim, color) << ' ' << result.round
            << "    " << styled("id", dim, color) << ' ' << result.id << '\n'
            << "│ " << styled(result.transition, magenta, color) << "    "
            << result.from_state << " ──▶ " << result.to_state << '\n'
            << "├─ " << styled("Δ state", cyan, color) << '\n';

  if (result.optimized_score) {
    std::cout << "│ " << styled("optimized", dim, color) << ' '
              << dtessl::value_text(*result.optimized_score) << " @ "
              << result.optimization_scope << '\n';
  }

  bool changed = false;
  for (const auto& [name, after] : result.state) {
    const auto found = before.find(name);
    if (found != before.end() && found->second == after) continue;
    changed = true;
    std::cout << "│  " << styled(name, cyan, color) << "  "
              << (found == before.end() ? "∅" : dtessl::value_text(found->second))
              << "  ──▶  " << styled(dtessl::value_text(after), green, color) << '\n';
  }
  for (const auto& [name, value] : before) {
    if (result.state.contains(name)) continue;
    changed = true;
    std::cout << "│  " << styled(name, cyan, color) << "  "
              << dtessl::value_text(value) << "  ──▶  ∅\n";
  }
  if (!changed) std::cout << "│  " << styled("no logical fields changed", dim, color) << '\n';

  std::cout << "├─ " << styled("access", yellow, color) << '\n'
            << "│  " << styled("reads ", dim, color) << field_set_text(result.reads) << '\n'
            << "│  " << styled("writes", dim, color) << ' ' << field_set_text(result.writes)
            << '\n'
            << "│  " << styled("after ", dim, color)
            << ' ' << field_set_text(result.causal_predecessors) << '\n'
            << "├─ " << styled("ActionPlan", magenta, color) << '\n';

  if (result.actions.calls.empty()) {
    std::cout << "│  " << styled("no external calls", dim, color) << '\n';
  } else {
    for (std::size_t index = 0; index < result.actions.calls.size(); ++index) {
      const dtessl::ActionCall& call = result.actions.calls[index];
      std::cout << "│  " << styled("[" + std::to_string(index) + "]", dim, color) << ' '
                << styled(call.label, yellow, color) << "  "
                << styled(action_text(call), cyan, color) << '\n';
    }
    if (!result.actions.dependencies.empty()) {
      std::cout << "│  " << styled("requires", dim, color) << '\n';
      for (const auto& [before_call, after_call] : result.actions.dependencies) {
        std::cout << "│    " << result.actions.calls[before_call].label << " ──▶ "
                  << result.actions.calls[after_call].label << '\n';
      }
    }
  }
  std::cout << "╰──────────────────────────────────────────────────────────\n\n";
}

std::string event_text(const dtessl::Event& event) {
  std::string result = event.name + "(";
  bool first = true;
  for (const auto& [name, value] : event.fields) {
    if (!first) result += ", ";
    first = false;
    result += name + "=" + dtessl::value_text(value);
  }
  return result + ")";
}

std::map<std::string, dtessl::Value, std::less<>> qualified_runtime_state(
    const std::map<std::string, dtessl::Value, std::less<>>& state) {
  const bool has_qualified = std::any_of(
      state.begin(), state.end(), [](const auto& item) {
        return item.first.find('.') != std::string::npos;
      });
  if (!has_qualified) return state;
  std::map<std::string, dtessl::Value, std::less<>> result;
  for (const auto& [name, value] : state) {
    if (name.find('.') != std::string::npos) result.emplace(name, value);
  }
  return result;
}

std::map<std::string, dtessl::Value, std::less<>> snapshot_runtime_state(
    const dtessl::TraceSnapshot& snapshot) {
  std::map<std::string, dtessl::Value, std::less<>> result;
  for (const auto& [procedure, state] : snapshot.procedure_states) {
    for (const auto& [field, value] : state) {
      result.emplace(procedure + "." + field, value);
    }
  }
  return result;
}

void print_runtime_result(
    const std::map<std::string, dtessl::Value, std::less<>>& raw_before,
    const dtessl::ParallelStepResult& result,
    const std::vector<std::pair<std::string, dtessl::TransitionInput>>& injections,
    bool color) {
  static constexpr std::string_view green = "\033[1;32m";
  static constexpr std::string_view cyan = "\033[1;36m";
  static constexpr std::string_view magenta = "\033[1;35m";
  static constexpr std::string_view yellow = "\033[1;33m";
  static constexpr std::string_view dim = "\033[0;90m";
  const auto before = qualified_runtime_state(raw_before);
  const auto after = qualified_runtime_state(result.state);

  std::cout << '\n' << "╭─ "
            << styled(result.transitions.empty() ? "○ Context admitted; quiescent"
                                                  : "✓ Procedure round committed",
                      result.transitions.empty() ? yellow : green, color)
            << " ───────────────────────────────────\n"
            << "│ " << styled("RoundId", dim, color) << ' ' << result.round << '\n';
  for (const auto& [procedure, transition] : injections) {
    std::cout << "│ " << styled(procedure, magenta, color) << "  ←  "
              << styled(event_text(dtessl::Event{transition.transition,
                                                transition.fields}),
                        cyan, color)
              << '\n';
  }

  std::cout << "├─ " << styled("derived decisions", magenta, color) << '\n';
  if (result.transitions.empty()) {
    std::cout << "│  " << styled("no transition enabled", dim, color) << '\n';
  } else {
    for (const dtessl::StepResult& step : result.transitions) {
      std::cout << "│  " << styled(step.procedure, yellow, color)
                << " rev=" << step.procedure_revision << "  "
                << styled(step.transition, magenta, color) << "  "
                << step.from_state << " ──▶ " << step.to_state << "  "
                << styled(step.id, dim, color) << '\n';
    }
  }

  std::cout << "├─ " << styled("Δ runtime state", cyan, color) << '\n';
  bool changed = false;
  for (const auto& [name, value] : after) {
    const auto found = before.find(name);
    if (found != before.end() && found->second == value) continue;
    changed = true;
    std::cout << "│  " << styled(name, cyan, color) << "  "
              << (found == before.end() ? "∅" : dtessl::value_text(found->second))
              << "  ──▶  " << styled(dtessl::value_text(value), green, color) << '\n';
  }
  if (!changed) std::cout << "│  " << styled("no logical fields changed", dim, color) << '\n';

  std::cout << "├─ " << styled("ActionPlan", magenta, color) << '\n';
  bool has_actions = false;
  for (const dtessl::StepResult& step : result.transitions) {
    for (const dtessl::ActionCall& call : step.actions.calls) {
      has_actions = true;
      std::cout << "│  " << styled(step.procedure, yellow, color) << "/"
                << styled(call.label, yellow, color) << "  "
                << styled(action_text(call), cyan, color) << '\n';
    }
    for (const auto& [before_call, after_call] : step.actions.dependencies) {
      std::cout << "│    " << step.actions.calls[before_call].label << " ──▶ "
                << step.actions.calls[after_call].label << '\n';
    }
  }
  if (!has_actions) std::cout << "│  " << styled("no external calls", dim, color) << '\n';
  std::cout << "╰──────────────────────────────────────────────────────────\n\n";
}

void print_runtime_snapshot(const dtessl::TraceSnapshot& snapshot,
                            std::string_view kind = "capture") {
  std::size_t decisions = 0;
  for (const dtessl::ParallelStepResult& round : snapshot.rounds) {
    decisions += round.transitions.size();
  }
  std::cout << kind << ' ' << snapshot.name
            << " closed replayable=" << (snapshot.replayable ? "yes" : "no")
            << " rounds=" << snapshot.rounds.size()
            << " decisions=" << decisions
            << " procedures=" << snapshot.procedure_artifacts.size() << '\n';
  for (const auto& [name, artifact] : snapshot.procedure_artifacts) {
    std::uint64_t revision = 0;
    const auto history = snapshot.procedure_history.find(name);
    if (history != snapshot.procedure_history.end() && !history->second.empty()) {
      revision = history->second.back().procedure_revision;
    }
    std::cout << "  " << name << " @ " << artifact.initial_context
              << " revision=" << revision
              << " injections=" << artifact.injections.size()
              << " initial-states=" << artifact.initial_states.size() << '\n';
  }
}

dtessl::SourcePosition position_at(std::string_view source, std::size_t offset) {
  if (offset > source.size()) throw dtessl::Error("editor offset is outside the document");
  dtessl::SourcePosition result{1, 1, offset};
  for (std::size_t index = 0; index < offset; ++index) {
    if (source[index] == '\n') {
      ++result.line;
      result.column = 1;
    } else {
      ++result.column;
    }
  }
  return result;
}

std::size_t line_start(std::string_view source, std::size_t line) {
  if (line == 0) throw dtessl::Error("line numbers start at 1");
  if (line == 1) return 0;
  std::size_t current = 1;
  for (std::size_t offset = 0; offset < source.size(); ++offset) {
    if (source[offset] == '\n' && ++current == line) return offset + 1;
  }
  if (current + 1 == line && !source.empty() && source.back() == '\n') return source.size();
  throw dtessl::Error("line is outside the document");
}

std::size_t line_end(std::string_view source, std::size_t line) {
  const std::size_t start = line_start(source, line);
  const std::size_t newline = source.find('\n', start);
  return newline == std::string_view::npos ? source.size() : newline + 1;
}

std::size_t parse_line_number(std::string_view text) {
  std::size_t value = 0;
  const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
  if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size() || value == 0) {
    throw dtessl::Error("expected a positive line number");
  }
  return value;
}

std::string read_editor_block(dtessl::cli::LineEditor& editor, bool interactive) {
  std::string result;
  if (interactive) std::cout << "enter source; a single . ends the block\n";
  while (true) {
    const std::optional<std::string> line = editor.read("... ");
    if (!line) throw dtessl::Error("end of input inside editor block");
    if (*line == ".") break;
    result += *line;
    result.push_back('\n');
  }
  return result;
}

void print_numbered_source(std::string_view source,
                           const dtessl::LanguageAnalysis& analysis, bool color) {
  std::istringstream lines(highlighted_source(source, analysis.highlights, color));
  std::string line;
  std::size_t number = 1;
  while (std::getline(lines, line)) {
    std::cout << std::setw(4) << number++ << " | " << line << '\n';
  }
  if (source.empty()) std::cout << "   1 |\n";
}

void repl_help() {
  std::cout
      << "editing:\n"
      << "  ↑/↓ history   ←/→ move   Home/End   Delete/Backspace\n"
      << "  Ctrl-A/E start/end   Ctrl-U/K erase   Ctrl-W word   Ctrl-L clear\n\n"
      << "commands:\n"
      << "  :show                    show the editable source\n"
      << "  :append                  append a multi-line block (finish with .)\n"
      << "  :insert LINE             insert a block before LINE\n"
      << "  :replace FIRST LAST      replace inclusive lines with a block\n"
      << "  :delete FIRST LAST       delete inclusive lines\n"
      << "  :undo | :redo            navigate edit history\n"
      << "  :check | :highlight      parse/verify or list semantic highlight spans\n"
      << "  :load PATH | :write [PATH]\n"
      << "  :run EVENT [name=value]  execute against the current logical state\n"
      << "  :traces | :trace NAME    list or execute a declared source trace\n"
      << "  :trace-live NAME [close] inspect a legacy Engine dynamic capture\n"
      << "  :procedures              list declared procedure instances\n"
      << "  :start PROCEDURE         start one persistent procedure instance\n"
      << "  :inject P TRANSITION [...] inject typed transition; -- shares RoundId\n"
      << "  :runtime                 inspect the live procedure RuntimeContext\n"
      << "  :capture [NAME]          close the live runtime into replay artifacts\n"
      << "  :replay-procedures [P]   replay live artifacts and re-derive decisions\n"
      << "  :claims NAME             evaluate a declared source trace\n"
      << "  :claims-live NAME [close] evaluate a legacy Engine capture\n"
      << "  :reset                   rebuild legacy and procedure runtimes\n"
      << "  :history                 show entered commands\n"
      << "  :quit                    exit\n"
      << "\ncore v0.4.0 syntax:\n"
      << "  procedure/inject          persistent automaton + typed transition input\n"
      << "  replay/capture closed     transition replay + complete procedure closure\n"
      << "  name T / T(atom)         nominal logical names\n"
      << "  relation T / (A,B)       finite relation schemas\n"
      << "  subject ~ R1,(R2|R3)     recursive relation satisfaction\n"
      << "  (a,b) ~ happens_before   trace-domain relation match\n"
      << "  [T] / [] / [value]       option type, absent and present values\n"
      << "  list[...]                explicit ordered-list literal\n"
      << "\nexample:\n"
      << "  :inject SessionA Increment delta=1 -- SessionB Increment delta=2\n"
      << "  :capture InterleavedRuntime\n"
      << "  :replay-procedures\n"
      << "A non-command line is appended as one source line.\n";
}

int repl(std::optional<std::string> initial_path) {
  const bool interactive = stdout_is_terminal();
  const bool color = interactive && std::getenv("NO_COLOR") == nullptr;
  std::string path = initial_path.value_or("");
  std::string initial = initial_path ? read_file(*initial_path) : std::string{};
  dtessl::LanguageDocument document(path.empty() ? "repl://buffer" : path, initial, 1);
  std::vector<std::string> undo;
  std::vector<std::string> redo;
  std::optional<dtessl::Engine> engine;
  std::optional<dtessl::RuntimeContext> runtime;
  std::set<std::string, std::less<>> started_procedures;
  dtessl::cli::LineEditor editor(interactive);

  const auto replace_document = [&](std::string replacement, bool remember) {
    if (remember) {
      undo.push_back(document.source());
      redo.clear();
    }
    const std::string before = document.source();
    const dtessl::SourceRange all{position_at(before, 0), position_at(before, before.size())};
    document.apply_edits({{all, std::move(replacement)}}, document.version() + 1);
    engine.reset();
    runtime.reset();
    started_procedures.clear();
  };
  const auto current_analysis = [&]() {
    return dtessl::analyze_source(
        document.source(), path.empty() ? "repl://buffer" : path, document.version());
  };
  const auto ensure_runtime = [&]() -> dtessl::RuntimeContext& {
    if (!runtime) runtime.emplace(dtessl::parse(document.source()));
    return *runtime;
  };
  const auto ensure_started = [&](const std::string& procedure, bool announce) {
    if (started_procedures.contains(procedure)) return;
    dtessl::RuntimeContext& active = ensure_runtime();
    active.start(procedure);
    started_procedures.insert(procedure);
    if (announce) {
      const dtessl::ProcedureArtifact artifact = active.artifact(procedure);
      std::cout << "started " << procedure << " @ " << artifact.initial_context
                << " initial-states=" << artifact.initial_states.size() << '\n';
    }
  };

  if (interactive) {
    std::cout << "DTESSL language workbench v" << dtessl::version
              << " — :help for commands\n";
  }
  while (true) {
    const std::optional<std::string> input = editor.read("dtessl> ");
    if (!input) break;
    const std::string& line = *input;
    if (line.empty()) continue;
    editor.add_history(line);
    try {
      if (line.front() != ':') {
        std::string updated = document.source();
        if (!updated.empty() && updated.back() != '\n') updated.push_back('\n');
        updated += line;
        updated.push_back('\n');
        replace_document(std::move(updated), true);
        continue;
      }
      const std::vector<std::string> words = split_words(line);
      const std::string& command = words.front();
      if (command == ":quit" || command == ":q") break;
      if (command == ":help") {
        repl_help();
      } else if (command == ":show") {
        print_numbered_source(document.source(), current_analysis(), color);
      } else if (command == ":check") {
        print_diagnostics(current_analysis(), std::cout);
      } else if (command == ":highlight") {
        print_highlights(document.source(), current_analysis(), std::cout);
      } else if (command == ":append") {
        std::string updated = document.source();
        if (!updated.empty() && updated.back() != '\n') updated.push_back('\n');
        updated += read_editor_block(editor, interactive);
        replace_document(std::move(updated), true);
      } else if (command == ":insert") {
        if (words.size() != 2) throw dtessl::Error("usage: :insert LINE");
        const std::size_t offset = line_start(document.source(), parse_line_number(words[1]));
        std::string updated = document.source();
        updated.insert(offset, read_editor_block(editor, interactive));
        replace_document(std::move(updated), true);
      } else if (command == ":replace" || command == ":delete") {
        if (words.size() != 3) {
          throw dtessl::Error("usage: " + command + " FIRST LAST");
        }
        const std::size_t first = parse_line_number(words[1]);
        const std::size_t last = parse_line_number(words[2]);
        if (last < first) throw dtessl::Error("LAST must not precede FIRST");
        const std::size_t begin = line_start(document.source(), first);
        const std::size_t end = line_end(document.source(), last);
        std::string updated = document.source();
        const std::string replacement = command == ":replace"
                                            ? read_editor_block(editor, interactive)
                                            : std::string{};
        updated.replace(begin, end - begin, replacement);
        replace_document(std::move(updated), true);
      } else if (command == ":undo") {
        if (undo.empty()) throw dtessl::Error("nothing to undo");
        redo.push_back(document.source());
        std::string restored = std::move(undo.back());
        undo.pop_back();
        replace_document(std::move(restored), false);
      } else if (command == ":redo") {
        if (redo.empty()) throw dtessl::Error("nothing to redo");
        undo.push_back(document.source());
        std::string restored = std::move(redo.back());
        redo.pop_back();
        replace_document(std::move(restored), false);
      } else if (command == ":load") {
        if (words.size() != 2) throw dtessl::Error("usage: :load PATH");
        path = words[1];
        replace_document(read_file(path), true);
      } else if (command == ":write") {
        const std::string destination = words.size() == 2 ? words[1] : path;
        if (destination.empty()) throw dtessl::Error("usage: :write PATH");
        if (words.size() > 2) throw dtessl::Error("usage: :write [PATH]");
        std::ofstream output(destination, std::ios::binary | std::ios::trunc);
        if (!output) throw dtessl::Error("cannot open '" + destination + "' for writing");
        output << document.source();
        if (!output) throw dtessl::Error("cannot write '" + destination + "'");
        path = destination;
        std::cout << "wrote " << destination << '\n';
      } else if (command == ":reset") {
        const dtessl::Program program = dtessl::parse(document.source());
        engine.emplace(program);
        runtime.emplace(program);
        started_procedures.clear();
        std::cout << "legacy engine and procedure runtime reset\n";
      } else if (command == ":run") {
        if (!engine) engine.emplace(dtessl::parse(document.source()));
        const auto before = engine->values();
        const dtessl::StepResult result = engine->step(parse_event(words, 1));
        print_repl_result(before, result, color);
      } else if (command == ":start") {
        if (words.size() != 2) throw dtessl::Error("usage: :start PROCEDURE");
        ensure_started(words[1], true);
      } else if (command == ":inject") {
        if (words.size() < 3) {
          throw dtessl::Error(
              "usage: :inject PROCEDURE EVENT [name=value ...] [-- PROCEDURE EVENT ...]");
        }
        std::vector<std::pair<std::string, dtessl::TransitionInput>> injections;
        std::size_t cursor = 1;
        while (cursor < words.size()) {
          const std::string procedure = words[cursor++];
          const std::size_t event_start = cursor;
          while (cursor < words.size() && words[cursor] != "--") ++cursor;
          if (event_start == cursor) throw dtessl::Error("missing injected event");
          ensure_started(procedure, true);
          dtessl::Event parsed = parse_event(words, event_start, cursor);
          injections.emplace_back(
              procedure,
              dtessl::TransitionInput{std::move(parsed.name),
                                      std::move(parsed.fields)});
          if (cursor < words.size()) {
            ++cursor;
            if (cursor == words.size()) {
              throw dtessl::Error("injection separator needs a following procedure");
            }
          }
        }
        dtessl::RuntimeContext& active = ensure_runtime();
        const auto before =
            snapshot_runtime_state(active.snapshot("before-injection"));
        const dtessl::ParallelStepResult result = active.inject(injections);
        print_runtime_result(before, result, injections, color);
      } else if (command == ":runtime") {
        if (!runtime || started_procedures.empty()) {
          throw dtessl::Error("no procedure runtime is active");
        }
        print_runtime_snapshot(runtime->snapshot("live-runtime"), "runtime");
      } else if (command == ":capture") {
        if (words.size() > 2) throw dtessl::Error("usage: :capture [NAME]");
        if (!runtime || started_procedures.empty()) {
          throw dtessl::Error("no procedure runtime is active");
        }
        print_runtime_snapshot(runtime->snapshot(
            words.size() == 2 ? words[1] : "repl-capture"));
      } else if (command == ":replay-procedures") {
        if (!runtime || started_procedures.empty()) {
          throw dtessl::Error("no procedure runtime is active");
        }
        std::vector<std::string> names;
        if (words.size() == 1) {
          names.assign(started_procedures.begin(), started_procedures.end());
        } else {
          names.assign(words.begin() + 1, words.end());
        }
        std::vector<dtessl::ProcedureArtifact> artifacts;
        artifacts.reserve(names.size());
        for (const std::string& name : names) artifacts.push_back(runtime->artifact(name));
        const dtessl::TraceSnapshot replayed = dtessl::replay_procedures(
            dtessl::parse(document.source()), artifacts);
        std::cout << "procedure replay ok\n";
        print_runtime_snapshot(replayed, "replay");
      } else if (command == ":traces") {
        const dtessl::Program program = dtessl::parse(document.source());
        for (const std::string& name : dtessl::declared_traces(program)) {
          std::cout << name << '\n';
        }
      } else if (command == ":procedures") {
        const dtessl::Program program = dtessl::parse(document.source());
        for (const std::string& name : dtessl::declared_procedures(program)) {
          std::cout << name
                    << (started_procedures.contains(name) ? " [running]" : "") << '\n';
        }
      } else if (command == ":trace") {
        if (words.size() != 2) throw dtessl::Error("usage: :trace NAME");
        const dtessl::TraceSnapshot trace =
            dtessl::run_named_trace(dtessl::parse(document.source()), words[1]);
        print_runtime_snapshot(trace, "trace");
      } else if (command == ":trace-live") {
        if (words.size() < 2 || words.size() > 3 ||
            (words.size() == 3 && words[2] != "close")) {
          throw dtessl::Error("usage: :trace-live NAME [close]");
        }
        if (!engine) engine.emplace(dtessl::parse(document.source()));
        print_runtime_snapshot(
            engine->captured_trace(words[1], words.size() == 3), "live-trace");
      } else if (command == ":claims") {
        if (words.size() != 2) throw dtessl::Error("usage: :claims NAME");
        for (const dtessl::ClaimEvaluation& claim :
             dtessl::evaluate_named_trace(dtessl::parse(document.source()), words[1])) {
          std::cout << claim.name << ' ' << dtessl::claim_status_name(claim.status)
                    << " round=" << claim.witness_round << " " << claim.detail << '\n';
        }
      } else if (command == ":claims-live") {
        if (words.size() < 2 || words.size() > 3 ||
            (words.size() == 3 && words[2] != "close")) {
          throw dtessl::Error("usage: :claims-live NAME [close]");
        }
        if (!engine) engine.emplace(dtessl::parse(document.source()));
        const bool close = words.size() == 3 && words[2] == "close";
        for (const dtessl::ClaimEvaluation& claim :
             engine->evaluate_claims(words[1], close)) {
          std::cout << claim.name << ' ' << dtessl::claim_status_name(claim.status)
                    << " round=" << claim.witness_round << " " << claim.detail << '\n';
        }
      } else if (command == ":history") {
        const auto& history = editor.history();
        for (std::size_t index = 0; index < history.size(); ++index) {
          std::cout << index + 1 << "  " << history[index] << '\n';
        }
      } else {
        throw dtessl::Error("unknown REPL command '" + command + "'");
      }
    } catch (const dtessl::Error& error) {
      std::cerr << "dtessl: " << error.what() << '\n';
    }
  }
  return 0;
}

std::vector<dtessl::Event> parse_event_bag(int argc, char** argv, int start) {
  std::vector<dtessl::Event> events;
  while (start < argc) {
    int end = start;
    while (end < argc && std::string_view(argv[end]) != "--") ++end;
    events.push_back(parse_event(argv, start, end));
    start = end + 1;
    if (end + 1 == argc) throw dtessl::Error("event separator needs a following event");
  }
  return events;
}

void usage(std::ostream& out) {
  out << "DTESSL (戴特赛尔) - Discrete-Time Event System Simulation Language\n\n"
      << "usage:\n"
      << "  dtessl version\n"
      << "  dtessl features <program.dtessl>\n"
      << "  dtessl plans <program.dtessl>\n"
      << "  dtessl bench <program.dtessl> <Transition> [iterations]\n"
      << "  dtessl verify-claim <program.dtessl> <Claim> [max-depth] [max-embeddings]\n"
      << "  dtessl descriptor-check <model.semantic>\n"
      << "  dtessl descriptor-generate <model.semantic>\n"
      << "  dtessl descriptor-source-map <model.semantic>\n"
      << "  dtessl descriptor-manifest <model.semantic>\n"
      << "  dtessl descriptor-run <model.semantic> <Event> [field=value ...]\n"
      << "  dtessl descriptor-replay <model.semantic> <Event> [field=value ...]\n"
      << "  dtessl repl [program.dtessl]\n"
      << "  dtessl highlight <program.dtessl>\n"
      << "  dtessl check <program.dtessl>\n"
      << "  dtessl traces <program.dtessl>\n"
      << "  dtessl procedures <program.dtessl>\n"
      << "  dtessl trace <program.dtessl> <Trace>\n"
      << "  dtessl claims <program.dtessl> <Trace>\n"
      << "  dtessl run <program.dtessl> <Event> [field=value ...]\n"
      << "  dtessl replay <program.dtessl> <Event> [field=value ...]\n"
      << "  dtessl run-batch <program.dtessl> <Event> [...] -- <Event> [...]\n"
      << "  dtessl replay-batch <program.dtessl> <Event> [...] -- <Event> [...]\n\n"
      << "core v0.4.0: recursive StateSchema/Embedding, recursive relations, temporal Product,\n"
      << "indexed search, causal rounds,\n"
      << "             native trace/Claim, name T, RelationMatch, [T], list[...]\n";
}

}  // namespace

int main(int argc, char** argv) {
  try {
    if (argc < 2 || std::string_view(argv[1]) == "help" || std::string_view(argv[1]) == "--help") {
      usage(std::cout);
      return argc < 2 ? 2 : 0;
    }
    const std::string command = argv[1];
    if (command == "version") {
      if (argc != 2) throw dtessl::Error("version does not accept arguments");
      std::cout << "dtessl v" << dtessl::version << '\n';
      return 0;
    }
    if (command == "repl") {
      if (argc > 3) throw dtessl::Error("usage: dtessl repl [program.dtessl]");
      return repl(argc == 3 ? std::optional<std::string>(argv[2]) : std::nullopt);
    }
    if (argc < 3) {
      usage(std::cerr);
      return 2;
    }
    if (command == "highlight") {
      if (argc != 3) throw dtessl::Error("highlight does not accept extra arguments");
      const std::string source_text = read_file(argv[2]);
      const dtessl::LanguageAnalysis analysis =
          dtessl::analyze_source(source_text, argv[2]);
      print_highlights(source_text, analysis, std::cout);
      if (!analysis.valid()) print_diagnostics(analysis, std::cerr);
      return analysis.valid() ? 0 : 1;
    }
    if (command.starts_with("descriptor-")) {
      const dtessl::SemanticDescriptor descriptor =
          dtessl::parse_semantic_descriptor(read_file(argv[2]));
      if (command == "descriptor-check") {
        if (argc != 3) throw dtessl::Error("descriptor-check does not accept event arguments");
        const auto checked = dtessl::check_semantic_descriptor(descriptor);
        std::cout << "ok descriptor=" << checked.coverage.descriptor_digest
                  << " source=" << checked.coverage.generated_source_digest << '\n';
        return 0;
      }
      if (command == "descriptor-generate") {
        if (argc != 3) throw dtessl::Error("descriptor-generate does not accept event arguments");
        std::cout << dtessl::generate_dtessl(descriptor).source;
        return 0;
      }
      if (command == "descriptor-source-map") {
        if (argc != 3) throw dtessl::Error("descriptor-source-map does not accept event arguments");
        const auto generated = dtessl::generate_dtessl(descriptor);
        std::cout << dtessl::print_source_map(descriptor, generated);
        return 0;
      }
      if (command == "descriptor-manifest") {
        if (argc != 3) throw dtessl::Error("descriptor-manifest does not accept event arguments");
        const auto checked = dtessl::check_semantic_descriptor(descriptor);
        std::cout << dtessl::print_semantic_coverage(checked.coverage);
        return 0;
      }
      if (command != "descriptor-run" && command != "descriptor-replay") {
        throw dtessl::Error("unknown descriptor command '" + command + "'");
      }
      const std::vector<dtessl::Event> events{parse_event(argv, 3, argc)};
      const auto result = command == "descriptor-replay"
                              ? dtessl::replay_semantic_descriptor(descriptor, events)
                              : dtessl::run_semantic_descriptor(descriptor, events);
      if (command == "descriptor-replay") std::cout << "replay ok\n";
      std::cout << "batch round " << result.round << " transitions "
                << result.transitions.size() << '\n';
      for (const auto& transition : result.transitions) {
        std::cout << dtessl::result_text(transition);
      }
      return 0;
    }
    const dtessl::Program program = dtessl::parse(read_file(argv[2]));
    if (command == "traces") {
      if (argc != 3) throw dtessl::Error("traces does not accept extra arguments");
      for (const std::string& name : dtessl::declared_traces(program)) std::cout << name << '\n';
      return 0;
    }
    if (command == "procedures") {
      if (argc != 3) throw dtessl::Error("procedures does not accept extra arguments");
      for (const std::string& name : dtessl::declared_procedures(program)) {
        std::cout << name << '\n';
      }
      return 0;
    }
    if (command == "trace") {
      if (argc != 4) throw dtessl::Error("usage: dtessl trace <program.dtessl> <Trace>");
      const dtessl::TraceSnapshot trace = dtessl::run_named_trace(program, argv[3]);
      std::cout << "trace " << trace.name << " closed rounds=" << trace.rounds.size()
                << " replayable=" << (trace.replayable ? "yes" : "no")
                << " procedures=" << trace.procedure_artifacts.size() << '\n';
      for (const dtessl::ParallelStepResult& round : trace.rounds) {
        for (const dtessl::StepResult& step : round.transitions) {
          std::cout << dtessl::result_text(step);
        }
      }
      return 0;
    }
    if (command == "claims") {
      if (argc != 4) throw dtessl::Error("usage: dtessl claims <program.dtessl> <Trace>");
      for (const dtessl::ClaimEvaluation& claim :
           dtessl::evaluate_named_trace(program, argv[3])) {
        std::cout << claim.name << ' ' << dtessl::claim_status_name(claim.status)
                  << " round=" << claim.witness_round << " " << claim.detail << '\n';
      }
      return 0;
    }
    if (command == "features") {
      if (argc != 3) throw dtessl::Error("features does not accept event arguments");
      for (const dtessl::LanguageFeature feature : dtessl::required_features(program)) {
        std::cout << dtessl::feature_name(feature) << '\n';
      }
      return 0;
    }
    if (command == "plans") {
      if (argc != 3) throw dtessl::Error("plans does not accept event arguments");
      for (const dtessl::SearchPlanSummary& plan : dtessl::search_plans(program)) {
        std::cout << plan.operation << " rows=" << plan.max_rows
                  << " work=" << plan.max_work
                  << " deterministic=" << (plan.deterministic ? "yes" : "no")
                  << " ambiguity=" << (plan.rejects_ambiguous_score ? "reject" : "n/a")
                  << '\n';
      }
      return 0;
    }
    if (command == "bench") {
      if (argc < 4 || argc > 5) {
        throw dtessl::Error(
            "usage: dtessl bench <program.dtessl> <Transition> [iterations]");
      }
      std::uint64_t iterations = 10000U;
      if (argc == 5) {
        const std::string_view text = argv[4];
        const auto parsed = std::from_chars(text.data(), text.data() + text.size(),
                                            iterations);
        if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size() ||
            iterations == 0U) {
          throw dtessl::Error("benchmark iterations must be a positive integer");
        }
      }
      struct Measurement {
        std::chrono::nanoseconds elapsed;
        std::map<std::string, std::string, std::less<>> states;
        std::map<std::string, dtessl::Value, std::less<>> values;
      };
      const auto measure = [&](dtessl::SolverEncoding encoding) {
        dtessl::Engine engine(program, encoding);
        const auto start = std::chrono::steady_clock::now();
        for (std::uint64_t iteration = 0; iteration < iterations; ++iteration) {
          static_cast<void>(engine.step_transition(
              dtessl::TransitionInput{argv[3], {}}));
        }
        const auto stop = std::chrono::steady_clock::now();
        return Measurement{std::chrono::duration_cast<std::chrono::nanoseconds>(
                               stop - start),
                           engine.current_states(), engine.values()};
      };
      const Measurement reference =
          measure(dtessl::SolverEncoding::ReferenceStrings);
      const Measurement dense = measure(dtessl::SolverEncoding::DenseIds);
      if (reference.states != dense.states || reference.values != dense.values) {
        throw dtessl::Error("dense encoding diverged from reference encoding");
      }
      const double reference_ns =
          static_cast<double>(reference.elapsed.count()) / static_cast<double>(iterations);
      const double dense_ns =
          static_cast<double>(dense.elapsed.count()) / static_cast<double>(iterations);
      std::cout << std::fixed << std::setprecision(2)
                << "iterations " << iterations << '\n'
                << "reference-string-map ns/step " << reference_ns << '\n'
                << "dense-id-table ns/step " << dense_ns << '\n'
                << "speedup " << (dense_ns == 0.0 ? 0.0 : reference_ns / dense_ns)
                << "x\n"
                << "semantic-parity yes\n";
      return 0;
    }
    if (command == "verify-claim") {
      if (argc < 4 || argc > 6) {
        throw dtessl::Error(
            "usage: dtessl verify-claim <program.dtessl> <Claim> [max-depth] [max-embeddings]");
      }
      dtessl::SolverLimits limits;
      const auto parse_limit = [&](int argument, std::size_t& target,
                                   std::string_view description) {
        if (argc <= argument) return;
        const std::string_view text = argv[argument];
        const auto parsed =
            std::from_chars(text.data(), text.data() + text.size(), target);
        if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size() ||
            target == 0U) {
          throw dtessl::Error(std::string(description) + " must be a positive integer");
        }
      };
      parse_limit(4, limits.max_depth, "max-depth");
      parse_limit(5, limits.max_embeddings, "max-embeddings");
      const dtessl::ClaimSolveResult verified =
          dtessl::Solver(program).verify_claim(argv[3], limits);
      std::cout << dtessl::claim_solve_status_name(verified.status)
                << " claim=" << verified.claim
                << " embeddings=" << verified.explored_embeddings
                << " product=" << verified.explored_product_states
                << " monitors=" << verified.claim_monitor_states
                << " edges=" << verified.explored_edges
                << " depth=" << verified.max_depth_reached << '\n'
                << verified.detail << '\n';
      for (const dtessl::CounterexampleFrame& frame : verified.counterexample) {
        std::cout << "counterexample depth=" << frame.depth
                  << " embedding=" << frame.embedding_digest;
        if (!frame.transition.empty()) std::cout << " via=" << frame.transition;
        std::cout << '\n';
      }
      return verified.status == dtessl::ClaimSolveStatus::Counterexample ? 3 : 0;
    }
    if (command == "check") {
      if (argc != 3) throw dtessl::Error("check does not accept event arguments");
      std::cout << "ok\n";
      return 0;
    }
    const bool batch = command == "run-batch" || command == "replay-batch";
    const bool replay = command == "replay" || command == "replay-batch";
    if (command != "run" && command != "replay" && !batch) {
      throw dtessl::Error("unknown command '" + command + "'");
    }
    const std::vector<dtessl::Event> events =
        batch ? parse_event_bag(argc, argv, 3)
              : std::vector<dtessl::Event>{parse_event(argv, 3, argc)};
    dtessl::Engine engine(program);
    const dtessl::ParallelStepResult result = engine.step_parallel(events);
    if (replay) {
      dtessl::Engine replay_engine(program);
      const dtessl::ParallelStepResult replayed = replay_engine.step_parallel(events);
      if (result != replayed) throw dtessl::Error("replay diverged");
      std::cout << "replay ok\n";
    }
    std::cout << "batch round " << result.round << " transitions "
              << result.transitions.size() << '\n';
    for (const dtessl::StepResult& transition : result.transitions) {
      std::cout << dtessl::result_text(transition);
    }
    return 0;
  } catch (const dtessl::Error& error) {
    std::cerr << "dtessl: ";
    if (error.line() != 0) {
      std::cerr << error.line() << ':' << error.column() << ": ";
    }
    std::cerr << error.what() << '\n';
    return 1;
  } catch (const std::exception& error) {
    std::cerr << "dtessl: " << error.what() << '\n';
    return 1;
  }
}
