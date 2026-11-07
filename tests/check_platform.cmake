# check_platform.cmake — test_platform_name: the platform tag and the tree-name
# check of cmake/flywheel_dag_platform.cmake, over a fixed table, so that the
# result does not depend on the host it runs on. The check refuses a name that
# claims another platform, a tree named for a sanitizer it is not configured
# for, and a tree that remembers another platform.
#
# Run by ctest as
#
#   cmake -DSOURCE_DIR=<source> -P check_platform.cmake
#
# SOURCE_DIR is the checkout whose module is under test. The module's two
# functions are pure; a "name" below is a tree's own name, the last component
# of its path. A hand-made change to a copy of the module is passed here, and
# must fail the row that names it.

cmake_minimum_required(VERSION 4.0)

if(NOT SOURCE_DIR)
    message(FATAL_ERROR "usage: cmake -DSOURCE_DIR=<dir> -P check_platform.cmake")
endif()
set(module "${SOURCE_DIR}/cmake/flywheel_dag_platform.cmake")
if(NOT EXISTS "${module}")
    message(FATAL_ERROR "${module} does not exist")
endif()
include("${module}")

set(cases 0)
set(failed 0)
set(failures "")

# Macros, not functions, so that they count and record in this scope.
macro(check_failed label detail)
    math(EXPR failed "${failed} + 1")
    string(APPEND failures "  ${label}: ${detail}\n")
endmacro()

# The tag for a host.
macro(expect_tag system machine expected)
    math(EXPR cases "${cases} + 1")
    flywheel_dag_platform(actual "${system}" "${machine}")
    if(NOT "${actual}" STREQUAL "${expected}")
        check_failed("tag: ${system}/${machine}" "expected ${expected}, got '${actual}'")
    endif()
endmacro()

# A name that is no fault of the host's: no problem, with the platform the tree
# remembers (empty on a first configure) and the sanitizer it is configured for
# (the value of FLYWHEEL_DAG_SANITIZE, empty for none).
macro(expect_accepted name platform recorded sanitize)
    math(EXPR cases "${cases} + 1")
    flywheel_dag_tree_problem(problem "${name}" "${platform}" "${recorded}" "${sanitize}")
    if(NOT "${problem}" STREQUAL "")
        check_failed("accepted: ${name} on ${platform} (remembers '${recorded}', sanitizer '${sanitize}')"
                     "refused with: ${problem}")
    endif()
endmacro()

# A name that claims a platform other than the host's, and a text that says
# which platform it claims and which one the host is.
macro(expect_claim_refused name platform claimed)
    math(EXPR cases "${cases} + 1")
    flywheel_dag_tree_problem(problem "${name}" "${platform}" "" "")
    if("${problem}" STREQUAL "")
        check_failed("refused, names both platforms: ${name} on ${platform}" "accepted")
    else()
        string(FIND "${problem}" "is for ${claimed}, but this host is ${platform}" at)
        if(at EQUAL -1)
            check_failed("refused, names both platforms: ${name} on ${platform}"
                         "the text lacks 'is for ${claimed}, but this host is ${platform}': ${problem}")
        endif()
    endif()
endmacro()

# A tree that remembers another platform than the host's.
macro(expect_recorded_refused name platform recorded)
    math(EXPR cases "${cases} + 1")
    flywheel_dag_tree_problem(problem "${name}" "${platform}" "${recorded}" "")
    if("${problem}" STREQUAL "")
        check_failed("refused, names both platforms: ${name} on ${platform} (remembers ${recorded})"
                     "accepted")
    else()
        string(FIND "${problem}" "first configured on ${recorded}, but this host is ${platform}" at)
        if(at EQUAL -1)
            check_failed("refused, names both platforms: ${name} on ${platform} (remembers ${recorded})"
                         "the text lacks 'first configured on ${recorded}, but this host is ${platform}': ${problem}")
        endif()
    endif()
endmacro()

# A name that is refused for what it leaves out; the text names the host.
macro(expect_refused name platform)
    math(EXPR cases "${cases} + 1")
    flywheel_dag_tree_problem(problem "${name}" "${platform}" "" "")
    if("${problem}" STREQUAL "")
        check_failed("refused: ${name} on ${platform}" "accepted")
    else()
        string(FIND "${problem}" "this host is ${platform}" at)
        if(at EQUAL -1)
            check_failed("refused: ${name} on ${platform}"
                         "the text lacks 'this host is ${platform}': ${problem}")
        endif()
    endif()
endmacro()

