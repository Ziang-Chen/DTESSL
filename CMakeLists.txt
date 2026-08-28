cmake_minimum_required(VERSION 3.20)

set(DTESSL_VERSION 0.0.1)

if(CMAKE_CURRENT_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
  project(dtessl VERSION ${DTESSL_VERSION} LANGUAGES CXX)
endif()

option(DTESSL_BUILD_TESTS "Build DTESSL tests" ON)

add_library(dtessl src/dtessl.cpp)
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
endif()
