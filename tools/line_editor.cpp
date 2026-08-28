#include "line_editor.hpp"

#include <cctype>
#include <iostream>

#if !defined(_WIN32)
#include <cerrno>
#include <termios.h>
#include <unistd.h>
#endif

namespace dtessl::cli {
namespace {

std::size_t previous_character(std::string_view text, std::size_t cursor) {
  if (cursor == 0) return 0;
  std::size_t result = cursor - 1;
  while (result != 0 &&
         (static_cast<unsigned char>(text[result]) & 0xc0U) == 0x80U) {
    --result;
  }
  return result;
}

std::size_t next_character(std::string_view text, std::size_t cursor) {
  if (cursor >= text.size()) return text.size();
  std::size_t result = cursor + 1;
  while (result < text.size() &&
         (static_cast<unsigned char>(text[result]) & 0xc0U) == 0x80U) {
    ++result;
  }
  return result;
}

#if !defined(_WIN32)

class RawTerminal {
 public:
  RawTerminal() {
    if (tcgetattr(STDIN_FILENO, &saved_) != 0) return;
    termios raw = saved_;
    raw.c_lflag &= static_cast<tcflag_t>(~(ICANON | ECHO | IEXTEN | ISIG));
    raw.c_iflag &= static_cast<tcflag_t>(~(IXON | ICRNL));
    raw.c_cc[VMIN] = 1;
    raw.c_cc[VTIME] = 0;
    active_ = tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) == 0;
  }

  ~RawTerminal() {
    if (active_) static_cast<void>(tcsetattr(STDIN_FILENO, TCSAFLUSH, &saved_));
  }

  [[nodiscard]] bool active() const noexcept { return active_; }

 private:
  termios saved_{};
  bool active_{false};
};

bool read_byte(char& value) {
  while (true) {
    const ssize_t count = ::read(STDIN_FILENO, &value, 1);
    if (count == 1) return true;
    if (count == 0) return false;
    if (errno != EINTR) return false;
  }
}

void redraw(std::string_view prompt, std::string_view buffer, std::size_t cursor) {
  std::cout << '\r' << prompt << buffer << "\033[K";
  const std::size_t suffix = buffer.size() - cursor;
  if (suffix != 0) std::cout << "\033[" << suffix << 'D';
  std::cout << std::flush;
}

#endif

}  // namespace

void LineEditor::add_history(std::string line) {
  if (line.empty()) return;
  if (!history_.empty() && history_.back() == line) return;
  history_.push_back(std::move(line));
}

