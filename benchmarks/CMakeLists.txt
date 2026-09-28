# A measurement tool, not a test: EXCLUDE_FROM_ALL, and deliberately not
# registered with ctest, because a wall-clock threshold would be a flaky test.
# Build and run it on demand:
#
#   cmake --build build --target bench_hot_path
#   ./build/bin/bench_hot_path
add_executable(bench_hot_path EXCLUDE_FROM_ALL bench_hot_path.cpp bench_report.cpp)
target_link_libraries(bench_hot_path PRIVATE flywheel::dag)

# No fused multiply-adds. arm64 compilers fuse a * b + c by default, and x86-64
# without -mfma cannot, so the two would differ in the last bit of a result.
# The checksum is compared on both CI platforms, so it must not depend on that.
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(bench_hot_path PRIVATE -ffp-contract=off)
endif()

# The report format is tested, because CI compares the benchmark's output with
# a committed file. The timings are not.
if(FLYWHEEL_DAG_BUILD_TESTS)
  add_executable(test_bench_report test_bench_report.cpp bench_report.cpp)
  target_link_libraries(test_bench_report PRIVATE GTest::gtest)
  add_test(NAME test_bench_report COMMAND test_bench_report)
endif()
