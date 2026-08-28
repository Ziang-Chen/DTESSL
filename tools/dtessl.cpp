#include "dtessl/dtessl.hpp"
#include "dtessl/version.hpp"

#include <charconv>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

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
      << "  dtessl check <program.dtessl>\n"
      << "  dtessl run <program.dtessl> <Event> [field=value ...]\n"
      << "  dtessl replay <program.dtessl> <Event> [field=value ...]\n"
      << "  dtessl run-batch <program.dtessl> <Event> [...] -- <Event> [...]\n"
      << "  dtessl replay-batch <program.dtessl> <Event> [...] -- <Event> [...]\n";
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