std::optional<std::string> LineEditor::read(std::string_view prompt) {
  if (!interactive_) {
    std::string line;
    if (!std::getline(std::cin, line)) return std::nullopt;
    return line;
  }

#if defined(_WIN32)
  std::cout << prompt << std::flush;
  std::string line;
  if (!std::getline(std::cin, line)) return std::nullopt;
  return line;
#else
  RawTerminal terminal;
  if (!terminal.active()) {
    std::cout << prompt << std::flush;
    std::string line;
    if (!std::getline(std::cin, line)) return std::nullopt;
    return line;
  }

  std::string buffer;
  std::string draft;
  std::size_t cursor = 0;
  std::size_t history_index = history_.size();
  std::cout << prompt << std::flush;

  const auto move_history = [&](bool older) {
    if (history_.empty()) return;
    if (older) {
      if (history_index == history_.size()) draft = buffer;
      if (history_index != 0) --history_index;
    } else {
      if (history_index == history_.size()) return;
      ++history_index;
    }
    buffer = history_index == history_.size() ? draft : history_[history_index];
    cursor = buffer.size();
    redraw(prompt, buffer, cursor);
  };

  while (true) {
    char key = 0;
    if (!read_byte(key)) {
      std::cout << '\n';
      return std::nullopt;
    }
    const unsigned char code = static_cast<unsigned char>(key);
    if (key == '\r' || key == '\n') {
      std::cout << "\r\n";
      return buffer;
    }
    if (code == 3U) {  // Ctrl-C
      std::cout << "^C\r\n";
      return std::string{};
    }
    if (code == 4U) {  // Ctrl-D
      if (buffer.empty()) {
        std::cout << "\r\n";
        return std::nullopt;
      }
      if (cursor < buffer.size()) {
        buffer.erase(cursor, next_character(buffer, cursor) - cursor);
        redraw(prompt, buffer, cursor);
      }
      continue;
    }
    if (code == 1U) {  // Ctrl-A
      cursor = 0;
      redraw(prompt, buffer, cursor);
      continue;
    }
    if (code == 5U) {  // Ctrl-E
      cursor = buffer.size();
      redraw(prompt, buffer, cursor);
      continue;
    }
    if (code == 11U) {  // Ctrl-K
      buffer.erase(cursor);
      redraw(prompt, buffer, cursor);
      continue;
    }
    if (code == 21U) {  // Ctrl-U
      buffer.erase(0, cursor);
      cursor = 0;
      redraw(prompt, buffer, cursor);
      continue;
    }
    if (code == 23U) {  // Ctrl-W
      while (cursor != 0 &&
             std::isspace(static_cast<unsigned char>(buffer[cursor - 1])) != 0) {
        const std::size_t previous = previous_character(buffer, cursor);
        buffer.erase(previous, cursor - previous);
        cursor = previous;
      }
      while (cursor != 0 &&
             std::isspace(static_cast<unsigned char>(buffer[cursor - 1])) == 0) {
        const std::size_t previous = previous_character(buffer, cursor);
        buffer.erase(previous, cursor - previous);
        cursor = previous;
      }
      redraw(prompt, buffer, cursor);
      continue;
    }
    if (code == 12U) {  // Ctrl-L
      std::cout << "\033[2J\033[H";
      redraw(prompt, buffer, cursor);
      continue;
    }
    if (code == 16U) {  // Ctrl-P
      move_history(true);
      continue;
    }
    if (code == 14U) {  // Ctrl-N
      move_history(false);
      continue;
    }
    if (code == 127U || code == 8U) {
      if (cursor != 0) {
        const std::size_t previous = previous_character(buffer, cursor);
        buffer.erase(previous, cursor - previous);
        cursor = previous;
        redraw(prompt, buffer, cursor);
      }
      continue;
    }
    if (code == 27U) {
      char first = 0;
      char second = 0;
      if (!read_byte(first)) continue;
      if (first != '[' && first != 'O') continue;
      if (!read_byte(second)) continue;
      if (second == 'A') move_history(true);
      else if (second == 'B') move_history(false);
      else if (second == 'C' && cursor < buffer.size()) {
        cursor = next_character(buffer, cursor);
        redraw(prompt, buffer, cursor);
      } else if (second == 'D' && cursor != 0) {
        cursor = previous_character(buffer, cursor);
        redraw(prompt, buffer, cursor);
      } else if (second == 'H') {
        cursor = 0;
        redraw(prompt, buffer, cursor);
      } else if (second == 'F') {
        cursor = buffer.size();
        redraw(prompt, buffer, cursor);
      } else if (second == '3') {
        char terminator = 0;
        if (read_byte(terminator) && terminator == '~' && cursor < buffer.size()) {
          buffer.erase(cursor, next_character(buffer, cursor) - cursor);
          redraw(prompt, buffer, cursor);
        }
      } else if (second == '1' || second == '4' || second == '7' || second == '8') {
        char terminator = 0;
        if (read_byte(terminator) && terminator == '~') {
          cursor = second == '1' || second == '7' ? 0 : buffer.size();
          redraw(prompt, buffer, cursor);
        }
      }
      continue;
    }
    if (code >= 32U) {
      buffer.insert(cursor, 1, key);
      ++cursor;
      redraw(prompt, buffer, cursor);
    }
  }
#endif
}

}  // namespace dtessl::cli
