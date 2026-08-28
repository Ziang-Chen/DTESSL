#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace dtessl {

// Positions are one-based for line/column and zero-based for UTF-8 byte
// offsets. The explicit byte offset keeps edits deterministic; an LSP adapter
// can translate UTF-16 client positions at the protocol boundary.
struct SourcePosition {
  std::size_t line{1};
  std::size_t column{1};
  std::size_t offset{0};

  friend bool operator==(const SourcePosition&, const SourcePosition&) = default;
};

struct SourceRange {
  SourcePosition start;
  SourcePosition end;

  friend bool operator==(const SourceRange&, const SourceRange&) = default;
};

enum class SyntaxClass {
  Keyword,
  BuiltinType,
  Identifier,
  Number,
  String,
  Comment,
  Operator,
  Punctuation,
};

[[nodiscard]] std::string_view syntax_class_name(SyntaxClass value) noexcept;

struct HighlightToken {
  SyntaxClass syntax{SyntaxClass::Identifier};
  SourceRange range;

  friend bool operator==(const HighlightToken&, const HighlightToken&) = default;
};

enum class DiagnosticSeverity { Error, Warning, Information, Hint };

struct Diagnostic {
  std::string code;
  DiagnosticSeverity severity{DiagnosticSeverity::Error};
  std::string message;
  SourceRange range;

  friend bool operator==(const Diagnostic&, const Diagnostic&) = default;
};

struct LanguageAnalysis {
  std::string uri;
  std::uint64_t version{0};
  std::vector<HighlightToken> highlights;
  std::vector<Diagnostic> diagnostics;

  [[nodiscard]] bool valid() const noexcept { return diagnostics.empty(); }
};

// Runs the production lexer, parser and semantic verifier. Highlighting is
// therefore never based on a second, editor-only grammar.
[[nodiscard]] LanguageAnalysis analyze_source(std::string_view source,
                                              std::string uri = {},
                                              std::uint64_t version = 0);

struct TextEdit {
  SourceRange range;
  std::string replacement;
};

class LanguageDocument {
 public:
  LanguageDocument(std::string uri, std::string source, std::uint64_t version = 0);

  [[nodiscard]] const std::string& uri() const noexcept { return uri_; }
  [[nodiscard]] const std::string& source() const noexcept { return source_; }
  [[nodiscard]] std::uint64_t version() const noexcept { return version_; }
  [[nodiscard]] LanguageAnalysis analyze() const;

  // Edits refer to the same pre-edit snapshot, may be unordered, and must not
  // overlap. Versions must increase monotonically.
  void apply_edits(std::vector<TextEdit> edits, std::uint64_t new_version);

 private:
  std::string uri_;
  std::string source_;
  std::uint64_t version_{0};
};

// Small document store intended to be reused by a future LSP transport.
class LanguageService {
 public:
  void open(std::string uri, std::string source, std::uint64_t version = 0);
  void change(std::string_view uri, std::vector<TextEdit> edits,
              std::uint64_t new_version);
  void close(std::string_view uri);
  [[nodiscard]] bool contains(std::string_view uri) const;
  [[nodiscard]] const LanguageDocument& document(std::string_view uri) const;
  [[nodiscard]] LanguageAnalysis analyze(std::string_view uri) const;

 private:
  std::map<std::string, LanguageDocument, std::less<>> documents_;
};

}  // namespace dtessl
