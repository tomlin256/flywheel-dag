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
