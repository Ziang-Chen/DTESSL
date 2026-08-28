cmake_minimum_required(VERSION 3.20)

set(DTESSL_VERSION 0.2.0)

if(CMAKE_CURRENT_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
  project(dtessl VERSION ${DTESSL_VERSION} LANGUAGES CXX)
endif()

option(DTESSL_BUILD_TESTS "Build DTESSL tests" ON)

add_library(dtessl
  src/exact_numeric.cpp
  src/dtessl.cpp
  src/value_codec.cpp)
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

add_executable(dtessl_cli tools/dtessl.cpp)
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
  add_test(NAME dtessl.cli_search_plans
           COMMAND $<TARGET_FILE:dtessl_cli>
                   plans
                   ${CMAKE_CURRENT_SOURCE_DIR}/examples/relations.dtessl)
  set_tests_properties(dtessl.cli_search_plans PROPERTIES
                       PASS_REGULAR_EXPRESSION "select-by-lex.*ambiguity=reject")
endif()
