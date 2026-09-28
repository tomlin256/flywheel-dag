# flywheel-dag — a header-only C++17 reactive DAG computation engine.
#
# Consume it with FetchContent (or add_subdirectory) and link flywheel::dag:
#
#   FetchContent_Declare(flywheel_dag
#     GIT_REPOSITORY https://github.com/tomlin256/flywheel-dag.git
#     GIT_TAG        v0.1.1)
#   FetchContent_MakeAvailable(flywheel_dag)
#   target_link_libraries(my_app PRIVATE flywheel::dag)
#
# 3.18 is a plain minimum, not a <min>...<max> range: a range flips every policy
# up to <max> to NEW, which needs its own audit.
cmake_minimum_required(VERSION 3.18)
project(FLYWHEEL_DAG VERSION 0.1.1 LANGUAGES CXX)

# Export compile_commands.json for clangd and other tooling.
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# ─────────────────────────────────────────────────────────────────────────────
# Top-level or subproject.
#
# Tests, examples and every build-wide setting below apply only when this is
# the top-level project. A consumer's build settings are its own, and it never
# inherits the engine's test targets or googletest.
# ─────────────────────────────────────────────────────────────────────────────
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    set(_flywheel_dag_top_level ON)
else()
    set(_flywheel_dag_top_level OFF)
endif()

option(FLYWHEEL_DAG_BUILD_TESTS    "Build the engine's tests" ${_flywheel_dag_top_level})
option(FLYWHEEL_DAG_BUILD_EXAMPLES "Build the examples"       ${_flywheel_dag_top_level})

if(_flywheel_dag_top_level)
    # RelWithDebInfo (-O2 -g -DNDEBUG) unless a build type was asked for: an
    # unset build type means no optimisation at all, which misrepresents every
    # allocation and timing the tests and examples exercise. An explicit
    # -DCMAKE_BUILD_TYPE=... is never overridden.
    get_property(_flywheel_dag_multi_config GLOBAL PROPERTY GENERATOR_IS_MULTI_CONFIG)
    if(NOT _flywheel_dag_multi_config AND NOT CMAKE_BUILD_TYPE)
        set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING
            "Build type (Debug|Release|RelWithDebInfo|MinSizeRel)" FORCE)
        message(STATUS "flywheel-dag: CMAKE_BUILD_TYPE not set — defaulting to RelWithDebInfo")
    endif()

    set(CMAKE_CXX_STANDARD 17)
    set(CMAKE_CXX_STANDARD_REQUIRED ON)
    set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
endif()

# ─────────────────────────────────────────────────────────────────────────────
# Dependencies: spdlog and nlohmann/json, and googletest for the tests.
#
# A parent that already provides a target (find_package, or its own
# FetchContent) is used as-is. A parent that declares the same FetchContent name
# first wins that way too, so a build holds one copy of each.
# ─────────────────────────────────────────────────────────────────────────────
include(FetchContent)

if(NOT TARGET spdlog::spdlog)
    FetchContent_Declare(
      spdlog
      GIT_REPOSITORY https://github.com/gabime/spdlog.git
      GIT_TAG        v1.17.0
    )
    FetchContent_MakeAvailable(spdlog)
endif()

if(NOT TARGET nlohmann_json::nlohmann_json)
    FetchContent_Declare(
      nlohmann_json
      GIT_REPOSITORY https://github.com/nlohmann/json.git
      GIT_TAG        v3.12.0
    )
    FetchContent_MakeAvailable(nlohmann_json)
endif()

if(FLYWHEEL_DAG_BUILD_TESTS)
    set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
    set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
    FetchContent_Declare(
      googletest
      GIT_REPOSITORY https://github.com/google/googletest.git
      GIT_TAG        v1.16.0
    )
    FetchContent_MakeAvailable(googletest)
endif()

# ─────────────────────────────────────────────────────────────────────────────
# The engine. flywheel_dag is the real target; consumers link the alias,
# flywheel::dag (an ALIAS cannot be the subject of target_* commands).
# ─────────────────────────────────────────────────────────────────────────────
add_library(flywheel_dag INTERFACE)
add_library(flywheel::dag ALIAS flywheel_dag)
target_include_directories(flywheel_dag INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(flywheel_dag INTERFACE spdlog::spdlog nlohmann_json::nlohmann_json)
target_compile_features(flywheel_dag INTERFACE cxx_std_17)

# ─────────────────────────────────────────────────────────────────────────────
# Tests and examples (top-level by default).
# ─────────────────────────────────────────────────────────────────────────────
if(_flywheel_dag_top_level)
    # Any use of a [[deprecated]] API is an error in this project's own code.
    # After every FetchContent dependency, so third-party sources are unaffected.
    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        add_compile_options(-Werror=deprecated-declarations)
    endif()

    # List each static library once on a link line where the linker allows it.
    # Unset, CMP0156 repeats them the way a single-pass linker needs, and Apple's
    # ld warns "ignoring duplicate libraries" on every relink. NEW de-duplicates
    # on Apple and Windows only; CMP0179 NEW keeps the first occurrence.
    if(POLICY CMP0156)
        cmake_policy(SET CMP0156 NEW)
    endif()
    if(POLICY CMP0179)
        cmake_policy(SET CMP0179 NEW)
    endif()
endif()

if(FLYWHEEL_DAG_BUILD_TESTS)
    # include(CTest), not a bare enable_testing(): it writes the
    # DartConfiguration.tcl that gives every test a default TIMEOUT, so one hung
    # test cannot run until something outside kills it.
    set(DART_TESTING_TIMEOUT 120 CACHE STRING "Default per-test timeout (seconds)")
    include(CTest)
    add_subdirectory(tests)
endif()

if(FLYWHEEL_DAG_BUILD_EXAMPLES)
    add_subdirectory(examples)
endif()
