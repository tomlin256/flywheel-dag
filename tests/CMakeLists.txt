# One executable per suite; each defines its own main().
set(FLYWHEEL_DAG_TESTS
  test_compute_module
  test_dag
  test_dag_async
  test_dag_graph
  test_dag_memoize
  test_dag_ops
  test_dag_replay
  test_dag_state_store
  test_dag_traversal
  test_engine_state
  test_engine_stateful_discovery
  test_json_state_store
  test_lazy_invalidation
  test_ring_buffer
  test_stateful_node_base
  test_timeseries
  test_value_slot
  test_window_status
  test_window_status_integration
)

foreach(test IN LISTS FLYWHEEL_DAG_TESTS)
  add_executable(${test} ${test}.cpp)
  target_link_libraries(${test} PRIVATE flywheel::dag GTest::gtest)
  add_test(NAME ${test} COMMAND ${test})
endforeach()

# The engine as a downstream project sees it: pulled in with FetchContent, built
# and run (consumer/). It reuses the dependency sources this build has already
# fetched, so it needs no network. Building spdlog again takes longer than the
# default timeout allows.
add_test(NAME test_consumer_subproject
  COMMAND ${CMAKE_CTEST_COMMAND}
    --build-and-test ${CMAKE_CURRENT_SOURCE_DIR}/consumer
                     ${CMAKE_CURRENT_BINARY_DIR}/consumer
    --build-generator ${CMAKE_GENERATOR}
    --build-options -DFLYWHEEL_DAG_SOURCE_DIR=${PROJECT_SOURCE_DIR}
                    -DFETCHCONTENT_SOURCE_DIR_SPDLOG=${spdlog_SOURCE_DIR}
                    -DFETCHCONTENT_SOURCE_DIR_NLOHMANN_JSON=${nlohmann_json_SOURCE_DIR}
                    -DCMAKE_BUILD_TYPE=${CMAKE_BUILD_TYPE}
    --test-command consumer)
set_tests_properties(test_consumer_subproject PROPERTIES TIMEOUT 600)

# The warning flags reach every translation unit of this project and none of
# its dependencies' (flywheel-dag#4). Registered wherever the root
# CMakeLists.txt promises the flags (the top level, GCC or Clang), never from
# anything the code that sets them decides: a slip there must fail this test,
# not unregister it. The check reads compile_commands.json, which only the
# Makefile and Ninja generators write, using string(JSON), which needs
# CMake 3.19.
if(_flywheel_dag_top_level AND CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang"
   AND (CMAKE_GENERATOR MATCHES "Makefiles" OR CMAKE_GENERATOR STREQUAL "Ninja")
   AND NOT CMAKE_VERSION VERSION_LESS 3.19)
  add_test(NAME test_warning_flags
    COMMAND ${CMAKE_COMMAND}
      -DCOMPILE_COMMANDS=${CMAKE_BINARY_DIR}/compile_commands.json
      -DSOURCE_DIR=${PROJECT_SOURCE_DIR}
      -DBINARY_DIR=${CMAKE_BINARY_DIR}
      -DWARNINGS_AS_ERRORS=${FLYWHEEL_DAG_WARNINGS_AS_ERRORS}
      -P ${CMAKE_CURRENT_SOURCE_DIR}/check_warning_flags.cmake)
endif()
