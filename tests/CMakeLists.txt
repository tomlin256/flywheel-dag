# One executable per suite; each defines its own main().
set(FLYWHEEL_DAG_TESTS
  test_aad_dual
  test_aad_forward
  test_aad_gradient_node
  test_aad_node
  test_aad_partials
  test_aad_reverse
  test_aad_tangent_node
  test_allocation_counter
  test_clean_inputs
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
  test_wake_state
  test_window_status
  test_window_status_integration
)

# The suites that count allocations. allocation_counter.cpp replaces the global
# operator new and operator delete (allocation_counter.hpp), and a replacement
# is the whole program's, so it is linked into these and no other.
set(FLYWHEEL_DAG_COUNTING_TESTS
  test_aad_node
  test_allocation_counter
  test_value_slot
)

foreach(test IN LISTS FLYWHEEL_DAG_TESTS)
  add_executable(${test} ${test}.cpp)
  if(test IN_LIST FLYWHEEL_DAG_COUNTING_TESTS)
    target_sources(${test} PRIVATE allocation_counter.cpp)
  endif()
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

# Which platform a build tree is for, and whether its name tells the truth. The
# first test holds the two functions of cmake/flywheel_dag_platform.cmake to a
# table. The second configures the real root CMakeLists.txt, which calls them
# before project(), into trees named for the host and for the other
# architecture; it compiles nothing of the project's and needs no network.
add_test(NAME test_platform_name
  COMMAND ${CMAKE_COMMAND}
    -DSOURCE_DIR=${PROJECT_SOURCE_DIR}
    -P ${CMAKE_CURRENT_SOURCE_DIR}/check_platform.cmake)
add_test(NAME test_tree_name
  COMMAND ${CMAKE_COMMAND}
    -DSOURCE_DIR=${PROJECT_SOURCE_DIR}
    -DSCRATCH_DIR=${CMAKE_CURRENT_BINARY_DIR}/tree_name
    -DGENERATOR=${CMAKE_GENERATOR}
    -DSPDLOG_SOURCE_DIR=${spdlog_SOURCE_DIR}
    -DNLOHMANN_JSON_SOURCE_DIR=${nlohmann_json_SOURCE_DIR}
    -P ${CMAKE_CURRENT_SOURCE_DIR}/check_tree_name.cmake)

# The installed package. These tests are registered wherever the root promises
# install rules, never from anything the install block itself sets. Everything
# they install goes into its own prefix under package/.
if(FLYWHEEL_DAG_INSTALL)
  set(package_dir ${CMAKE_CURRENT_BINARY_DIR}/package)

  # Installs this build into an empty prefix and checks that the prefix holds
  # the engine alone.
  add_test(NAME test_install
    COMMAND ${CMAKE_COMMAND}
      -DBINARY_DIR=${PROJECT_BINARY_DIR}
      -DSOURCE_DIR=${PROJECT_SOURCE_DIR}
      -DPREFIX=${package_dir}/engine
      -DINCLUDEDIR=${CMAKE_INSTALL_INCLUDEDIR}
      -DDATADIR=${CMAKE_INSTALL_DATADIR}
      -DCONFIG=$<CONFIG>
      -P ${CMAKE_CURRENT_SOURCE_DIR}/check_install.cmake)
  set_tests_properties(test_install PROPERTIES FIXTURES_SETUP installed_engine)

  # spdlog and nlohmann/json in a prefix of their own, as a package manager
  # provides them. Like test_consumer_subproject, it builds spdlog, which takes
  # longer than the default timeout allows.
  add_test(NAME install_dependencies
    COMMAND ${CMAKE_COMMAND}
      -DSPDLOG_SOURCE_DIR=${spdlog_SOURCE_DIR}
      -DNLOHMANN_JSON_SOURCE_DIR=${nlohmann_json_SOURCE_DIR}
      -DBINARY_DIR=${package_dir}/dependencies-build
      -DPREFIX=${package_dir}/dependencies
      -DGENERATOR=${CMAKE_GENERATOR}
      -DCONFIG=$<CONFIG>
      -P ${CMAKE_CURRENT_SOURCE_DIR}/install_dependencies.cmake)
  set_tests_properties(install_dependencies PROPERTIES
    FIXTURES_SETUP installed_dependencies
    TIMEOUT 600)

  # A consumer that finds the installed engine with find_package. The engine's
  # config finds spdlog and nlohmann/json as installed packages.
  add_test(NAME test_consumer_package
    COMMAND ${CMAKE_CTEST_COMMAND}
      --build-and-test ${CMAKE_CURRENT_SOURCE_DIR}/consumer
                       ${package_dir}/consumer
      --build-generator ${CMAKE_GENERATOR}
      --build-options -DFLYWHEEL_DAG_PACKAGE_VERSION=${PROJECT_VERSION}
                      -DCMAKE_PREFIX_PATH=${package_dir}/engine$<SEMICOLON>${package_dir}/dependencies
                      -DCMAKE_BUILD_TYPE=${CMAKE_BUILD_TYPE}
      --test-command consumer)
  set_tests_properties(test_consumer_package PROPERTIES
    FIXTURES_REQUIRED "installed_engine;installed_dependencies")

  # A consumer that fetches its own spdlog and nlohmann/json, then finds the
  # installed engine in a prefix that holds the engine alone. The engine's
  # config must use the consumer's copies. The consumer builds spdlog, like
  # test_consumer_subproject.
  add_test(NAME test_consumer_package_own_dependencies
    COMMAND ${CMAKE_CTEST_COMMAND}
      --build-and-test ${CMAKE_CURRENT_SOURCE_DIR}/consumer
                       ${package_dir}/consumer_own_dependencies
      --build-generator ${CMAKE_GENERATOR}
      --build-options -DFLYWHEEL_DAG_PACKAGE_VERSION=${PROJECT_VERSION}
                      -DCMAKE_PREFIX_PATH=${package_dir}/engine
                      -DCONSUMER_FETCHES_DEPENDENCIES=ON
                      -DFETCHCONTENT_SOURCE_DIR_SPDLOG=${spdlog_SOURCE_DIR}
                      -DFETCHCONTENT_SOURCE_DIR_NLOHMANN_JSON=${nlohmann_json_SOURCE_DIR}
                      -DCMAKE_BUILD_TYPE=${CMAKE_BUILD_TYPE}
      --test-command consumer)
  set_tests_properties(test_consumer_package_own_dependencies PROPERTIES
    FIXTURES_REQUIRED installed_engine
    TIMEOUT 600)
