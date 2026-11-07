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
  test_dag_thread_fence
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

  # Which find_package version requests the installed package accepts: SameMajorVersion,
  # so an old consumer asking for 0.3 is refused rather than handed an engine that needs
  # C++23 and CMake 4. Reads the version file the install ships; nothing is built.
  add_test(NAME test_package_version
    COMMAND ${CMAKE_COMMAND}
      -DVERSION_FILE=${PROJECT_BINARY_DIR}/flywheel_dagConfigVersion.cmake
      -DPROJECT_VERSION=${PROJECT_VERSION}
      -P ${CMAKE_CURRENT_SOURCE_DIR}/check_package_version.cmake)

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

# The CRTP bases (OpNodeImpl, NodeImpl, StatefulNodeBase) keep their constructors private to the class they are the
# base of, so that `class Wrong : public OpNodeImpl<Other>` does not compile. The suite derives them correctly, so
# nothing else would notice a constructor that is public or protected: this compiles a wrong derivation of each, with
# -std=c++23 and -fsyntax-only, and expects an access error. Registered for GCC and Clang, at the top level, like
# test_standard_guard.
if(_flywheel_dag_top_level AND CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  add_test(NAME test_crtp_access
    COMMAND ${CMAKE_COMMAND}
      -DCXX=${CMAKE_CXX_COMPILER}
      -DINCLUDE_DIR=${PROJECT_SOURCE_DIR}/include
      -DSPDLOG_SOURCE_DIR=${spdlog_SOURCE_DIR}
      -DNLOHMANN_JSON_SOURCE_DIR=${nlohmann_json_SOURCE_DIR}
      -DWORK_DIR=${CMAKE_CURRENT_BINARY_DIR}/crtp_access
      -P ${CMAKE_CURRENT_SOURCE_DIR}/check_crtp_access.cmake)
endif()

# The engine issues its standalone fences through dag::threadFence(), which keeps GCC's -Wtsan quiet for a fence that
# orders atomics only. Nothing else would notice a bare std::atomic_thread_fence, or a pragma that does not work: the
# suite is not built under ThreadSanitizer, and the default build is not -Werror. This scans the headers for a bare
# fence, compiles the helper and an engine unit with -fsanitize=thread (compile only, so no ThreadSanitizer runtime is
# needed), and on GCC checks that a bare fence draws the warning. Registered for GCC and Clang, at the top level, like
# test_crtp_access.
if(_flywheel_dag_top_level AND CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  add_test(NAME test_tsan_fences
    COMMAND ${CMAKE_COMMAND}
      -DCXX=${CMAKE_CXX_COMPILER}
      -DCXX_ID=${CMAKE_CXX_COMPILER_ID}
      -DINCLUDE_DIR=${PROJECT_SOURCE_DIR}/include
      -DSPDLOG_SOURCE_DIR=${spdlog_SOURCE_DIR}
      -DNLOHMANN_JSON_SOURCE_DIR=${nlohmann_json_SOURCE_DIR}
      -DWORK_DIR=${CMAKE_CURRENT_BINARY_DIR}/tsan_fences
      -P ${CMAKE_CURRENT_SOURCE_DIR}/check_tsan_fences.cmake)
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

# The strict configure flags. CI's two workflows, the README and CLAUDE.md spell them
# -Werror=author -Werror=deprecated, and the check holds the four to that spelling. On
# CMake 4.4 or later it also configures a probe project to see that the flags make an
# author warning and a deprecation fatal; an older CMake ignores -Werror=author without
# a word, so there it checks the spelling only and says so. The probe compiles nothing
# and needs no network. Registered at the top level, where the checkout it reads is the
# engine's.
if(_flywheel_dag_top_level)
  add_test(NAME test_cmake_diagnostics
    COMMAND ${CMAKE_COMMAND}
      -DSOURCE_DIR=${PROJECT_SOURCE_DIR}
      -DSCRATCH_DIR=${CMAKE_CURRENT_BINARY_DIR}/cmake_diagnostics
      -DGENERATOR=${CMAKE_GENERATOR}
      -P ${CMAKE_CURRENT_SOURCE_DIR}/check_cmake_diagnostics.cmake)
endif()

# The clang-tidy workflow cannot be run here, so this holds what a run would find out late
# or not at all: that it names the LLVM major the gate registers below, in one place; that
# it turns the gate on, with the strict configure flags; that it runs only the clang_tidy_
# tests; that nothing lets it pass by failing; and that its matrix names real presets, for a
# Linux and a macOS leg. Registered at the top level, where the checkout it reads is the
# engine's.
if(_flywheel_dag_top_level)
  add_test(NAME test_ci_clang_tidy
    COMMAND ${CMAKE_COMMAND}
      -DSOURCE_DIR=${PROJECT_SOURCE_DIR}
      -P ${CMAKE_CURRENT_SOURCE_DIR}/check_ci_clang_tidy.cmake)
endif()

# What CI ignores. A workflow that skips a push because it changed only documentation is right
# when its run reads none of it, and silently wrong when it reads some. This holds every
# paths-ignore list to the checkout: the two triggers of a workflow agree, each entry matches a
# file, no entry covers what the build or the lint reads, and a workflow that runs the checks
# ignores nothing that a check reads. Registered at the top level, where the checkout it reads
# is the engine's.
if(_flywheel_dag_top_level)
  add_test(NAME test_ci_paths
    COMMAND ${CMAKE_COMMAND}
      -DSOURCE_DIR=${PROJECT_SOURCE_DIR}
      -P ${CMAKE_CURRENT_SOURCE_DIR}/check_ci_paths.cmake)
endif()

# The files Claude Code loads stay small. CLAUDE.md and the engine rules it imports are in every
# session's context, so they have byte budgets; a CLAUDE.md anywhere in the checkout loads on demand, and Claude Code
# keeps only the start of one past 40,000 characters, so none may pass 38,000. The budgets are these three numbers,
# the one place they are written. The check is a plain cmake -P script, so it runs in every tree.
#
# Its own behaviour is held by fixtures in tests/claude_md_budget/, run with budgets of 20 bytes, 20 bytes and 30
# characters: one it must accept, at the limit and with multi-byte text that is under the character cap and over the
# byte count, and five it must refuse, each for its own reason. A refusal passes on the text of the script's own
# message and fails on the message of any other rule, because a PASS_REGULAR_EXPRESSION ignores the exit status: a
# script that accepts the fixture, or one that dies of something else, fails the test.
# Registered at the top level, where the checkout it reads is the engine's.
if(_flywheel_dag_top_level)
  set(_claude_md_budget_script   ${CMAKE_CURRENT_SOURCE_DIR}/check_claude_md_budget.cmake)
  set(_claude_md_budget_fixtures ${CMAKE_CURRENT_SOURCE_DIR}/claude_md_budget)
  set(_claude_md_budget_small    -DCLAUDE_MD_BYTES=20 -DRULES_BYTES=20 -DNESTED_CHARS=30)

  add_test(NAME test_claude_md_budget
    COMMAND ${CMAKE_COMMAND}
      -DSOURCE_DIR=${PROJECT_SOURCE_DIR}
      -DCLAUDE_MD_BYTES=6500 -DRULES_BYTES=6000 -DNESTED_CHARS=38000
      -P ${_claude_md_budget_script})

  add_test(NAME test_claude_md_budget_accepts
    COMMAND ${CMAKE_COMMAND}
      -DSOURCE_DIR=${_claude_md_budget_fixtures}/accepts ${_claude_md_budget_small}
      -P ${_claude_md_budget_script})

  # The fixture's name, the problem it must draw, and the messages of the other rules, which it must not.
  function(flywheel_dag_claude_md_budget_rejects name problem others)
    add_test(NAME test_claude_md_budget_${name}
      COMMAND ${CMAKE_COMMAND}
        -DSOURCE_DIR=${_claude_md_budget_fixtures}/${name} ${_claude_md_budget_small}
        -P ${_claude_md_budget_script})
    set_tests_properties(test_claude_md_budget_${name} PROPERTIES
      PASS_REGULAR_EXPRESSION "${problem}"
      FAIL_REGULAR_EXPRESSION "${others}")
  endfunction()

  set(_over_claude_md  "CLAUDE\\.md is [0-9]+ bytes, over its budget")
  set(_over_rules      "docs/engine_rules\\.md is [0-9]+ bytes, over its budget")
  set(_over_nested     "CLAUDE\\.md is [0-9]+ characters, over the cap")
  set(_missing_claude_md "[^/]CLAUDE\\.md is missing")
  set(_missing_rules   "docs/engine_rules\\.md is missing")

  flywheel_dag_claude_md_budget_rejects(rejects_claude_md
    "CLAUDE\\.md is 22 bytes, over its budget of 20"
    "${_over_rules}|${_over_nested}|is missing")
  flywheel_dag_claude_md_budget_rejects(rejects_rules
    "docs/engine_rules\\.md is 21 bytes, over its budget of 20"
    "${_over_claude_md}|${_over_nested}|is missing")
  flywheel_dag_claude_md_budget_rejects(rejects_nested
    "sub/CLAUDE\\.md is 31 characters, over the cap of 30"
    "${_over_claude_md}|${_over_rules}|is missing")
  flywheel_dag_claude_md_budget_rejects(rejects_no_claude_md
    "[^/]CLAUDE\\.md is missing"
    "${_over_claude_md}|${_over_rules}|${_over_nested}|${_missing_rules}")
  flywheel_dag_claude_md_budget_rejects(rejects_no_rules
    "docs/engine_rules\\.md is missing"
    "${_over_claude_md}|${_over_rules}|${_over_nested}|${_missing_claude_md}")
endif()

# Comments state what is true of the code now. History lives in the commit, the issue and the plan, so a comment cites no
# closed issue and points at no plan. check_comment_refs.cmake reads every file under include/, tests/, benchmarks/,
# examples/, cmake/ and .github/ and every file at the top that is not a document, and fails on a reference to an issue
# that OPEN_ISSUES does not name, on a pointer into the plans, and on a number in OPEN_ISSUES that no file cites.
# OPEN_ISSUES is the one place the issues the code may cite are written: a comment cites an open issue only where it
# states the limitation the issue tracks, and the number leaves the list with the last such comment. The check is a
# plain cmake -P script, so it runs in every tree.
#
# Its own behaviour is held by fixtures in tests/comment_refs/, which the scan skips because they cite issues on
# purpose: a tree it must accept, a tree with a problem of every kind, and a tree with nothing to scan. Each problem has
# a test of its own that passes on the text of that problem, because a PASS_REGULAR_EXPRESSION ignores the exit status:
# a script that accepts the tree, or one that dies of something else, fails every one of them. The patterns spell a
# reference with a bracket expression, [#] and [/], so that this file holds no reference itself.
# Registered at the top level, where the checkout it reads is the engine's.
if(_flywheel_dag_top_level)
  set(_comment_refs_script   ${CMAKE_CURRENT_SOURCE_DIR}/check_comment_refs.cmake)
  set(_comment_refs_fixtures ${CMAKE_CURRENT_SOURCE_DIR}/comment_refs)

  add_test(NAME test_comment_refs
    COMMAND ${CMAKE_COMMAND}
      -DSOURCE_DIR=${PROJECT_SOURCE_DIR} -DOPEN_ISSUES=31,32
      -P ${_comment_refs_script})

  add_test(NAME test_comment_refs_accepts
    COMMAND ${CMAKE_COMMAND}
      -DSOURCE_DIR=${_comment_refs_fixtures}/accepts -DOPEN_ISSUES=31,32
      -P ${_comment_refs_script})

  # A run the check must refuse: the test's name, the text of the one problem it must print, and the arguments of the run.
  function(flywheel_dag_comment_refs_refuses name problem)
    add_test(NAME test_comment_refs_${name}
      COMMAND ${CMAKE_COMMAND} ${ARGN} -P ${_comment_refs_script})
    set_tests_properties(test_comment_refs_${name} PROPERTIES PASS_REGULAR_EXPRESSION "${problem}")
  endfunction()

  # tests/comment_refs/rejects names one open issue (31) that no file cites, and cites others line by line.
  function(flywheel_dag_comment_refs_rejects name problem)
    flywheel_dag_comment_refs_refuses(rejects_${name} "${problem}"
      -DSOURCE_DIR=${_comment_refs_fixtures}/rejects -DOPEN_ISSUES=31)
  endfunction()

  flywheel_dag_comment_refs_rejects(include
    "include/flywheel/refs\\.hpp:13 cites flywheel-dag[#]12, which")
  flywheel_dag_comment_refs_rejects(second_reference_in_a_file
    "include/flywheel/refs\\.hpp:15 cites flywheel-dag[#]13, which")
  flywheel_dag_comment_refs_rejects(issue_url
    "include/flywheel/refs\\.hpp:16 cites flywheel-dag[/]issues[/]21, which")
  flywheel_dag_comment_refs_rejects(pull_request_url
    "include/flywheel/refs\\.hpp:17 cites flywheel-dag[/]pull[/]22, which")
  flywheel_dag_comment_refs_rejects(longer_number
    "include/flywheel/refs\\.hpp:18 cites flywheel-dag[#]310, which")
  flywheel_dag_comment_refs_rejects(tests
    "tests/CMakeLists\\.txt:2 cites flywheel-dag[#]14, which")
  flywheel_dag_comment_refs_rejects(plan_pointer
    "tests/CMakeLists\\.txt:3 points into the plans directory")
  flywheel_dag_comment_refs_rejects(benchmarks
    "benchmarks/CMakeLists\\.txt:2 cites flywheel-dag[#]15, which")
  flywheel_dag_comment_refs_rejects(examples
    "examples/CMakeLists\\.txt:2 cites flywheel-dag[#]16, which")
  flywheel_dag_comment_refs_rejects(cmake
    "cmake/refs\\.cmake:2 cites flywheel-dag[#]17, which")
  flywheel_dag_comment_refs_rejects(github
    "\\.github/workflows/ci\\.yml:2 cites flywheel-dag[#]18, which")
  flywheel_dag_comment_refs_rejects(root_cmake
    "[ ]CMakeLists\\.txt:2 cites flywheel-dag[#]19, which")
  flywheel_dag_comment_refs_rejects(tool_config
    "\\.clang-tidy:2 cites flywheel-dag[#]20, which")
  flywheel_dag_comment_refs_rejects(unused_open_issue
    "flywheel-dag[#]31 is in OPEN_ISSUES and no file cites it")

  # A tree with nothing to read, and the arguments the check refuses.
  flywheel_dag_comment_refs_refuses(rejects_nothing_to_scan "nothing to scan under"
    -DSOURCE_DIR=${_comment_refs_fixtures}/rejects_nothing_to_scan -DOPEN_ISSUES=)
  flywheel_dag_comment_refs_refuses(rejects_no_source_dir "SOURCE_DIR is not set"
    -DOPEN_ISSUES=31)
  flywheel_dag_comment_refs_refuses(rejects_no_open_issues "OPEN_ISSUES is not set"
    -DSOURCE_DIR=${_comment_refs_fixtures}/accepts)
  flywheel_dag_comment_refs_refuses(rejects_malformed_open_issues "OPEN_ISSUES is '31,x', not issue numbers"
    -DSOURCE_DIR=${_comment_refs_fixtures}/accepts -DOPEN_ISSUES=31,x)
endif()

# clang-tidy over this project's own sources, as a gate. The root sets _flywheel_dag_clang_tidy when
# FLYWHEEL_DAG_CLANG_TIDY names a tool, and has already refused a configure that asked for one it cannot run, so
# nothing here checks again. Nothing is built: clang-tidy reads compile_commands.json and the sources the build would
# compile, and FetchContent has put the dependencies' headers in place by now.
#
#   clang_tidy_<stem>    one per .cpp under tests/, examples/ and benchmarks/. The list is a glob, so a new file is
#                        never skipped: one that no target compiles has no compile command, and fails.
#   clang_tidy_version   the tool is LLVM 23. The Mac, the Ubuntu container and CI run one major, and this is what
#                        stops them drifting apart unseen. Moving to another is one commit: this number, the workflow,
#                        the Ubuntu image, and whatever the new major reports.
#   clang_tidy_checks    .clang-tidy enables the families it should, and none that it leaves out:
#                        bugprone-easily-swappable-parameters, bugprone-narrowing-conversions, readability-* and
#                        modernize-*.
#   clang_tidy_canary_*  three proofs that the gate bites, on fixtures that are in no compile database.
#
# A lint gate that silently checks nothing passes CI, which is why the gate itself is tested.
if(_flywheel_dag_clang_tidy)
  set(_flywheel_dag_clang_tidy_major 23)

  # The compile commands carry -Werror=deprecated-declarations (CMakeLists.txt). With it, a clang-tidy newer than the SDK's
  # libc++ stops 21 units on nlohmann/json 3.12.0's CBOR declarations, which name std::char_traits<unsigned char>, a
  # specialization that libc++ deprecates: the error is in the SDK's headers, from a use in a dependency's. Here a
  # deprecation is a warning, --warnings-as-errors makes it an error where it is in this project's own files, and the
  # header filter drops it in the SDK's and the dependencies'.
  set(_clang_tidy_args --warnings-as-errors=* --extra-arg=-Wno-error=deprecated-declarations
                       ${FLYWHEEL_DAG_CLANG_TIDY_ARGS})
  if(APPLE AND NOT CMAKE_OSX_SYSROOT)
    # /usr/bin/c++ finds the SDK itself, so the compile commands carry no -isysroot, and a clang-tidy that is not
    # Apple's cannot find the C library's headers without one.
    execute_process(COMMAND xcrun --show-sdk-path
                    OUTPUT_VARIABLE _clang_tidy_sdk OUTPUT_STRIP_TRAILING_WHITESPACE
                    RESULT_VARIABLE _clang_tidy_sdk_status)
    if(NOT _clang_tidy_sdk_status EQUAL 0 OR NOT _clang_tidy_sdk)
      message(FATAL_ERROR "xcrun --show-sdk-path names no SDK, and clang-tidy needs one on this host")
    endif()
    list(APPEND _clang_tidy_args --extra-arg=-isysroot --extra-arg=${_clang_tidy_sdk})
  endif()

  file(GLOB _clang_tidy_sources CONFIGURE_DEPENDS
       ${PROJECT_SOURCE_DIR}/tests/*.cpp
       ${PROJECT_SOURCE_DIR}/examples/*.cpp
       ${PROJECT_SOURCE_DIR}/benchmarks/*.cpp)
  foreach(source IN LISTS _clang_tidy_sources)
    cmake_path(GET source STEM _clang_tidy_stem)
    add_test(NAME clang_tidy_${_clang_tidy_stem}
      COMMAND ${_flywheel_dag_clang_tidy} -p ${PROJECT_BINARY_DIR} --quiet ${_clang_tidy_args} ${source})
  endforeach()

  add_test(NAME clang_tidy_version COMMAND ${_flywheel_dag_clang_tidy} --version)
  set_tests_properties(clang_tidy_version PROPERTIES
    PASS_REGULAR_EXPRESSION "LLVM version ${_flywheel_dag_clang_tidy_major}\\.")

  add_test(NAME clang_tidy_checks
    COMMAND ${_flywheel_dag_clang_tidy} --config-file=${PROJECT_SOURCE_DIR}/.clang-tidy --list-checks)
  # The list prints sorted, so one expression asks for a check of each family in order.
  set_tests_properties(clang_tidy_checks PROPERTIES
    PASS_REGULAR_EXPRESSION
      "bugprone-use-after-move.*cppcoreguidelines-pro-type-member-init.*performance-unnecessary-value-param"
    FAIL_REGULAR_EXPRESSION "bugprone-easily-swappable-parameters|bugprone-narrowing-conversions|readability-|modernize-")

  # The canary: one unit with a defect of its own, and two headers that carry the same defect at the two kinds of path
  # the header filter must tell apart. The fixtures are copied under the build tree so that their paths are
  # .../include/flywheel/... and .../_deps/..., not tests/..., which the filter would admit whatever else it did. All
  # three tests run one command: the output has to show the source's error, the engine header's error, and no word of
  # the dependency header. "error:", not "warning:", is --warnings-as-errors biting; a check's name is followed by a
  # comma or the bracket, because the flag appends ",-warnings-as-errors".
  set(_clang_tidy_canary ${PROJECT_BINARY_DIR}/clang_tidy_canary)
  configure_file(clang_tidy_canary/canary.cpp
                 ${_clang_tidy_canary}/canary.cpp COPYONLY)
  configure_file(clang_tidy_canary/canary_engine.hpp
                 ${_clang_tidy_canary}/include/flywheel/canary_engine.hpp COPYONLY)
  configure_file(clang_tidy_canary/canary_dep.hpp
                 ${_clang_tidy_canary}/_deps/canary-src/include/canary_dep.hpp COPYONLY)
  set(_clang_tidy_canary_command
    ${_flywheel_dag_clang_tidy} --config-file=${PROJECT_SOURCE_DIR}/.clang-tidy ${_clang_tidy_args}
    ${_clang_tidy_canary}/canary.cpp -- -std=c++23
    -I${_clang_tidy_canary}/include -I${_clang_tidy_canary}/_deps/canary-src/include)
  set(_clang_tidy_canary_source_error
    "canary\\.cpp:[0-9]+:[0-9]+: error: [^\n]*\\[bugprone-use-after-move[],]")
  add_test(NAME clang_tidy_canary_source COMMAND ${_clang_tidy_canary_command})
  set_tests_properties(clang_tidy_canary_source PROPERTIES
    PASS_REGULAR_EXPRESSION "${_clang_tidy_canary_source_error}")
  add_test(NAME clang_tidy_canary_engine_header COMMAND ${_clang_tidy_canary_command})
  set_tests_properties(clang_tidy_canary_engine_header PROPERTIES
    PASS_REGULAR_EXPRESSION
      "include/flywheel/canary_engine\\.hpp:[0-9]+:[0-9]+: error: [^\n]*\\[performance-unnecessary-value-param[],]")
  add_test(NAME clang_tidy_canary_dependency_header COMMAND ${_clang_tidy_canary_command})
  set_tests_properties(clang_tidy_canary_dependency_header PROPERTIES
    PASS_REGULAR_EXPRESSION "${_clang_tidy_canary_source_error}"
    FAIL_REGULAR_EXPRESSION "canary_dep\\.hpp")
endif()
