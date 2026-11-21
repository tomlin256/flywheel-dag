# check_target_options.cmake — test_target_options: flywheel_dag_add_target_options() gives its options to the targets it is asked
# about and to no other, says so loudly when it is called wrongly, and calls no command that CMake 4.5 reports as a non-target
# directive.
#
# Run by ctest as
#
#   cmake -DSOURCE_DIR=<source> -DSCRATCH_DIR=<dir> -DGENERATOR=<generator> -DCXX=<compiler> -P check_target_options.cmake
#
# It writes a stub project under SCRATCH_DIR and configures it once, with the running tree's compiler and generator, for every case
# that makes targets. It compiles nothing and needs no network. A case is a directory of the stub that makes the same targets: in a
# directory "own", an executable, a static, a shared, a module and an object library, an INTERFACE library, a custom target and an
# imported target, and below it, two directories deep, one more executable; in a sibling "other", which stands for a dependency, an
# executable, a static library and, one directory down, an executable. The case calls the function, once or twice, and prints each
# target's COMPILE_OPTIONS and LINK_OPTIONS, which this script reads and compares with what the case must give. A call that must be
# refused is a stub of its own that needs no compiler, and must fail to configure with a message that names what is wrong.
#
# The last case is the one that CMake 4.5 makes of the module itself: the main stub, configured again with -Werror=author
# -Werror=deprecated, must configure, so the function uses no non-target directive. Before 4.5 the diagnostic does not exist, and
# that case is skipped with a line that says so.
#
# SOURCE_DIR is a parameter so that a hand-made change can be made to a scratch copy of the module and must fail here.

cmake_minimum_required(VERSION 4.0)

foreach(var IN ITEMS SOURCE_DIR SCRATCH_DIR GENERATOR CXX)
    if(NOT ${var})
        message(FATAL_ERROR "usage: cmake -DSOURCE_DIR=<dir> -DSCRATCH_DIR=<dir> -DGENERATOR=<name> -DCXX=<compiler> "
                            "-P check_target_options.cmake")
    endif()
endforeach()

set(module "${SOURCE_DIR}/cmake/flywheel_dag_target_options.cmake")
if(NOT EXISTS "${module}")
    message(FATAL_ERROR "test_target_options: ${module} does not exist")
endif()

# Options that nothing compiles, so that each is its own evidence of where it came from.
set(compile_a -fprobe-compile-one -fprobe-compile-two)
set(compile_b -fprobe-compile-three)
set(link_a -fprobe-link-one -fprobe-link-two)

file(REMOVE_RECURSE "${SCRATCH_DIR}")
set(stub "${SCRATCH_DIR}/stub")
set(failures "")
set(cases 0)

# ── the stub ─────────────────────────────────────────────────────────────────

# write_case(<name> <lines>): the directory <name> of the stub, with the targets that every case makes, then <lines>, which the
# case's CMakeLists.txt runs once its own and its "dependency's" directories have been added.
function(write_case name lines)
    set(dir "${stub}/${name}")
    file(MAKE_DIRECTORY "${dir}/own/nested/deeper" "${dir}/other/deep" "${dir}/never")
    foreach(where IN ITEMS . own own/nested/deeper other other/deep never)
        file(WRITE "${dir}/${where}/s.cpp" "int f() { return 0; }\n")
    endforeach()
    file(WRITE "${dir}/own/CMakeLists.txt" [=[
add_executable(${P}_own_exe s.cpp)
add_library(${P}_own_static STATIC s.cpp)
add_library(${P}_own_shared SHARED s.cpp)
add_library(${P}_own_module MODULE s.cpp)
add_library(${P}_own_object OBJECT s.cpp)
add_library(${P}_own_interface INTERFACE)
add_custom_target(${P}_own_custom)
add_library(${P}_own_imported UNKNOWN IMPORTED GLOBAL)
add_subdirectory(nested)
]=])
    file(WRITE "${dir}/own/nested/CMakeLists.txt" "add_subdirectory(deeper)\n")
    file(WRITE "${dir}/own/nested/deeper/CMakeLists.txt" [=[
add_executable(${P}_own_deep_exe s.cpp)
]=])
    file(WRITE "${dir}/other/CMakeLists.txt" [=[
add_executable(${P}_other_exe s.cpp)
add_library(${P}_other_static STATIC s.cpp)
add_subdirectory(deep)
]=])
    file(WRITE "${dir}/other/deep/CMakeLists.txt" [=[
add_executable(${P}_other_deep_exe s.cpp)
]=])
    # Never added by any case: a directory that exists, and that this tree does not hold.
    file(WRITE "${dir}/never/CMakeLists.txt" [=[
add_executable(${P}_never_exe s.cpp)
]=])
    file(WRITE "${dir}/CMakeLists.txt" "set(P ${name})\nadd_subdirectory(other)\nadd_subdirectory(own)\n${lines}\n")
