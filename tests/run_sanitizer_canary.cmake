# run_sanitizer_canary.cmake — sanitizer_canary_*: run a canary and require the sanitizer to catch it.
#
# Run by ctest as
#
#   cmake -DCANARY=<exe> -DEXPECT=<regex> -P run_sanitizer_canary.cmake
#
# Each canary (tests/sanitizer_canary/) commits one deliberate violation and returns 0 itself, so the exit status and the report both
# come from the sanitizer. This requires BOTH:
#
#   - a non-zero exit. UBSan only gets one from -fno-sanitize-recover=undefined; without it the report is printed and the process
#     exits 0, which is a green run over a real finding.
#   - the sanitizer's own report header (EXPECT) in the output. A non-zero exit alone could be any crash, and says nothing about the
#     sanitizer.
#
# Either one missing fails the test. That is what proves a sanitizer tree's options are live: with every real test green, a tree
# whose options never reached the compiler is only visible here. test_sanitizer_scripts (check_sanitizer_scripts.cmake) runs this
# against stand-ins for a canary, so each way of failing is held in every tree.

cmake_minimum_required(VERSION 4.0)

if(NOT DEFINED CANARY OR NOT DEFINED EXPECT)
    message(FATAL_ERROR "usage: cmake -DCANARY=<exe> -DEXPECT=<regex> -P run_sanitizer_canary.cmake")
endif()

# One variable for both streams: the sanitizers report on stderr, and merging keeps the report in order with whatever else the
# program printed.
execute_process(
    COMMAND "${CANARY}"
    RESULT_VARIABLE status
    OUTPUT_VARIABLE output
    ERROR_VARIABLE output)

if("${output}" MATCHES "${EXPECT}")
    set(reported TRUE)
else()
    set(reported FALSE)
endif()

# status is "0" for a clean exit, a number for another exit code, and text such as "Child aborted" when a signal ended the process:
# anything but "0" is a stop.
if("${status}" STREQUAL "0")
    if(reported)
        message(FATAL_ERROR
            "${CANARY} printed the sanitizer's report and exited 0: the sanitizer is live but does not stop on a finding, so a "
            "real one would leave a green run. UBSan needs -fno-sanitize-recover=undefined.\n${output}")
    endif()
    message(FATAL_ERROR
        "${CANARY} exited 0 with no report: the sanitizer caught nothing. Its options probably never reached the compiler or the "
        "linker, or a runtime option switched it off (ASAN_OPTIONS=detect_leaks=0 does, for the leak canary).\n${output}")
endif()
if(NOT reported)
    message(FATAL_ERROR
        "${CANARY} stopped (${status}) but its output has no match for '${EXPECT}': it died of something other than the "
        "sanitizer.\n${output}")
endif()
message(STATUS "${CANARY} stopped (${status}) with a report matching '${EXPECT}'")
