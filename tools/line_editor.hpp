#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace dtessl::cli {

// Dependency-free terminal editor for the DTESSL workbench. It owns command
// history and terminal interaction only; parsing/completion remain separate.
class LineEditor {
 public:
  explicit LineEditor(bool interactive) : interactive_(interactive) {}

  [[nodiscard]] std::optional<std::string> read(std::string_view prompt);
  void add_history(std::string line);

  [[nodiscard]] const std::vector<std::string>& history() const noexcept {
    return history_;
  }

 private:
  bool interactive_{false};
  std::vector<std::string> history_;
};

}  // namespace dtessl::cli