endfunction()

# The stub's root. Each case's targets are reported with their options, ";" shown as "|" so that a line is one list item.
set(report_suffixes own_exe own_static own_shared own_module own_object own_interface own_custom own_imported own_deep_exe
                    other_exe other_static other_deep_exe never_exe late_exe)
set(root_lines "cmake_minimum_required(VERSION 4.0)\nproject(stub CXX)\ninclude(\"${module}\")\n")
string(APPEND root_lines [=[
function(probe_report prefix)
    foreach(suffix IN ITEMS @report_suffixes@)
        set(target ${prefix}_${suffix})
        if(TARGET ${target})
            get_target_property(compile ${target} COMPILE_OPTIONS)
            get_target_property(link ${target} LINK_OPTIONS)
            get_target_property(inherited_compile ${target} INTERFACE_COMPILE_OPTIONS)
            get_target_property(inherited_link ${target} INTERFACE_LINK_OPTIONS)
            foreach(kind IN ITEMS compile link inherited_compile inherited_link)
                if(${kind})
                    string(REPLACE ";" "|" ${kind} "${${kind}}")
                else()
                    set(${kind} "-")
                endif()
            endforeach()
            message(STATUS "probe: ${target} compile=${compile} link=${link} inherited=${inherited_compile}/${inherited_link}")
        endif()
    endforeach()
endfunction()
]=])
string(REPLACE "@report_suffixes@" "${report_suffixes}" root_lines "${root_lines}")

# name, then the lines that call the function. COMPILE_A and the like are replaced below, so that this table is the one place that
# says what each case asks for.
set(case_names in in_deeper overlap all compile_only link_only late twice)
set(call_in          "flywheel_dag_add_target_options(IN own never COMPILE @compile_a@ LINK @link_a@)")
set(call_in_deeper   "flywheel_dag_add_target_options(IN own/nested COMPILE @compile_a@ LINK @link_a@)")
set(call_overlap     "flywheel_dag_add_target_options(IN own own/nested COMPILE @compile_a@)")
set(call_all         "flywheel_dag_add_target_options(COMPILE @compile_a@ LINK @link_a@)")
set(call_compile_only "flywheel_dag_add_target_options(IN own COMPILE @compile_a@)")
set(call_link_only   "flywheel_dag_add_target_options(IN own LINK @link_a@)")
set(call_late        "flywheel_dag_add_target_options(IN own COMPILE @compile_a@)\nadd_executable(\${P}_late_exe s.cpp)")
set(call_twice       "flywheel_dag_add_target_options(IN own COMPILE @compile_a@)\nflywheel_dag_add_target_options(COMPILE @compile_b@)")
foreach(name IN LISTS case_names)
    set(lines "${call_${name}}")
    string(REPLACE "@compile_a@" "${compile_a}" lines "${lines}")
    string(REPLACE "@compile_b@" "${compile_b}" lines "${lines}")
    string(REPLACE "@link_a@" "${link_a}" lines "${lines}")
    write_case(${name} "${lines}")
    string(APPEND root_lines "add_subdirectory(${name})\nprobe_report(${name})\n")
endforeach()
file(WRITE "${stub}/CMakeLists.txt" "${root_lines}")

# ── configuring it ───────────────────────────────────────────────────────────