endif()

# The warning flags reach every translation unit of this project and none of
# its dependencies'. Registered wherever the root CMakeLists.txt promises the
# flags (the top level, GCC or Clang), never from anything the code that sets
# them decides: a slip there must fail this test, not unregister it. The check
# reads compile_commands.json, which only the Makefile and Ninja generators
# write.
if(_flywheel_dag_top_level AND CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang"
   AND (CMAKE_GENERATOR MATCHES "Makefiles" OR CMAKE_GENERATOR STREQUAL "Ninja"))
  add_test(NAME test_warning_flags
    COMMAND ${CMAKE_COMMAND}
      -DCOMPILE_COMMANDS=${CMAKE_BINARY_DIR}/compile_commands.json
      -DSOURCE_DIR=${PROJECT_SOURCE_DIR}
      -DBINARY_DIR=${CMAKE_BINARY_DIR}
      -DWARNINGS_AS_ERRORS=${FLYWHEEL_DAG_WARNINGS_AS_ERRORS}
      -P ${CMAKE_CURRENT_SOURCE_DIR}/check_warning_flags.cmake)
endif()

# Every public header refuses to compile below C++23, and says why. The suite
# builds as C++23, so nothing else would notice a guard that stopped working:
# this compiles each header at C++17 and at C++20 and expects the guard's message.
# It runs the compiler itself with -std= and -fsyntax-only, so it is registered
# for GCC and Clang, at the top level.
if(_flywheel_dag_top_level AND CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  add_test(NAME test_standard_guard
    COMMAND ${CMAKE_COMMAND}
      -DCXX=${CMAKE_CXX_COMPILER}
      -DINCLUDE_DIR=${PROJECT_SOURCE_DIR}/include
      -DWORK_DIR=${CMAKE_CURRENT_BINARY_DIR}/standard_guard
      -P ${CMAKE_CURRENT_SOURCE_DIR}/check_standard_guard.cmake)
endif()

# CMakePresets.json names one build tree per kind and platform and says which of
# them a host can use. The check reads the file with string(JSON) and asks CMake
# to list the presets for this host. Registered at the top level, where the file
# means something, and never from anything the file itself decides.
if(_flywheel_dag_top_level)
  add_test(NAME test_presets
    COMMAND ${CMAKE_COMMAND}
      -DSOURCE_DIR=${PROJECT_SOURCE_DIR}
      -DHOST_SYSTEM_NAME=${CMAKE_HOST_SYSTEM_NAME}
      -P ${CMAKE_CURRENT_SOURCE_DIR}/check_presets.cmake)
endif()

# One CMake floor. The root CMakeLists.txt states it, and every other minimum in
# the checkout and the README say the same. Registered at the top level, where the
# checkout it reads is the engine's.
if(_flywheel_dag_top_level)
  add_test(NAME test_cmake_floor
    COMMAND ${CMAKE_COMMAND}
      -DSOURCE_DIR=${PROJECT_SOURCE_DIR}
      -P ${CMAKE_CURRENT_SOURCE_DIR}/check_cmake_floor.cmake)
endif()
