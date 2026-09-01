if(NOT DEFINED CLI OR NOT DEFINED SOURCE)
  message(FATAL_ERROR "CLI and SOURCE are required")
endif()

set(input_file "${CMAKE_BINARY_DIR}/dtessl-repl-occurrence-input.txt")
file(WRITE "${input_file}"
  ":check\n"
  ":trace StartUntilDone\n"
  ":replay-artifact StartUntilDone\n"
  ":reset\n"
  ":step Start\n"
  ":step Progress\n"
  ":trace-live StartUntilDone\n"
  ":step Finish\n"
  ":step After\n"
  ":trace-live StartUntilDone close\n"
  ":replay-artifact StartUntilDone live\n"
  ":quit\n")

execute_process(
  COMMAND "${CLI}" repl "${SOURCE}"
  INPUT_FILE "${input_file}"
  OUTPUT_VARIABLE output
  ERROR_VARIABLE errors
  RESULT_VARIABLE result)
file(REMOVE "${input_file}")

if(NOT result EQUAL 0)
  message(FATAL_ERROR "occurrence REPL exited ${result}:\n${output}\n${errors}")
endif()

set(combined "${output}\n${errors}")
foreach(expected
    "ok"
    "trace StartUntilDone closed replayable=yes rounds=3 decisions=3 occurrence-artifact-rounds=3 procedure-views=1"
    "occurrence Flow/r1:0 round=1 input=transition Start() @ Flow/local"
    "interval Flow/r1:0 @ round 1 status=witnessed witness=Flow/r3:0 @ round 3"
    "artifact typed-prefix rounds=3"
    "trace artifact replay ok source=StartUntilDone"
    "input transition Start"
    "live-trace StartUntilDone open replayable=no rounds=2 decisions=2 occurrence-artifact-rounds=2 procedure-views=0"
    "interval r1:0 @ round 1 status=pending"
    "live-trace StartUntilDone closed replayable=yes rounds=3 decisions=3 occurrence-artifact-rounds=3 procedure-views=0"
    "occurrence r3:0 round=3 input=transition Finish()"
    "artifact-replay trace-artifact-replay closed replayable=yes rounds=3 decisions=3 occurrence-artifact-rounds=3 procedure-views=0")
  string(FIND "${combined}" "${expected}" found)
  if(found EQUAL -1)
    message(FATAL_ERROR "missing '${expected}' in occurrence REPL output:\n${combined}")
  endif()
endforeach()

string(FIND "${combined}" "occurrence r4:0" excluded_after)
if(NOT excluded_after EQUAL -1)
  message(FATAL_ERROR "temporal closure incorrectly retained After occurrence:\n${combined}")
endif()
