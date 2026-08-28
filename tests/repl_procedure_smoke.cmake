if(NOT DEFINED CLI OR NOT DEFINED SOURCE)
  message(FATAL_ERROR "CLI and SOURCE are required")
endif()

set(input_file "${CMAKE_BINARY_DIR}/dtessl-repl-procedure-input.txt")
file(WRITE "${input_file}"
  ":check\n"
  ":start SessionA\n"
  ":start SessionB\n"
  ":inject SessionA Add delta=1 -- SessionB Add delta=2\n"
  ":inject SessionA Add delta=3\n"
  ":runtime\n"
  ":capture InterleavedRuntime\n"
  ":replay-procedures\n"
  ":trace Interleaved\n"
  ":claims Interleaved\n"
  ":quit\n")

execute_process(
  COMMAND "${CLI}" repl "${SOURCE}"
  INPUT_FILE "${input_file}"
  OUTPUT_VARIABLE output
  ERROR_VARIABLE errors
  RESULT_VARIABLE result)
file(REMOVE "${input_file}")

if(NOT result EQUAL 0)
  message(FATAL_ERROR "procedure REPL exited ${result}:\n${output}\n${errors}")
endif()

set(combined "${output}\n${errors}")
foreach(expected
    "ok"
    "started SessionA @ alpha"
    "started SessionB @ beta"
    "Procedure round committed"
    "SessionA.value"
    "SessionB.value"
    "runtime live-runtime closed replayable=yes rounds=2 decisions=3 procedures=2"
    "capture InterleavedRuntime closed replayable=yes rounds=2 decisions=3 procedures=2"
    "procedure replay ok"
    "replay procedure-replay closed replayable=yes rounds=2 decisions=3 procedures=2"
    "trace Interleaved closed replayable=yes rounds=2 decisions=3 procedures=2"
    "Persisted satisfied")
  string(FIND "${combined}" "${expected}" found)
  if(found EQUAL -1)
    message(FATAL_ERROR "missing '${expected}' in procedure REPL output:\n${combined}")
  endif()
endforeach()
