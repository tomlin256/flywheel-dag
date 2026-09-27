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
FetchContent_MakeAvailable(flywheel_dag)

# A consumer must inherit neither the engine's tests nor googletest.
foreach(leaked IN ITEMS test_dag quickstart gtest GTest::gtest)
  if(TARGET ${leaked})
    message(FATAL_ERROR "flywheel-dag leaked target '${leaked}' into its consumer")
  endif()
endforeach()

# No CMAKE_CXX_STANDARD here on purpose: flywheel::dag's cxx_std_17 compile
# feature must be enough on its own.
add_executable(consumer main.cpp)
target_link_libraries(consumer PRIVATE flywheel::dag)
