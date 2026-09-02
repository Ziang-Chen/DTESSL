cmake_minimum_required(VERSION 3.20)

set(DTESSL_VERSION 0.4.7)

if(CMAKE_CURRENT_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
  project(dtessl VERSION ${DTESSL_VERSION} LANGUAGES CXX)
endif()

option(DTESSL_BUILD_TESTS "Build DTESSL tests" ON)

add_library(dtessl
  src/exact_numeric.cpp
  src/frontend.cpp
  src/frontend_ast.cpp
  src/semantics.cpp
  src/relation_semantics.cpp
  src/runtime.cpp
  src/capture_semantics.cpp
  src/trace_semantics.cpp
  src/monitor_semantics.cpp
  src/language_service.cpp
  src/semantic_descriptor.cpp
  src/embedding.cpp
  src/solver.cpp
  src/backend.cpp
  src/value_codec.cpp)
# The source AST remains private, so these implementation fragments are
# textually included once after the required private types exist. Listing the
# stability boundaries as header-only keeps them visible to IDEs without
# compiling duplicate implementation copies:
#   semantics        stable typed relations and Property dispositions
#   runtime          procedure/replay orchestration
#   capture_semantics occurrence/causal/temporal capture selection
#   trace_semantics  finite trace interpretation
#   monitor_semantics incremental temporal automata
#   solver           EmbeddingExpand x ClaimMonitor exploration
#   backend          feature/projection negotiation
set_source_files_properties(
  src/semantics.cpp
  src/frontend_ast.cpp
  src/relation_semantics.cpp
  src/runtime.cpp
  src/capture_semantics.cpp
  src/trace_semantics.cpp
  src/monitor_semantics.cpp
  src/solver.cpp
  src/backend.cpp
                            PROPERTIES HEADER_FILE_ONLY TRUE)
configure_file(include/dtessl/version.hpp.in
               generated/dtessl/version.hpp
               @ONLY)
target_include_directories(dtessl PUBLIC
  include
  ${CMAKE_CURRENT_BINARY_DIR}/generated)
target_compile_features(dtessl PUBLIC cxx_std_20)
set_target_properties(dtessl PROPERTIES CXX_EXTENSIONS OFF)
if(MSVC)
  target_compile_options(dtessl PRIVATE /W4 /permissive-)
else()
  target_compile_options(dtessl PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion)
endif()

add_executable(dtessl_cli
  tools/dtessl.cpp
  tools/line_editor.cpp)
target_link_libraries(dtessl_cli PRIVATE dtessl)
set_target_properties(dtessl_cli PROPERTIES OUTPUT_NAME dtessl)
install(TARGETS dtessl_cli RUNTIME DESTINATION bin)

if(DTESSL_BUILD_TESTS)
  enable_testing()
  add_executable(dtessl_tests tests/dtessl_test.cpp)
  target_link_libraries(dtessl_tests PRIVATE dtessl)
  add_test(NAME dtessl.minimum_language COMMAND dtessl_tests)
  add_test(NAME dtessl.cli_parallel_replay
           COMMAND $<TARGET_FILE:dtessl_cli>
                   replay-batch
                   ${CMAKE_CURRENT_SOURCE_DIR}/examples/scheduler.dtessl
                   Submit task=task-1 worker=worker-a
                   --
                   Note text=same-round)
  set_tests_properties(dtessl.cli_parallel_replay PROPERTIES
                       PASS_REGULAR_EXPRESSION "replay ok")
  add_test(NAME dtessl.cli_features
           COMMAND $<TARGET_FILE:dtessl_cli>
                   features
                   ${CMAKE_CURRENT_SOURCE_DIR}/examples/scheduler.dtessl)
  set_tests_properties(dtessl.cli_features PROPERTIES
                       PASS_REGULAR_EXPRESSION "parallel-event-bag")
  add_test(NAME dtessl.cli_highlight
           COMMAND $<TARGET_FILE:dtessl_cli>
                   highlight
                   ${CMAKE_CURRENT_SOURCE_DIR}/examples/scheduler.dtessl)
  set_tests_properties(dtessl.cli_highlight PROPERTIES
                       PASS_REGULAR_EXPRESSION "keyword state")
  add_test(NAME dtessl.cli_algebraic_replay
           COMMAND $<TARGET_FILE:dtessl_cli>
                   replay
                   ${CMAKE_CURRENT_SOURCE_DIR}/examples/algebraic.dtessl
                   Open id=session-1 owner=alice)
  set_tests_properties(dtessl.cli_algebraic_replay PROPERTIES
                       PASS_REGULAR_EXPRESSION "Decision.Accepted")
  add_test(NAME dtessl.cli_exact_numeric_replay
           COMMAND $<TARGET_FILE:dtessl_cli>
                   replay
                   ${CMAKE_CURRENT_SOURCE_DIR}/examples/exact_numeric.dtessl
                   Add delta=2)
  set_tests_properties(dtessl.cli_exact_numeric_replay PROPERTIES
                       PASS_REGULAR_EXPRESSION "18446744073709551617")
  add_test(NAME dtessl.cli_relation_replay
           COMMAND $<TARGET_FILE:dtessl_cli>
                   replay
                   ${CMAKE_CURRENT_SOURCE_DIR}/examples/relations.dtessl
                   Run minimum=1)
  set_tests_properties(dtessl.cli_relation_replay PROPERTIES
                       PASS_REGULAR_EXPRESSION "paths = relation")
  add_test(NAME dtessl.typed_relation_check
           COMMAND $<TARGET_FILE:dtessl_cli>
                   check
                   ${CMAKE_CURRENT_SOURCE_DIR}/examples/typed_relations.dtessl)
  set_tests_properties(dtessl.typed_relation_check PROPERTIES
                       PASS_REGULAR_EXPRESSION "ok")
  add_test(NAME dtessl.typed_relation_lazy_algebra
           COMMAND $<TARGET_FILE:dtessl_cli>
                   run
                   ${CMAKE_CURRENT_SOURCE_DIR}/examples/typed_relations.dtessl
                   Tick)
  set_tests_properties(dtessl.typed_relation_lazy_algebra PROPERTIES
                       PASS_REGULAR_EXPRESSION "derived = relation.*ID<int>\\(a\\).*ID<int>\\(b\\)")
  add_test(NAME dtessl.compact_pattern_transition
           COMMAND $<TARGET_FILE:dtessl_cli>
                   run
                   ${CMAKE_CURRENT_SOURCE_DIR}/examples/compact_pattern_transition.dtessl
                   Step)
  set_tests_properties(dtessl.compact_pattern_transition PROPERTIES
                       PASS_REGULAR_EXPRESSION "compact_b.sub_state: 0 -> 1")
  add_test(NAME dtessl.unordered_nested_pattern_transition
           COMMAND $<TARGET_FILE:dtessl_cli>
                   run
                   ${CMAKE_CURRENT_SOURCE_DIR}/examples/compact_pattern_transition.dtessl
                   Scatter)
  set_tests_properties(dtessl.unordered_nested_pattern_transition PROPERTIES
                       PASS_REGULAR_EXPRESSION "compact_b.sub_state: 0 -> 2")
  add_test(NAME dtessl.relation_properties_check
           COMMAND $<TARGET_FILE:dtessl_cli>
                   check
                   ${CMAKE_CURRENT_SOURCE_DIR}/examples/relation_properties.dtessl)
  set_tests_properties(dtessl.relation_properties_check PROPERTIES
                       PASS_REGULAR_EXPRESSION "ok")
  add_test(NAME dtessl.relation_properties_run
           COMMAND $<TARGET_FILE:dtessl_cli>
                   run
                   ${CMAKE_CURRENT_SOURCE_DIR}/examples/relation_properties.dtessl
                   Tick)
  set_tests_properties(dtessl.relation_properties_run PROPERTIES
                       PASS_REGULAR_EXPRESSION "value valid: false -> true")
  add_test(NAME dtessl.relation_claim_verified
           COMMAND $<TARGET_FILE:dtessl_cli>
                   verify-claim
                   ${CMAKE_CURRENT_SOURCE_DIR}/examples/relation_properties.dtessl
                   ReachIsTransitive)
  set_tests_properties(dtessl.relation_claim_verified PROPERTIES
                       PASS_REGULAR_EXPRESSION "verified claim=ReachIsTransitive")
  add_test(NAME dtessl.relation_claim_counterexample
           COMMAND $<TARGET_FILE:dtessl_cli>
                   verify-claim
                   ${CMAKE_CURRENT_SOURCE_DIR}/examples/relation_properties.dtessl
                   ReachIsAcyclic)
  set_tests_properties(dtessl.relation_claim_counterexample PROPERTIES
                       WILL_FAIL TRUE)
  add_test(NAME dtessl.relation_property_features
           COMMAND $<TARGET_FILE:dtessl_cli>
                   features
                   ${CMAKE_CURRENT_SOURCE_DIR}/examples/relation_properties.dtessl)
  set_tests_properties(dtessl.relation_property_features PROPERTIES
                       PASS_REGULAR_EXPRESSION "derived-relations.*relation-properties.*relation-claims.*higher-order-relations")
  add_test(NAME dtessl.transition_relation_check
           COMMAND $<TARGET_FILE:dtessl_cli>
                   check
                   ${CMAKE_CURRENT_SOURCE_DIR}/examples/transition_relations.dtessl)
  set_tests_properties(dtessl.transition_relation_check PROPERTIES
                       PASS_REGULAR_EXPRESSION "ok")
  add_test(NAME dtessl.transition_relation_run
           COMMAND $<TARGET_FILE:dtessl_cli>
                   run
                   ${CMAKE_CURRENT_SOURCE_DIR}/examples/transition_relations.dtessl
                   Dispatch)
  set_tests_properties(dtessl.transition_relation_run PROPERTIES
                       PASS_REGULAR_EXPRESSION "transition Dispatch.accept")
  add_test(NAME dtessl.transition_relation_replay
           COMMAND $<TARGET_FILE:dtessl_cli>
                   replay
                   ${CMAKE_CURRENT_SOURCE_DIR}/examples/transition_relations.dtessl
                   Dispatch)
  set_tests_properties(dtessl.transition_relation_replay PROPERTIES
                       PASS_REGULAR_EXPRESSION "replay ok")
  add_test(NAME dtessl.transition_relation_features
           COMMAND $<TARGET_FILE:dtessl_cli>
                   features
                   ${CMAKE_CURRENT_SOURCE_DIR}/examples/transition_relations.dtessl)
  set_tests_properties(dtessl.transition_relation_features PROPERTIES
                       PASS_REGULAR_EXPRESSION "transition-relations")
  add_test(NAME dtessl.transition_relation_plans
           COMMAND $<TARGET_FILE:dtessl_cli>
                   plans
                   ${CMAKE_CURRENT_SOURCE_DIR}/examples/transition_relations.dtessl)
  set_tests_properties(dtessl.transition_relation_plans PROPERTIES
                       PASS_REGULAR_EXPRESSION "transition-relation-union rows=2 work=2 deterministic=yes")
  add_test(NAME dtessl.cli_core_logic
           COMMAND $<TARGET_FILE:dtessl_cli>
                   replay
                   ${CMAKE_CURRENT_SOURCE_DIR}/examples/core_logic.dtessl
                   Submit minimum=2)
  set_tests_properties(dtessl.cli_core_logic PROPERTIES
                       PASS_REGULAR_EXPRESSION "chosen = .Worker.capacity: 2, id: WorkerId\\(a\\).")
  add_test(NAME dtessl.cli_core_check
           COMMAND $<TARGET_FILE:dtessl_cli>
                   check
                   ${CMAKE_CURRENT_SOURCE_DIR}/examples/core_logic.dtessl)
  set_tests_properties(dtessl.cli_core_check PROPERTIES
                       PASS_REGULAR_EXPRESSION "ok")
  add_test(NAME dtessl.cli_core_highlight_name
           COMMAND $<TARGET_FILE:dtessl_cli>
                   highlight
                   ${CMAKE_CURRENT_SOURCE_DIR}/examples/core_logic.dtessl)
  set_tests_properties(dtessl.cli_core_highlight_name PROPERTIES
                       PASS_REGULAR_EXPRESSION "keyword name")
  add_test(NAME dtessl.cli_core_highlight_relation
           COMMAND $<TARGET_FILE:dtessl_cli>
                   highlight
                   ${CMAKE_CURRENT_SOURCE_DIR}/examples/core_logic.dtessl)
  set_tests_properties(dtessl.cli_core_highlight_relation PROPERTIES
                       PASS_REGULAR_EXPRESSION "operator ~")
  add_test(NAME dtessl.cli_core_highlight_option
           COMMAND $<TARGET_FILE:dtessl_cli>
                   highlight
                   ${CMAKE_CURRENT_SOURCE_DIR}/examples/core_logic.dtessl)
  set_tests_properties(dtessl.cli_core_highlight_option PROPERTIES
                       PASS_REGULAR_EXPRESSION "punctuation \\[")
  add_test(NAME dtessl.cli_core_highlight_list
           COMMAND $<TARGET_FILE:dtessl_cli>
                   highlight
                   ${CMAKE_CURRENT_SOURCE_DIR}/examples/core_logic.dtessl)
  set_tests_properties(dtessl.cli_core_highlight_list PROPERTIES
                       PASS_REGULAR_EXPRESSION "type list")
  add_test(NAME dtessl.cli_search_plans
           COMMAND $<TARGET_FILE:dtessl_cli>
                   plans
                   ${CMAKE_CURRENT_SOURCE_DIR}/examples/relations.dtessl)
  set_tests_properties(dtessl.cli_search_plans PROPERTIES
                       PASS_REGULAR_EXPRESSION "select-by-lex.*ambiguity=reject")
  add_test(NAME dtessl.cli_transition_search_indexes
           COMMAND $<TARGET_FILE:dtessl_cli>
                   plans
                   ${CMAKE_CURRENT_SOURCE_DIR}/examples/composite_trace.dtessl)
  set_tests_properties(dtessl.cli_transition_search_indexes PROPERTIES
                       PASS_REGULAR_EXPRESSION "transition-id-index")
  add_test(NAME dtessl.semantic_descriptor_check
           COMMAND $<TARGET_FILE:dtessl_cli>
                   descriptor-check
                   ${CMAKE_CURRENT_SOURCE_DIR}/examples/scheduler.semantic)
  set_tests_properties(dtessl.semantic_descriptor_check PROPERTIES
                       PASS_REGULAR_EXPRESSION "ok descriptor=41810c5b91ae8df0978be5899613561b7daa73a8e888f2317b61b3ccc90a0053 source=8b7b34570f814a0f5d95b590b9468fc07c3fbf746625f6578290ba072846a761")
  add_test(NAME dtessl.semantic_descriptor_manifest
           COMMAND $<TARGET_FILE:dtessl_cli>
                   descriptor-manifest
                   ${CMAKE_CURRENT_SOURCE_DIR}/examples/scheduler.semantic)
  set_tests_properties(dtessl.semantic_descriptor_manifest PROPERTIES
                       PASS_REGULAR_EXPRESSION "independent-assurance-claim no")
  add_test(NAME dtessl.semantic_descriptor_replay
           COMMAND $<TARGET_FILE:dtessl_cli>
                   descriptor-replay
                   ${CMAKE_CURRENT_SOURCE_DIR}/examples/scheduler.semantic
                   Submit minimum=1 task=task-1)
  set_tests_properties(dtessl.semantic_descriptor_replay PROPERTIES
                       PASS_REGULAR_EXPRESSION "replay ok")
  add_test(NAME dtessl.composite_trace_check
           COMMAND $<TARGET_FILE:dtessl_cli>
                   check
                   ${CMAKE_CURRENT_SOURCE_DIR}/examples/composite_trace.dtessl)
  set_tests_properties(dtessl.composite_trace_check PROPERTIES
                       PASS_REGULAR_EXPRESSION "ok")
  add_test(NAME dtessl.composite_trace_run
           COMMAND $<TARGET_FILE:dtessl_cli>
                   trace
                   ${CMAKE_CURRENT_SOURCE_DIR}/examples/composite_trace.dtessl
                   Happy)
  set_tests_properties(dtessl.composite_trace_run PROPERTIES
                       PASS_REGULAR_EXPRESSION "ProcessRunning@process")
  add_test(NAME dtessl.composite_trace_replayable
           COMMAND $<TARGET_FILE:dtessl_cli>
                   trace
                   ${CMAKE_CURRENT_SOURCE_DIR}/examples/composite_trace.dtessl
                   Happy)
  set_tests_properties(dtessl.composite_trace_replayable PROPERTIES
                       PASS_REGULAR_EXPRESSION "replayable=yes occurrence-artifact-rounds=1 procedure-views=1")
  add_test(NAME dtessl.composite_procedures
           COMMAND $<TARGET_FILE:dtessl_cli>
                   procedures
                   ${CMAKE_CURRENT_SOURCE_DIR}/examples/composite_trace.dtessl)
  set_tests_properties(dtessl.composite_procedures PROPERTIES
                       PASS_REGULAR_EXPRESSION "DispatchRun")
  add_test(NAME dtessl.composite_trace_claims
           COMMAND $<TARGET_FILE:dtessl_cli>
                   claims
                   ${CMAKE_CURRENT_SOURCE_DIR}/examples/composite_trace.dtessl
                   Happy)
  set_tests_properties(dtessl.composite_trace_claims PROPERTIES
                       PASS_REGULAR_EXPRESSION "AtMostOneDispatch satisfied")
  add_test(NAME dtessl.optimized_transition_check
           COMMAND $<TARGET_FILE:dtessl_cli>
                   check
                   ${CMAKE_CURRENT_SOURCE_DIR}/examples/optimized_transition.dtessl)
  set_tests_properties(dtessl.optimized_transition_check PROPERTIES
                       PASS_REGULAR_EXPRESSION "ok")
  add_test(NAME dtessl.optimized_transition_replay
           COMMAND $<TARGET_FILE:dtessl_cli>
                   trace
                   ${CMAKE_CURRENT_SOURCE_DIR}/examples/optimized_transition.dtessl
                   Optimized)
  set_tests_properties(dtessl.optimized_transition_replay PROPERTIES
                       PASS_REGULAR_EXPRESSION "optimized_score 7 @ scheduler")
  add_test(NAME dtessl.procedure_replay_check
           COMMAND $<TARGET_FILE:dtessl_cli>
                   check
                   ${CMAKE_CURRENT_SOURCE_DIR}/examples/procedure_replay.dtessl)
  set_tests_properties(dtessl.procedure_replay_check PROPERTIES
                       PASS_REGULAR_EXPRESSION "ok")
  add_test(NAME dtessl.procedure_replay_trace
           COMMAND $<TARGET_FILE:dtessl_cli>
                   trace
                   ${CMAKE_CURRENT_SOURCE_DIR}/examples/procedure_replay.dtessl
                   Interleaved)
  set_tests_properties(dtessl.procedure_replay_trace PROPERTIES
                       PASS_REGULAR_EXPRESSION "replayable=yes occurrence-artifact-rounds=2 procedure-views=2")
  add_test(NAME dtessl.procedure_replay_claim
           COMMAND $<TARGET_FILE:dtessl_cli>
                   claims
                   ${CMAKE_CURRENT_SOURCE_DIR}/examples/procedure_replay.dtessl
                   Interleaved)
  set_tests_properties(dtessl.procedure_replay_claim PROPERTIES
                       PASS_REGULAR_EXPRESSION "Persisted satisfied")
  add_test(NAME dtessl.compact_automaton_check
           COMMAND $<TARGET_FILE:dtessl_cli>
                   check
                   ${CMAKE_CURRENT_SOURCE_DIR}/examples/compact_automaton.dtessl)
  set_tests_properties(dtessl.compact_automaton_check PROPERTIES
                       PASS_REGULAR_EXPRESSION "ok")
  add_test(NAME dtessl.compact_automaton_trace
           COMMAND $<TARGET_FILE:dtessl_cli>
                   trace
                   ${CMAKE_CURRENT_SOURCE_DIR}/examples/compact_automaton.dtessl
                   p1)
  set_tests_properties(dtessl.compact_automaton_trace PROPERTIES
                       PASS_REGULAR_EXPRESSION "transition ctodo.a_to_b")
  add_test(NAME dtessl.repl_procedure_runtime
           COMMAND ${CMAKE_COMMAND}
                   -DCLI=$<TARGET_FILE:dtessl_cli>
                   -DSOURCE=${CMAKE_CURRENT_SOURCE_DIR}/examples/procedure_replay.dtessl
                   -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/repl_procedure_smoke.cmake)
  add_test(NAME dtessl.solver_safety_counterexample
           COMMAND $<TARGET_FILE:dtessl_cli>
                   verify-claim
                   ${CMAKE_CURRENT_SOURCE_DIR}/examples/claims/safety_counterexample.dtessl
                   NoFailure)
  set_tests_properties(dtessl.solver_safety_counterexample PROPERTIES
                       WILL_FAIL TRUE)
  add_test(NAME dtessl.solver_safety_verified
           COMMAND $<TARGET_FILE:dtessl_cli>
                   verify-claim
                   ${CMAKE_CURRENT_SOURCE_DIR}/examples/claims/safety_verified.dtessl
                   NoFailure)
  set_tests_properties(dtessl.solver_safety_verified PROPERTIES
                       PASS_REGULAR_EXPRESSION "verified claim=NoFailure embeddings=2")
  add_test(NAME dtessl.solver_eventually_counterexample
           COMMAND $<TARGET_FILE:dtessl_cli>
                   verify-claim
                   ${CMAKE_CURRENT_SOURCE_DIR}/examples/claims/eventually_counterexample.dtessl
                   Completes)
  set_tests_properties(dtessl.solver_eventually_counterexample PROPERTIES
                       WILL_FAIL TRUE)
  add_test(NAME dtessl.solver_eventually_verified
           COMMAND $<TARGET_FILE:dtessl_cli>
                   verify-claim
                   ${CMAKE_CURRENT_SOURCE_DIR}/examples/claims/eventually_verified.dtessl
                   Completes)
  set_tests_properties(dtessl.solver_eventually_verified PROPERTIES
                       PASS_REGULAR_EXPRESSION "verified claim=Completes embeddings=2")
  add_test(NAME dtessl.temporal_trace_claims
           COMMAND $<TARGET_FILE:dtessl_cli>
                   claims
                   ${CMAKE_CURRENT_SOURCE_DIR}/examples/temporal_trace.dtessl
                   History)
  set_tests_properties(dtessl.temporal_trace_claims PROPERTIES
                       PASS_REGULAR_EXPRESSION "StartedSinceStart satisfied")
  add_test(NAME dtessl.temporal_procedure_product
           COMMAND $<TARGET_FILE:dtessl_cli>
                   verify-claim
                   ${CMAKE_CURRENT_SOURCE_DIR}/examples/temporal_procedure.dtessl
                   StartedSinceDone)
  set_tests_properties(dtessl.temporal_procedure_product PROPERTIES
                       PASS_REGULAR_EXPRESSION "verified claim=StartedSinceDone.*product=")
  add_test(NAME dtessl.temporal_features
           COMMAND $<TARGET_FILE:dtessl_cli>
                   features
                   ${CMAKE_CURRENT_SOURCE_DIR}/examples/temporal_procedure.dtessl)
  set_tests_properties(dtessl.temporal_features PROPERTIES
                       PASS_REGULAR_EXPRESSION "transition-obligation")
  add_test(NAME dtessl.recursive_embedding_check
           COMMAND $<TARGET_FILE:dtessl_cli>
                   check
                   ${CMAKE_CURRENT_SOURCE_DIR}/examples/recursive_embedding.dtessl)
  set_tests_properties(dtessl.recursive_embedding_check PROPERTIES
                       PASS_REGULAR_EXPRESSION "ok")
  add_test(NAME dtessl.recursive_embedding_compact
           COMMAND $<TARGET_FILE:dtessl_cli>
                   check
                   ${CMAKE_CURRENT_SOURCE_DIR}/examples/recursive_embedding_compact.dtessl)
  set_tests_properties(dtessl.recursive_embedding_compact PROPERTIES
                       PASS_REGULAR_EXPRESSION "ok")
  add_test(NAME dtessl.recursive_embedding_claim
           COMMAND $<TARGET_FILE:dtessl_cli>
                   verify-claim
                   ${CMAKE_CURRENT_SOURCE_DIR}/examples/recursive_embedding.dtessl
                   Safe)
  set_tests_properties(dtessl.recursive_embedding_claim PROPERTIES
                       PASS_REGULAR_EXPRESSION "verified claim=Safe embeddings=3")
  add_test(NAME dtessl.recursive_relation_features
           COMMAND $<TARGET_FILE:dtessl_cli>
                   features
                   ${CMAKE_CURRENT_SOURCE_DIR}/examples/recursive_embedding.dtessl)
  set_tests_properties(dtessl.recursive_relation_features PROPERTIES
                       PASS_REGULAR_EXPRESSION "recursive-state-schema.*relation-expression")
  add_test(NAME dtessl.state_case_finite_check
           COMMAND $<TARGET_FILE:dtessl_cli>
                   check
                   ${CMAKE_CURRENT_SOURCE_DIR}/examples/state_case_finite.dtessl)
  set_tests_properties(dtessl.state_case_finite_check PROPERTIES
                       PASS_REGULAR_EXPRESSION "ok")
  add_test(NAME dtessl.state_case_finite_solver
           COMMAND $<TARGET_FILE:dtessl_cli>
                   verify-claim
                   ${CMAKE_CURRENT_SOURCE_DIR}/examples/state_case_finite.dtessl
                   RetryBound)
  set_tests_properties(dtessl.state_case_finite_solver PROPERTIES
                       PASS_REGULAR_EXPRESSION "verified claim=RetryBound embeddings=9")
  add_test(NAME dtessl.occurrence_capture_check
           COMMAND $<TARGET_FILE:dtessl_cli>
                   check
                   ${CMAKE_CURRENT_SOURCE_DIR}/examples/occurrence_capture.dtessl)
  set_tests_properties(dtessl.occurrence_capture_check PROPERTIES
                       PASS_REGULAR_EXPRESSION "ok")
  add_test(NAME dtessl.occurrence_capture_trace
           COMMAND $<TARGET_FILE:dtessl_cli>
                   trace
                   ${CMAKE_CURRENT_SOURCE_DIR}/examples/occurrence_capture.dtessl
                   StartUntilDone)
  set_tests_properties(dtessl.occurrence_capture_trace PROPERTIES
                       PASS_REGULAR_EXPRESSION "closed rounds=3 replayable=yes occurrence-artifact-rounds=3 procedure-views=1;interval anchor=Flow/r1:0 round=1 witnessed=Flow/r3:0 round=3")
  add_test(NAME dtessl.repl_occurrence_artifact
           COMMAND ${CMAKE_COMMAND}
                   -DCLI=$<TARGET_FILE:dtessl_cli>
                   -DSOURCE=${CMAKE_CURRENT_SOURCE_DIR}/examples/occurrence_capture.dtessl
                   -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/repl_occurrence_smoke.cmake)
endif()
