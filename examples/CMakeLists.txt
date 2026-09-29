add_executable(quickstart quickstart.cpp)
target_link_libraries(quickstart PRIVATE flywheel::dag)

add_executable(aad aad.cpp)
target_link_libraries(aad PRIVATE flywheel::dag)

if(FLYWHEEL_DAG_BUILD_TESTS)
  # The example doubles as a test: its signal has exactly eight spikes in 2000
  # cycles, so a correct graph reports exactly eight episodes, each resolved.
  add_test(NAME example_quickstart COMMAND quickstart 2000)
  set_tests_properties(example_quickstart PROPERTIES
    PASS_REGULAR_EXPRESSION "onsets=8 resolved=8")

  # So does this one: its last line says whether the reverse sweep, the forward
  # sweeps and the bumps agree. It prints that line only when they do.
  add_test(NAME example_aad COMMAND aad)
  set_tests_properties(example_aad PROPERTIES
    PASS_REGULAR_EXPRESSION "reverse, forward and bumped agree")
endif()
