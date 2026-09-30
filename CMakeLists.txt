# flywheel-dag — a header-only C++17 reactive DAG computation engine.
#
# Consume it with FetchContent (or add_subdirectory) and link flywheel::dag:
#
#   FetchContent_Declare(flywheel_dag
#     GIT_REPOSITORY https://github.com/tomlin256/flywheel-dag.git
#     GIT_TAG        v0.1.11)
#   FetchContent_MakeAvailable(flywheel_dag)
#   target_link_libraries(my_app PRIVATE flywheel::dag)
#
# or install it (cmake --install <build> --prefix <prefix>) and find it. The
# consumer then provides spdlog and nlohmann/json itself:
#
#   find_package(flywheel_dag 0.1 CONFIG REQUIRED)
#   target_link_libraries(my_app PRIVATE flywheel::dag)
#
# 3.18 is a plain minimum, not a <min>...<max> range: a range flips every policy
# up to <max> to NEW, which needs its own audit.
cmake_minimum_required(VERSION 3.18)
project(FLYWHEEL_DAG VERSION 0.1.11 LANGUAGES CXX)

# Export compile_commands.json for clangd and other tooling.
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# ─────────────────────────────────────────────────────────────────────────────
# Top-level or subproject.
#
# Tests, examples, benchmarks, install rules and every build-wide setting below
# apply only when this is the top-level project. A consumer's build settings are
# its own: it never inherits the engine's test targets or googletest, and the
# engine lands in its install prefix only if it sets FLYWHEEL_DAG_INSTALL.
# ─────────────────────────────────────────────────────────────────────────────
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    set(_flywheel_dag_top_level ON)
else()
    set(_flywheel_dag_top_level OFF)
endif()

option(FLYWHEEL_DAG_BUILD_TESTS      "Build the engine's tests"                           ${_flywheel_dag_top_level})
option(FLYWHEEL_DAG_BUILD_EXAMPLES   "Build the examples"                                 ${_flywheel_dag_top_level})
option(FLYWHEEL_DAG_BUILD_BENCHMARKS "Add the benchmarks, which build on demand"          ${_flywheel_dag_top_level})
option(FLYWHEEL_DAG_INSTALL          "Generate the install rules and the package config" ${_flywheel_dag_top_level})

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
# flywheel::dag (an ALIAS cannot be the subject of target_* commands). The
# install exports it under the same name: EXPORT_NAME dag, in the flywheel::
# namespace.
#
# The include path and the dependencies are $<BUILD_INTERFACE:...>, which is
# its content in a build tree and empty in the export. The export cannot name
# the dependencies: the copies this build fetched are its own targets, in no
# export set. The package config links the copies the consumer has instead
# (cmake/flywheel_dagConfig.cmake.in).
# ─────────────────────────────────────────────────────────────────────────────
add_library(flywheel_dag INTERFACE)
add_library(flywheel::dag ALIAS flywheel_dag)
target_include_directories(flywheel_dag INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)
target_link_libraries(flywheel_dag INTERFACE
    $<BUILD_INTERFACE:spdlog::spdlog>
    $<BUILD_INTERFACE:nlohmann_json::nlohmann_json>)
target_compile_features(flywheel_dag INTERFACE cxx_std_17)
set_target_properties(flywheel_dag PROPERTIES EXPORT_NAME dag)

# ─────────────────────────────────────────────────────────────────────────────
# Install (flywheel-dag#2): the engine alone, meaning its headers, a
# find_package(flywheel_dag) config and the LICENSE file. Every installed file
# exists once configure finishes, so nothing needs building first. spdlog and
# nlohmann/json are not installed: a consumer provides them, as installed
# packages or through its own FetchContent before find_package. test_install
# checks the installed set.
# ─────────────────────────────────────────────────────────────────────────────
if(FLYWHEEL_DAG_INSTALL)
    include(GNUInstallDirs)
    include(CMakePackageConfigHelpers)

    # share/, not lib/: a header-only package does not depend on the architecture.
    set(_flywheel_dag_config_dir ${CMAKE_INSTALL_DATADIR}/cmake/flywheel_dag)

    install(TARGETS flywheel_dag EXPORT flywheel_dagTargets
            INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
    # Headers only, so an editor backup or a .DS_Store is never installed.
    install(DIRECTORY include/flywheel DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
            FILES_MATCHING PATTERN "*.hpp" PATTERN "*.inl")
    install(EXPORT flywheel_dagTargets NAMESPACE flywheel::
            DESTINATION ${_flywheel_dag_config_dir})

    configure_package_config_file(cmake/flywheel_dagConfig.cmake.in
        ${CMAKE_CURRENT_BINARY_DIR}/flywheel_dagConfig.cmake
        INSTALL_DESTINATION ${_flywheel_dag_config_dir})
    # Before 1.0 a minor release may break the API, so a request for 0.1 accepts
    # any 0.1.z and nothing else. At 1.0 this becomes SameMajorVersion.
    # ARCH_INDEPENDENT: headers do not care what pointer size the consumer has.
    write_basic_package_version_file(
        ${CMAKE_CURRENT_BINARY_DIR}/flywheel_dagConfigVersion.cmake
        COMPATIBILITY SameMinorVersion ARCH_INDEPENDENT)
    install(FILES ${CMAKE_CURRENT_BINARY_DIR}/flywheel_dagConfig.cmake
                  ${CMAKE_CURRENT_BINARY_DIR}/flywheel_dagConfigVersion.cmake
            DESTINATION ${_flywheel_dag_config_dir})

    # Every header's banner points to this file.
    install(FILES LICENSE DESTINATION ${CMAKE_INSTALL_DATADIR}/doc/flywheel_dag)
endif()

# ─────────────────────────────────────────────────────────────────────────────
# Tests, examples and benchmarks (top-level by default).
# ─────────────────────────────────────────────────────────────────────────────
if(_flywheel_dag_top_level)
    # This project's own targets build with -Wall -Wextra -Wpedantic, and any
    # use of a [[deprecated]] API is an error (flywheel-dag#4). Those targets
    # are the tests, the example and the benchmark: they are what compile the
    # engine's headers. add_compile_options reaches only the targets created
    # after it, here and below, so placing it after every FetchContent
    # dependency leaves third-party sources unaffected. Nothing goes on
    # flywheel_dag's INTERFACE, and a subproject sets none of this, so a
    # consumer's flags stay its own. test_warning_flags and
    # test_consumer_subproject hold both halves.
    #
    # FLYWHEEL_DAG_WARNINGS_AS_ERRORS makes every warning an error, and CI
    # turns it on. It is off by default, so a newer compiler's new warning
    # does not stop a contributor's build; CI still refuses it.
    option(FLYWHEEL_DAG_WARNINGS_AS_ERRORS
           "Make every compiler warning an error in this project's own targets" OFF)
    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        add_compile_options(-Wall -Wextra -Wpedantic -Werror=deprecated-declarations)
        if(FLYWHEEL_DAG_WARNINGS_AS_ERRORS)
            add_compile_options(-Werror)
        endif()
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

if(FLYWHEEL_DAG_BUILD_BENCHMARKS)
    add_subdirectory(benchmarks)
endif()
