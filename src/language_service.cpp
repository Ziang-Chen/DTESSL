#include "dtessl/language_service.hpp"

#include "dtessl/dtessl.hpp"

#include <algorithm>
#include <utility>

namespace dtessl {

std::string_view syntax_class_name(SyntaxClass value) noexcept {
  switch (value) {
    case SyntaxClass::Keyword: return "keyword";
    case SyntaxClass::BuiltinType: return "type";
    case SyntaxClass::Identifier: return "identifier";
    case SyntaxClass::Number: return "number";
    case SyntaxClass::String: return "string";
    case SyntaxClass::Comment: return "comment";
    case SyntaxClass::Operator: return "operator";
    case SyntaxClass::Punctuation: return "punctuation";
  }
  return "identifier";
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

void validate_position(std::string_view source, const SourcePosition& position) {
  const SourcePosition canonical = position_at(source, position.offset);
  if (canonical.line != position.line || canonical.column != position.column) {
    throw Error("text edit line/column does not match its byte offset");
  }
}

}  // namespace

LanguageDocument::LanguageDocument(std::string uri, std::string source,
                                   std::uint64_t version)
    : uri_(std::move(uri)), source_(std::move(source)), version_(version) {
  if (uri_.empty()) throw Error("language document URI must not be empty");
}

LanguageAnalysis LanguageDocument::analyze() const {
  return analyze_source(source_, uri_, version_);
}

void LanguageDocument::apply_edits(std::vector<TextEdit> edits,
                                   std::uint64_t new_version) {
  if (new_version <= version_) throw Error("document version must increase");
  for (const TextEdit& edit : edits) {
    validate_position(source_, edit.range.start);
    validate_position(source_, edit.range.end);
    if (edit.range.start.offset > edit.range.end.offset) {
      throw Error("text edit range is reversed");
    }
  }
  std::sort(edits.begin(), edits.end(), [](const TextEdit& left, const TextEdit& right) {
    if (left.range.start.offset != right.range.start.offset) {
      return left.range.start.offset < right.range.start.offset;
    }
    return left.range.end.offset < right.range.end.offset;
  });
  for (std::size_t index = 1; index < edits.size(); ++index) {
    const TextEdit& previous = edits[index - 1];
    const TextEdit& current = edits[index];
    if (current.range.start.offset < previous.range.end.offset ||
        current.range.start.offset == previous.range.start.offset) {
      throw Error("text edits overlap or have an ambiguous shared start");
    }
  }
  for (auto edit = edits.rbegin(); edit != edits.rend(); ++edit) {
    source_.replace(edit->range.start.offset,
                    edit->range.end.offset - edit->range.start.offset,
                    edit->replacement);
  }
  version_ = new_version;
}

void LanguageService::open(std::string uri, std::string source,
                           std::uint64_t version) {
  if (documents_.contains(uri)) throw Error("language document is already open");
  const std::string key = uri;
  documents_.emplace(key, LanguageDocument(std::move(uri), std::move(source), version));
}

void LanguageService::change(std::string_view uri, std::vector<TextEdit> edits,
                             std::uint64_t new_version) {
  auto found = documents_.find(uri);
  if (found == documents_.end()) throw Error("language document is not open");
  found->second.apply_edits(std::move(edits), new_version);
}

void LanguageService::close(std::string_view uri) {
  if (documents_.erase(std::string(uri)) == 0) {
    throw Error("language document is not open");
  }
}

bool LanguageService::contains(std::string_view uri) const {
  return documents_.find(uri) != documents_.end();
}

const LanguageDocument& LanguageService::document(std::string_view uri) const {
  const auto found = documents_.find(uri);
  if (found == documents_.end()) throw Error("language document is not open");
  return found->second;
}

LanguageAnalysis LanguageService::analyze(std::string_view uri) const {
  return document(uri).analyze();
}

}  // namespace dtessl
