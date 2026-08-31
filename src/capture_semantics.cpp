// Occurrence-owned capture semantics. This file is included once by
// runtime.cpp after the private typed AST is available. State, transition and
// procedure filters are seed relations; procedure membership is never an
// ownership boundary for closure.

bool capture_path_matches(const StepResult& selected,
                          std::string_view seed) {
  return seed.find('.') == std::string_view::npos
             ? selected.transition_family == seed
             : selected.transition == seed;
}

bool capture_state_matches(
    const StepResult& step,
    const std::set<std::pair<std::string, std::string>>& seeds) {
  return std::any_of(
      seeds.begin(), seeds.end(), [&](const auto& seed) {
        const auto before = step.before_active_states.find(seed.first);
        const auto after = step.active_states.find(seed.first);
        return (before != step.before_active_states.end() &&
                before->second == seed.second) ||
               (after != step.active_states.end() &&
                after->second == seed.second);
      });
}

bool capture_relation_matches(const TraceSnapshot& trace,
                              const StepResult& step,
                              std::string_view procedure) {
  const bool has_seeds = !trace.captured_states.empty() ||
      !trace.captured_paths.empty() || !trace.captured_procedures.empty();
  if (!has_seeds) return true;
  const bool state = capture_state_matches(step, trace.captured_states);
  const bool path = std::any_of(
      trace.captured_paths.begin(), trace.captured_paths.end(),
      [&](const std::string& seed) {
        return capture_path_matches(step, seed);
      });
  const bool procedure_match = trace.captured_procedures.contains(
      step.procedure.empty() ? std::string(procedure) : step.procedure);
  return state || path || procedure_match;
}

bool capture_temporal_target_matches(const CaptureTemporalRule& rule,
                                     const StepResult& step) {
  if (rule.target_kind == CaptureTemporalTargetKind::Transition) {
    return capture_path_matches(step, rule.target);
  }
  const auto active = step.active_states.find(rule.context);
  return active != step.active_states.end() && active->second == rule.target;
}

struct CaptureSelection {
  std::set<std::string, std::less<>> occurrences;
  std::set<std::string, std::less<>> procedures;
  std::set<std::uint64_t> rounds;
  std::vector<CaptureInterval> intervals;
};

CaptureSelection select_capture_occurrences(
    const TraceSnapshot& seeds,
    const std::vector<ParallelStepResult>& rounds,
    bool include_causal_predecessors,
    bool stream_closed) {
  CaptureSelection result;
  std::vector<const StepResult*> ordered;
  for (const ParallelStepResult& round : rounds) {
    for (const StepResult& step : round.transitions) ordered.push_back(&step);
  }

  const bool has_seeds = !seeds.captured_states.empty() ||
      !seeds.captured_paths.empty() || !seeds.captured_procedures.empty();
  std::vector<const StepResult*> anchors;
  for (const StepResult* step : ordered) {
    if (!has_seeds || capture_relation_matches(seeds, *step, step->procedure)) {
      anchors.push_back(step);
      result.occurrences.insert(step->id);
    }
  }

  if (seeds.temporal_rule) {
    for (const StepResult* anchor : anchors) {
      CaptureInterval interval;
      interval.anchor_occurrence = anchor->id;
      interval.anchor_round = anchor->round;
      const StepResult* witness = nullptr;
      for (const StepResult* candidate : ordered) {
        if (candidate->round < anchor->round) continue;
        if (capture_temporal_target_matches(*seeds.temporal_rule, *candidate)) {
          witness = candidate;
          break;
        }
      }
      const std::uint64_t end_round = witness != nullptr
          ? witness->round
          : (rounds.empty() ? anchor->round : rounds.back().round);
      for (const StepResult* candidate : ordered) {
        if (candidate->round >= anchor->round &&
            candidate->round <= end_round) {
          result.occurrences.insert(candidate->id);
        }
      }
      if (witness != nullptr) {
        interval.witness_occurrence = witness->id;
        interval.witness_round = witness->round;
        interval.status = CaptureIntervalStatus::Witnessed;
      } else {
        interval.status = stream_closed ? CaptureIntervalStatus::Unresolved
                                        : CaptureIntervalStatus::Pending;
      }
      result.intervals.push_back(std::move(interval));
    }
  }

  if (include_causal_predecessors) {
    bool changed = true;
    while (changed) {
      changed = false;
      for (const StepResult* step : ordered) {
        if (!result.occurrences.contains(step->id)) continue;
        for (const std::string& predecessor : step->causal_predecessors) {
          changed = result.occurrences.insert(predecessor).second || changed;
        }
      }
    }
  }

  for (const StepResult* step : ordered) {
    if (!result.occurrences.contains(step->id)) continue;
    result.rounds.insert(step->round);
    if (!step->procedure.empty()) result.procedures.insert(step->procedure);
  }
  return result;
}

bool capture_procedure_selected(
    const std::set<std::string, std::less<>>& procedures,
    std::string_view procedure) {
  return procedures.contains(std::string(procedure));
}

bool capture_qualified_field_selected(
    const std::set<std::string, std::less<>>& procedures,
    std::string_view field) {
  return std::any_of(procedures.begin(), procedures.end(),
                     [&](const std::string& procedure) {
                       return field.starts_with(procedure + ".");
                     });
}
