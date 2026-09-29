# A minimal downstream project: pulls flywheel-dag in as a FetchContent
# subproject, the way a consumer would, then builds and runs one program
# against flywheel::dag. Built and run by test_consumer_subproject.
cmake_minimum_required(VERSION 3.18)
project(flywheel_dag_consumer LANGUAGES CXX)

if(NOT FLYWHEEL_DAG_SOURCE_DIR)
  message(FATAL_ERROR "set -DFLYWHEEL_DAG_SOURCE_DIR to the flywheel-dag checkout")
endif()

include(FetchContent)
FetchContent_Declare(flywheel_dag SOURCE_DIR ${FLYWHEEL_DAG_SOURCE_DIR})
set(cxx_flags_before "${CMAKE_CXX_FLAGS}")
FetchContent_MakeAvailable(flywheel_dag)

# A consumer must inherit neither the engine's tests nor googletest.
foreach(leaked IN ITEMS test_dag quickstart gtest GTest::gtest)
  if(TARGET ${leaked})
    message(FATAL_ERROR "flywheel-dag leaked target '${leaked}' into its consumer")
  endif()
endforeach()

# Nor does the engine install itself into its consumer's prefix unless the
# consumer asks (flywheel-dag#2).
if(FLYWHEEL_DAG_INSTALL)
  message(FATAL_ERROR "flywheel-dag turned on its install rules in its consumer")
endif()

# A consumer's compile flags are its own (flywheel-dag#4): the engine's warning
# flags must reach it neither through flywheel::dag nor through the cache.
get_target_property(engine_options flywheel::dag INTERFACE_COMPILE_OPTIONS)
if(engine_options)
  message(FATAL_ERROR "flywheel::dag passes compile options to its consumer: "
                      "${engine_options}")
endif()
if(NOT "${CMAKE_CXX_FLAGS}" STREQUAL "${cxx_flags_before}")
  message(FATAL_ERROR "flywheel-dag changed its consumer's CMAKE_CXX_FLAGS from "
                      "'${cxx_flags_before}' to '${CMAKE_CXX_FLAGS}'")
endif()

# No CMAKE_CXX_STANDARD here on purpose: flywheel::dag's cxx_std_17 compile
# feature must be enough on its own.
add_executable(consumer main.cpp)
target_link_libraries(consumer PRIVATE flywheel::dag)
