#include "dtessl/dtessl.hpp"

#include <charconv>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <string_view>

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
  std::int64_t integer = 0;
  const auto parsed = std::from_chars(text.data(), text.data() + text.size(), integer);
  if (parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size()) {
    return dtessl::Value(integer);
  }
  return dtessl::Value(std::string(text));
}

dtessl::Event parse_event(int argc, char** argv, int start) {
  if (start >= argc) throw dtessl::Error("missing event name");
  dtessl::Event event;
  event.name = argv[start];
  for (int index = start + 1; index < argc; ++index) {
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

void usage(std::ostream& out) {
  out << "DTESSL (戴特赛尔) - Discrete-Time Event System Simulation Language\n\n"
      << "usage:\n"
      << "  dtessl check <program.dtessl>\n"
      << "  dtessl run <program.dtessl> <Event> [field=value ...]\n"
      << "  dtessl replay <program.dtessl> <Event> [field=value ...]\n";
}

}  // namespace

int main(int argc, char** argv) {
  try {
    if (argc < 2 || std::string_view(argv[1]) == "help" || std::string_view(argv[1]) == "--help") {
      usage(std::cout);
      return argc < 2 ? 2 : 0;
    }
    const std::string command = argv[1];
    if (argc < 3) {
      usage(std::cerr);
      return 2;
    }
    const dtessl::Program program = dtessl::parse(read_file(argv[2]));
    if (command == "check") {
      if (argc != 3) throw dtessl::Error("check does not accept event arguments");
      std::cout << "ok\n";
      return 0;
    }
    if (command != "run" && command != "replay") {
      throw dtessl::Error("unknown command '" + command + "'");
    }
    const dtessl::Event event = parse_event(argc, argv, 3);
    dtessl::Engine engine(program);
    const dtessl::StepResult result = engine.step(event);
    if (command == "replay") {
      dtessl::Engine replay_engine(program);
      const dtessl::StepResult replayed = replay_engine.step(event);
      if (result != replayed) throw dtessl::Error("replay diverged");
      std::cout << "replay ok\n";
    }
    std::cout << dtessl::result_text(result);
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
