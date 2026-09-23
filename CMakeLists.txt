cmake_minimum_required(VERSION 3.25)

project(fel
  VERSION 1.0.0
  DESCRIPTION "Fabric Efficiency Ledger - deterministic accounting of useful network work"
  HOMEPAGE_URL "https://github.com/summonlabs/Fabric-Efficiency-Ledger"
  LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

option(FEL_BUILD_SHARED "Build the runtime as a shared library" OFF)
option(FEL_BUILD_TESTS "Build the test suite" ON)
option(FEL_BUILD_TOOLS "Build the fel command line tool" ON)
option(FEL_BUILD_EXAMPLES "Build the examples" ON)
option(FEL_BUILD_BENCHMARKS "Build the benchmarks" ON)
option(FEL_WERROR "Treat first-party warnings as errors" ON)
option(FEL_ENABLE_ASAN "Enable AddressSanitizer if the toolchain provides it" OFF)

# ---------------------------------------------------------------------------
# Warning configuration. Strict on every supported compiler; the first-party
# warning count is required to be zero.
# ---------------------------------------------------------------------------
# Applied as a function rather than an exported INTERFACE target so that the
# installed package exports only fel::fel.
function(fel_apply_strict_warnings target)
  if(MSVC)
    target_compile_options(${target} PRIVATE
      /W4 /permissive- /utf-8 /Zc:__cplusplus /Zc:preprocessor /EHsc /MP)
    if(FEL_WERROR)
      target_compile_options(${target} PRIVATE /WX)
    endif()
    # fopen/fprintf and friends: this runtime deliberately uses the C stdio
    # surface because it needs fileno() based durability barriers.
    target_compile_definitions(${target} PRIVATE
      _CRT_SECURE_NO_WARNINGS NOMINMAX WIN32_LEAN_AND_MEAN)
  else()
    target_compile_options(${target} PRIVATE
      -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion
      -Wold-style-cast -Wcast-qual -Wdouble-promotion -Wformat=2 -Wundef
      -Wnon-virtual-dtor -Woverloaded-virtual -Wnull-dereference)
    if(FEL_WERROR)
      target_compile_options(${target} PRIVATE -Werror)
    endif()
  endif()

  if(FEL_ENABLE_ASAN)
    if(MSVC)
      target_compile_options(${target} PRIVATE /fsanitize=address)
      target_link_options(${target} PRIVATE /fsanitize=address)
    else()
      target_compile_options(${target} PRIVATE -fsanitize=address -fno-omit-frame-pointer)
      target_link_options(${target} PRIVATE -fsanitize=address)
    endif()
  endif()
endfunction()

# ---------------------------------------------------------------------------
# Runtime library
# ---------------------------------------------------------------------------
set(FEL_SOURCES
  src/error.cpp
  src/crypto.cpp
  src/time.cpp
  src/serialize.cpp
  src/measure.cpp
  src/observation.cpp
  src/claim.cpp
  src/topology.cpp
  src/topology_json.cpp
  src/policy.cpp
  src/freshness.cpp
  src/period.cpp
  src/aggregate.cpp
  src/provenance.cpp
  src/concurrency.cpp
  src/persistence.cpp
  src/io.cpp
  src/ledger_json.cpp
  src/ledger.cpp)

if(FEL_BUILD_SHARED)
  add_library(fel SHARED ${FEL_SOURCES})
  target_compile_definitions(fel PRIVATE FEL_BUILD_SHARED FEL_EXPORTS)
  target_compile_definitions(fel PUBLIC FEL_SHARED)
else()
  add_library(fel STATIC ${FEL_SOURCES})
endif()
add_library(fel::fel ALIAS fel)

target_include_directories(fel
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
  PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src)

fel_apply_strict_warnings(fel)

find_package(Threads REQUIRED)
target_link_libraries(fel PUBLIC Threads::Threads)

set_target_properties(fel PROPERTIES
  VERSION ${PROJECT_VERSION}
  SOVERSION ${PROJECT_VERSION_MAJOR}
  OUTPUT_NAME fel)

# ---------------------------------------------------------------------------
# Command line tooling
# ---------------------------------------------------------------------------
if(FEL_BUILD_TOOLS)
  add_executable(fel_cli
    tools/fel/main.cpp
    tools/fel/commands.cpp)
  set_target_properties(fel_cli PROPERTIES OUTPUT_NAME fel)
  
  target_link_libraries(fel_cli PRIVATE fel::fel)
  fel_apply_strict_warnings(fel_cli)
endif()

# ---------------------------------------------------------------------------
# Examples
# ---------------------------------------------------------------------------
if(FEL_BUILD_EXAMPLES)
  add_executable(fel_example_basic examples/basic_ledger.cpp)
  
  target_link_libraries(fel_example_basic PRIVATE fel::fel)
  fel_apply_strict_warnings(fel_example_basic)

  add_executable(fel_example_explain examples/explain_walkthrough.cpp)
  
  target_link_libraries(fel_example_explain PRIVATE fel::fel)
  fel_apply_strict_warnings(fel_example_explain)

  add_executable(fel_example_correction examples/correction_lineage.cpp)
  
  target_link_libraries(fel_example_correction PRIVATE fel::fel)
  fel_apply_strict_warnings(fel_example_correction)
endif()

# ---------------------------------------------------------------------------
# Benchmarks - every benchmark reports completed work, not just elapsed time.
# ---------------------------------------------------------------------------
if(FEL_BUILD_BENCHMARKS)
  add_executable(fel_bench_ingest benchmarks/bench_ingest.cpp)
  
  target_link_libraries(fel_bench_ingest PRIVATE fel::fel)
  fel_apply_strict_warnings(fel_bench_ingest)

  add_executable(fel_bench_close benchmarks/bench_close.cpp)
  
  target_link_libraries(fel_bench_close PRIVATE fel::fel)
  fel_apply_strict_warnings(fel_bench_close)

  add_executable(fel_bench_query benchmarks/bench_query.cpp)
  
  target_link_libraries(fel_bench_query PRIVATE fel::fel)
  fel_apply_strict_warnings(fel_bench_query)

  add_executable(fel_bench_persist benchmarks/bench_persist.cpp)
  
  target_link_libraries(fel_bench_persist PRIVATE fel::fel)
  fel_apply_strict_warnings(fel_bench_persist)
endif()

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------
if(FEL_BUILD_TESTS)
  enable_testing()

  add_library(fel_test_support STATIC
    tests/support/test_framework.cpp
    tests/support/fixtures.cpp)
  target_include_directories(fel_test_support PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/tests)
  
  target_link_libraries(fel_test_support PUBLIC fel::fel)
  fel_apply_strict_warnings(fel_test_support)

  add_executable(fel_tests
    tests/unit/test_ids_and_hashing.cpp
    tests/unit/test_checked_arithmetic.cpp
    tests/unit/test_serialize.cpp
    tests/unit/test_time.cpp
    tests/unit/test_concurrency_primitives.cpp
    tests/unit/test_topology.cpp
    tests/unit/test_policy.cpp
    tests/unit/test_freshness.cpp
    tests/unit/test_measure_and_claims.cpp
    tests/unit/test_persistence_format.cpp
    tests/integration/test_ledger_ingest_close.cpp
    tests/integration/test_conservation.cpp
    tests/integration/test_aggregation.cpp
    tests/integration/test_explain_export.cpp
    tests/integration/test_restart_recovery.cpp
    tests/integration/test_corrections_lineage.cpp
    tests/property/test_property_invariants.cpp
    tests/property/test_determinism.cpp
    tests/adversarial/test_adversarial_evidence.cpp
    tests/concurrency/test_concurrency_races.cpp
    tests/e2e/test_end_to_end.cpp)
  
  target_link_libraries(fel_tests PRIVATE fel_test_support)
  fel_apply_strict_warnings(fel_tests)

  add_test(NAME unit COMMAND fel_tests --suite unit)
  add_test(NAME integration COMMAND fel_tests --suite integration)
  add_test(NAME property COMMAND fel_tests --suite property)
  add_test(NAME adversarial COMMAND fel_tests --suite adversarial)
  add_test(NAME concurrency COMMAND fel_tests --suite concurrency)
  add_test(NAME e2e COMMAND fel_tests --suite e2e)
  add_test(NAME all COMMAND fel_tests)

  # Real independent process tests: the CLI is spawned as a separate process.
  add_executable(fel_process_tests tests/process/test_process_lock.cpp)
  fel_apply_strict_warnings(fel_process_tests)
  add_test(NAME process COMMAND fel_process_tests $<TARGET_FILE:fel_cli>)
endif()

# ---------------------------------------------------------------------------
# Install / export
# ---------------------------------------------------------------------------
install(TARGETS fel
  EXPORT fel-targets
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
  LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
  ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
  INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

install(DIRECTORY include/fel DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

if(FEL_BUILD_TOOLS)
  install(TARGETS fel_cli RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()

install(EXPORT fel-targets
  FILE fel-targets.cmake
  NAMESPACE fel::
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/fel)

configure_package_config_file(
  ${CMAKE_CURRENT_SOURCE_DIR}/cmake/fel-config.cmake.in
  ${CMAKE_CURRENT_BINARY_DIR}/fel-config.cmake
  INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/fel)

write_basic_package_version_file(
  ${CMAKE_CURRENT_BINARY_DIR}/fel-config-version.cmake
  VERSION ${PROJECT_VERSION}
  COMPATIBILITY SameMajorVersion
  ARCH_INDEPENDENT)

install(FILES
  ${CMAKE_CURRENT_BINARY_DIR}/fel-config.cmake
  ${CMAKE_CURRENT_BINARY_DIR}/fel-config-version.cmake
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/fel)

install(FILES LICENSE README.md DESTINATION ${CMAKE_INSTALL_DATADIR}/fel)

message(STATUS "Fabric Efficiency Ledger ${PROJECT_VERSION}")
message(STATUS "  build shared     : ${FEL_BUILD_SHARED}")
message(STATUS "  tests            : ${FEL_BUILD_TESTS}")
message(STATUS "  tools            : ${FEL_BUILD_TOOLS}")
message(STATUS "  warnings as err  : ${FEL_WERROR}")
message(STATUS "  address sanitizer: ${FEL_ENABLE_ASAN}")