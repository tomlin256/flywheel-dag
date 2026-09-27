add_executable(quickstart quickstart.cpp)
target_link_libraries(quickstart PRIVATE flywheel::dag)

# The example doubles as a test: its signal has exactly eight spikes in 2000
# cycles, so a correct graph reports exactly eight episodes, each resolved.
if(FLYWHEEL_DAG_BUILD_TESTS)
  add_test(NAME example_quickstart COMMAND quickstart 2000)
  set_tests_properties(example_quickstart PROPERTIES
    PASS_REGULAR_EXPRESSION "onsets=8 resolved=8")
endif()