# A tree named for a sanitizer it is not configured for. <wanted> is the sanitizer in the name and <sanitize> the value of
# FLYWHEEL_DAG_SANITIZE; the text names the tree, <wanted> and the value (OFF when it is empty).
macro(expect_sanitizer_refused name platform sanitize wanted)
    math(EXPR cases "${cases} + 1")
    flywheel_dag_tree_problem(problem "${name}" "${platform}" "" "${sanitize}")
    set(shown "${sanitize}")
    if("${shown}" STREQUAL "")
        set(shown OFF)
    endif()
    set(expected "the build directory \"${name}\" is named for the ${wanted} sanitizer, but FLYWHEEL_DAG_SANITIZE is '${shown}'")
    if("${problem}" STREQUAL "")
        check_failed("refused, names the tree and both modes: ${name} on ${platform} (sanitizer '${sanitize}')" "accepted")
    else()
        string(FIND "${problem}" "${expected}" at)
        if(at EQUAL -1)
            check_failed("refused, names the tree and both modes: ${name} on ${platform} (sanitizer '${sanitize}')"
                         "the text lacks '${expected}': ${problem}")
        endif()
    endif()
endmacro()

# ── tag ──────────────────────────────────────────────────────────────────────
expect_tag(Darwin  arm64   macos-arm64)
expect_tag(Linux   aarch64 linux-arm64)
expect_tag(Linux   x86_64  linux-x86_64)
expect_tag(Linux   AMD64   linux-x86_64)
expect_tag(Darwin  x86_64  macos-x86_64)
expect_tag(Windows AMD64   windows-x86_64)

# ── accepted ─────────────────────────────────────────────────────────────────
# Truthful platform trees, with a suffix after the architecture and with no kind.
foreach(tree IN ITEMS relwithdebinfo-linux-arm64 release-linux-arm64-gcc13 linux-arm64)
    expect_accepted("${tree}" linux-arm64 "" "")
endforeach()

# A sanitizer tree configured for the mode in its name, with a suffix and in either case; a kind that only starts like a sanitizer's;
# and a sanitizer in a tree that is not named for one.
expect_accepted(asan-linux-arm64       linux-arm64 "" asan)
expect_accepted(asan-linux-arm64-gcc13 linux-arm64 "" asan)
expect_accepted(tsan-macos-arm64       macos-arm64 "" tsan)
expect_accepted(Asan-Linux-Arm64       linux-arm64 "" asan)
expect_accepted(asanx-linux-arm64      linux-arm64 "" "")
expect_accepted(release-linux-arm64    linux-arm64 "" asan)
expect_accepted(linux-arm64            linux-arm64 "" tsan)

# Names that claim no platform are not examined, whatever the host is, and a sanitizer's name is no exception.
foreach(tree IN ITEMS build scratch vscode build-vscode cmake-build-debug release-linuxbrew asan tsan asan-scratch)
    expect_accepted("${tree}" linux-arm64 "" "")
    expect_accepted("${tree}" macos-arm64 "" "")
endforeach()

# The platform a tree remembers, and a first configure.
expect_accepted(build linux-arm64 linux-arm64 "")
expect_accepted(build linux-arm64 "" "")
expect_accepted(asan-linux-arm64 linux-arm64 linux-arm64 asan)

# ── refused, and the text names both platforms ───────────────────────────────
expect_claim_refused(release-linux-x86_64  linux-arm64 linux-x86_64)
expect_claim_refused(linux-x86_64          linux-arm64 linux-x86_64)
expect_claim_refused(debug-macos-arm64     linux-arm64 macos-arm64)
expect_claim_refused(release-linux-aarch64 linux-arm64 linux-aarch64)
expect_claim_refused(release-linux-arm64x  linux-arm64 linux-arm64x)
expect_claim_refused(Release-Linux-X86_64  linux-arm64 linux-x86_64)
# A false claim is named before a sanitizer that is missing.
expect_claim_refused(asan-macos-arm64      linux-arm64 macos-arm64)
expect_recorded_refused(build linux-arm64 macos-arm64)

# ── refused: a tree named for a sanitizer it is not configured for ───────────
expect_sanitizer_refused(asan-linux-arm64       linux-arm64 ""   asan)
expect_sanitizer_refused(asan-linux-arm64       linux-arm64 tsan asan)
expect_sanitizer_refused(tsan-macos-arm64       macos-arm64 asan tsan)
expect_sanitizer_refused(asan-macos-arm64       macos-arm64 ""   asan)
expect_sanitizer_refused(asan-linux-arm64-gcc13 linux-arm64 ""   asan)
expect_sanitizer_refused(Asan-Linux-Arm64       linux-arm64 ""   asan)
expect_sanitizer_refused(tsan-linux-arm64       linux-arm64 OFF  tsan)
# The mode is compared exactly, as the option compares it.
expect_sanitizer_refused(asan-linux-arm64       linux-arm64 ASAN asan)

# ── refused ──────────────────────────────────────────────────────────────────
expect_refused(release-linux      linux-arm64)
expect_refused(release-linux-asan linux-arm64)
expect_refused(linux              linux-arm64)

if(failed GREATER 0)
    message(FATAL_ERROR "test_platform_name: ${failed} of ${cases} cases failed:\n${failures}")
endif()
message(STATUS "test_platform_name: all ${cases} cases hold")