# configure(<rc> <output> <source> <tree> [<arguments>...]): <output> keeps its lines, and has the output and the error output
# together.
function(configure rc_var out_var source tree)
    execute_process(
        COMMAND "${CMAKE_COMMAND}" -G "${GENERATOR}" -S "${source}" -B "${tree}" "-DCMAKE_CXX_COMPILER=${CXX}" ${ARGN}
        RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
    set(${rc_var} "${rc}" PARENT_SCOPE)
    set(${out_var} "${out}\n${err}" PARENT_SCOPE)
endfunction()

# One line of text, whatever the width CMake wrapped an error at.
function(squash out_var text)
    string(REGEX REPLACE "[ \t\r\n]+" " " text "${text}")
    set(${out_var} "${text}" PARENT_SCOPE)
endfunction()

configure(rc out "${stub}" "${SCRATCH_DIR}/build")
if(NOT rc EQUAL 0)
    squash(text "${out}")
    string(APPEND failures "\n  the stub did not configure (${rc}): ${text}")
endif()

# What the stub printed: seen_compile_<target>, seen_link_<target> and seen_inherited_<target>, the interface options.
string(REPLACE "\n" ";" lines "${out}")
foreach(line IN LISTS lines)
    if(line MATCHES "^-- probe: ([A-Za-z0-9_]+) compile=([^ ]*) link=([^ ]*) inherited=([^ ]*)$")
        set(seen_compile_${CMAKE_MATCH_1} "${CMAKE_MATCH_2}")
        set(seen_link_${CMAKE_MATCH_1} "${CMAKE_MATCH_3}")
        set(seen_inherited_${CMAKE_MATCH_1} "${CMAKE_MATCH_4}")
    endif()
endforeach()

# expect(<target> <compile> <link>): "-" is no option. <compile> and <link> are lists, shown with "|".
function(expect target compile link)
    math(EXPR n "${cases} + 1")
    set(cases ${n} PARENT_SCOPE)
    string(REPLACE ";" "|" compile "${compile}")
    string(REPLACE ";" "|" link "${link}")
    if(NOT DEFINED seen_compile_${target})
        string(APPEND failures "\n  ${target}: the stub did not report it")
    else()
        if(NOT "${seen_compile_${target}}" STREQUAL "${compile}")
            string(APPEND failures "\n  ${target}: compile options are '${seen_compile_${target}}', want '${compile}'")
        endif()
        if(NOT "${seen_link_${target}}" STREQUAL "${link}")
            string(APPEND failures "\n  ${target}: link options are '${seen_link_${target}}', want '${link}'")
        endif()
        # The options are PRIVATE: a target that links this one inherits none of them.
        if(NOT "${seen_inherited_${target}}" STREQUAL "-/-")
            string(APPEND failures "\n  ${target}: interface options are '${seen_inherited_${target}}', want none")
        endif()
    endif()
    set(failures "${failures}" PARENT_SCOPE)
endfunction()

function(expect_absent target)
    math(EXPR n "${cases} + 1")
    set(cases ${n} PARENT_SCOPE)
    if(DEFINED seen_compile_${target})
        string(APPEND failures "\n  ${target}: exists, and the case never makes it")
    endif()
    set(failures "${failures}" PARENT_SCOPE)
endfunction()

# expect_tree(<case> <own compile> <own link> <deep compile> <deep link> <other compile> <other link>): every target of a case.
# Compile options go to every executable and library, link options to executables, shared libraries and modules, and nothing goes
# to an INTERFACE library, a custom target or an imported target. "own" is the directory the case names, "deep" what is two levels
# below it, and "other" the sibling that stands for a dependency.
function(expect_tree case own_compile own_link deep_compile deep_link other_compile other_link)
    foreach(kind IN ITEMS own_exe own_shared own_module)
        expect(${case}_${kind} "${own_compile}" "${own_link}")
    endforeach()
    foreach(kind IN ITEMS own_static own_object)
        expect(${case}_${kind} "${own_compile}" "-")
    endforeach()
    foreach(kind IN ITEMS own_interface own_custom own_imported)
        expect(${case}_${kind} "-" "-")
    endforeach()
    expect(${case}_own_deep_exe "${deep_compile}" "${deep_link}")
    expect(${case}_other_exe "${other_compile}" "${other_link}")
    expect(${case}_other_static "${other_compile}" "-")
    expect(${case}_other_deep_exe "${other_compile}" "${other_link}")
    expect_absent(${case}_never_exe)
    set(cases ${cases} PARENT_SCOPE)
    set(failures "${failures}" PARENT_SCOPE)
endfunction()

if(rc EQUAL 0)
    # IN: own and what it added; a directory that exists and this tree never added is no error and adds nothing.
    expect_tree(in "${compile_a}" "${link_a}" "${compile_a}" "${link_a}" - -)
    # IN names a directory below the current one's own subdirectory.
    expect_tree(in_deeper - - "${compile_a}" "${link_a}" - -)
    # IN names a directory and one below it: a target is given its options once.
    expect_tree(overlap "${compile_a}" - "${compile_a}" - - -)
    # No IN: the whole tree, the "dependency" too.
    expect_tree(all "${compile_a}" "${link_a}" "${compile_a}" "${link_a}" "${compile_a}" "${link_a}")
    # COMPILE alone and LINK alone.
    expect_tree(compile_only "${compile_a}" - "${compile_a}" - - -)
    expect_tree(link_only - "${link_a}" - "${link_a}" - -)
    # A target made after the call has no option: the rule the caller must keep.
    expect_tree(late "${compile_a}" - "${compile_a}" - - -)
    expect(late_late_exe - -)
    # Two calls add up, in call order.
    expect_tree(twice "${compile_a};${compile_b}" - "${compile_a};${compile_b}" - "${compile_b}" -)
endif()

# ── a call that must be refused ──────────────────────────────────────────────
# Each is a stub of its own with no language, so that it needs no compiler and configures in a moment. It must fail, and its
# message must name what is wrong.
set(refused_calls
    "no option at all|flywheel_dag_add_target_options()|needs COMPILE or LINK"
    "COMPILE with nothing after it|flywheel_dag_add_target_options(COMPILE)|COMPILE has no option"
    "IN with nothing after it|flywheel_dag_add_target_options(IN COMPILE -Wall)|IN has no directory"
    "a word that is no keyword|flywheel_dag_add_target_options(BOGUS x COMPILE -Wall)|unknown argument BOGUS x"
    "a directory that does not exist|flywheel_dag_add_target_options(IN nowhere COMPILE -Wall)|nowhere is not a directory")
set(refused 0)
foreach(entry IN LISTS refused_calls)
    string(REPLACE "|" ";" parts "${entry}")
    list(GET parts 0 label)
    list(GET parts 1 call)
    list(GET parts 2 needle)
    math(EXPR refused "${refused} + 1")
    set(dir "${SCRATCH_DIR}/refused_${refused}")
    file(MAKE_DIRECTORY "${dir}")
    file(WRITE "${dir}/CMakeLists.txt"
         "cmake_minimum_required(VERSION 4.0)\nproject(stub NONE)\ninclude(\"${module}\")\n${call}\n")
    execute_process(
        COMMAND "${CMAKE_COMMAND}" -G "${GENERATOR}" -S "${dir}" -B "${dir}/build"
        RESULT_VARIABLE refused_rc OUTPUT_VARIABLE refused_out ERROR_VARIABLE refused_err)
    squash(refused_text "${refused_out} ${refused_err}")
    math(EXPR cases "${cases} + 1")
    if(refused_rc EQUAL 0)
        string(APPEND failures "\n  ${label}: ${call} configured, want it refused")
    elseif(NOT refused_text MATCHES "flywheel_dag_add_target_options")
        string(APPEND failures "\n  ${label}: the refusal does not name the function (${refused_rc}): ${refused_text}")
    elseif(NOT refused_text MATCHES "${needle}")
        string(APPEND failures "\n  ${label}: the refusal does not say '${needle}': ${refused_text}")
    endif()
endforeach()

# ── the module and CMake 4.5 ─────────────────────────────────────────────────
if(CMAKE_VERSION VERSION_LESS 4.5)
    set(strict "the strict configure skipped: CMake ${CMAKE_VERSION} has no non-target-directive diagnostic, which came with 4.5")
elseif(rc EQUAL 0)
    configure(strict_rc strict_out "${stub}" "${SCRATCH_DIR}/strict" -Werror=author -Werror=deprecated)
    squash(strict_text "${strict_out}")
    math(EXPR cases "${cases} + 1")
    if(NOT strict_rc EQUAL 0 OR strict_text MATCHES "CMake (Error|Warning)")
        string(APPEND failures "\n  the stub did not configure cleanly under -Werror=author -Werror=deprecated (${strict_rc}): ${strict_text}")
    endif()
    set(strict "the stub configures under -Werror=author -Werror=deprecated on CMake ${CMAKE_VERSION}")
else()
    set(strict "the strict configure not run: the stub did not configure")
endif()

if(failures)
    message(FATAL_ERROR "test_target_options failed:${failures}")
endif()
file(REMOVE_RECURSE "${SCRATCH_DIR}")
message(STATUS "test_target_options: ok (${cases} cases; ${strict})")
