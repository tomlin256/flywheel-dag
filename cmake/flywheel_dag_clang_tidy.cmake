# flywheel_dag_clang_tidy.cmake — the clang-tidy gate: which sources are its units, and the tests that hold them to .clang-tidy.
#
# A unit is a C++ source of this project that a build compiles: a .cpp, .cc or .cxx under the project's root, not under the build
# tree, and not under a directory called _deps, which is where a dependency's sources sit wherever its build tree is. The gate
# holds one clang_tidy_ test to each unit, and a unit that is not in the tree's compile database would be analysed with flags that
# clang-tidy guesses from its neighbours' entries, not the flags a build uses.
#
#   flywheel_dag_clang_tidy_select     the one rule for what a unit is. It reads no disk, so tests/check_clang_tidy_module.cmake runs
#                                      it under `cmake -P` over a table (test_clang_tidy_module), and the gate's own units test runs
#                                      it over the compile database.
#   _flywheel_dag_clang_tidy_walk      the candidates: the sources of the project's targets.
#   flywheel_dag_clang_tidy_add_tests  the gate. The root calls it after its last add_subdirectory, because the candidates are the
#                                      sources of the targets made by then (test_clang_tidy_wiring).

# flywheel_dag_clang_tidy_select(<root> <build> <candidates> <units> <problem>)
#
# Sets <units> to the units among <candidates>, a list of absolute paths: the .cpp, .cc and .cxx files under <root>, less those
# under <build> (generated sources, and a dependency fetched into the tree) and those under a directory called _deps on the way
# from <root> (a dependency's, in this tree or another's). A path is normalised first, so one with a dot segment is the file it
# names, and the units are sorted and each once. Headers are not units: clang-tidy reaches them through the sources that include
# them. The test for a directory called _deps is for a name and not a substring, and is measured from <root>, so a checkout that
# is itself under a _deps is not held against its own sources.
#
# Sets <problem> to empty, or to why the gate cannot use the answer, and then <units> is empty. There are three problems: a root or
# a build tree that is empty, since a missing value is never guessed; no unit at all, since a gate over no file passes on nothing;
# and two units that would be one test name, since clang_tidy_<stem> names a unit by its file name without the last extension
# (tests/a.cpp and examples/a.cpp are both clang_tidy_a), and a test dropped silently is a file nobody checks.
function(flywheel_dag_clang_tidy_select root build candidates units_out problem_out)
    set(${units_out} "" PARENT_SCOPE)
    set(${problem_out} "" PARENT_SCOPE)
    if("${root}" STREQUAL "" OR "${build}" STREQUAL "")
        set(${problem_out}
            "the unit filter needs a root and a build tree, and was given root '${root}' and build tree '${build}'"
            PARENT_SCOPE)
        return()
    endif()

    set(base "${root}")
    cmake_path(NORMAL_PATH base)
    set(build_base "${build}")
    cmake_path(NORMAL_PATH build_base)

    set(selected "")
    foreach(candidate IN LISTS candidates)
        set(path "${candidate}")
        cmake_path(NORMAL_PATH path)
        cmake_path(GET path EXTENSION LAST_ONLY extension)
        if(NOT extension MATCHES "^\\.(cpp|cc|cxx)$")
            continue()
        endif()
        cmake_path(IS_PREFIX base "${path}" under_root)
        if(NOT under_root)
            continue()
        endif()
        cmake_path(IS_PREFIX build_base "${path}" under_build)
        if(under_build)
            continue()
        endif()
        set(relative "${path}")
        cmake_path(RELATIVE_PATH relative BASE_DIRECTORY "${base}")
        if("/${relative}/" MATCHES "/_deps/")
            continue()
        endif()
        list(APPEND selected "${path}")
    endforeach()
    list(REMOVE_DUPLICATES selected)
    list(SORT selected)

    list(LENGTH selected found)
    if(found EQUAL 0)
        list(LENGTH candidates given)
        set(${problem_out}
            "found no .cpp, .cc or .cxx source under ${root} among ${given} candidate(s): a clang-tidy gate over no files passes on nothing"
            PARENT_SCOPE)
        return()
    endif()

    set(stems "")
    set(owners "")
    foreach(source IN LISTS selected)
        cmake_path(GET source STEM LAST_ONLY stem)
        list(FIND stems "${stem}" at)
        if(NOT at EQUAL -1)
            list(GET owners ${at} owner)
            set(${problem_out} "${owner} and ${source} would both be the test clang_tidy_${stem}" PARENT_SCOPE)
            return()
        endif()
        list(APPEND stems "${stem}")
        list(APPEND owners "${source}")
    endforeach()
    set(${units_out} "${selected}" PARENT_SCOPE)
endfunction()

# _flywheel_dag_clang_tidy_walk(<directory> <out>)
#
# Sets <out> to the sources of the executable, static, shared, module and object library targets made in <directory> and in every
# directory below it, each as an absolute, normalised path (a source named relative is taken against its target's source directory),
# once for each target that lists it. Other targets are skipped: nothing compiles the sources of a custom target. So is a source
# inside a generator expression, which has no value until generation; such a source is in the compile database and not here, and
# clang_tidy_units fails on it by name. It reads the properties of directories and targets, so it needs a project.
function(_flywheel_dag_clang_tidy_walk directory out)
    set(sources "")
    get_property(targets DIRECTORY "${directory}" PROPERTY BUILDSYSTEM_TARGETS)
    foreach(target IN LISTS targets)
        get_target_property(type ${target} TYPE)
        if(NOT type MATCHES "^(EXECUTABLE|STATIC_LIBRARY|SHARED_LIBRARY|MODULE_LIBRARY|OBJECT_LIBRARY)$")
            continue()
        endif()
        get_target_property(listed ${target} SOURCES)
        if(NOT listed)
            continue()
        endif()
        get_target_property(target_directory ${target} SOURCE_DIR)
        foreach(source IN LISTS listed)
            if(source MATCHES "\\$<")
                continue()
            endif()
            cmake_path(ABSOLUTE_PATH source BASE_DIRECTORY "${target_directory}" NORMALIZE)
            list(APPEND sources "${source}")
        endforeach()
    endforeach()
    get_property(subdirectories DIRECTORY "${directory}" PROPERTY SUBDIRECTORIES)
    foreach(subdirectory IN LISTS subdirectories)
        _flywheel_dag_clang_tidy_walk("${subdirectory}" below)
        list(APPEND sources ${below})
    endforeach()
    set(${out} "${sources}" PARENT_SCOPE)
endfunction()

# flywheel_dag_clang_tidy_add_tests(<tool> <root> <extra>)
#
# Registers the gate: clang-tidy over this project's own sources, as tests. <tool> is the command FLYWHEEL_DAG_CLANG_TIDY resolved
# to (the root has already refused a configure that asked for one it cannot run, so nothing here checks again), <root> is the
# project's root and <extra> a list of arguments for every run. Call it where every target of the project has been made: the units
# are the sources of the targets under <root>. Nothing is built: clang-tidy reads compile_commands.json and the sources the build
# would compile, and FetchContent has put the dependencies' headers in place by then.
#
#   clang_tidy_<stem>    one per unit, <stem> being the file name less its last extension. A source that no target compiles is not a
#                        unit and has no test; test_clang_tidy_wiring fails on one in tests/, examples/ or benchmarks/.
#   clang_tidy_units     the registered units are the units of the tree's compile database (tests/check_clang_tidy_units.cmake).
#   clang_tidy_version   the tool is LLVM 23. The Mac, the Ubuntu container and CI run one major, and this is what stops them
#                        drifting apart unseen. Moving to another is one commit: this number, the workflow, the Ubuntu image, and
#                        whatever the new major reports.
#   clang_tidy_checks    .clang-tidy enables the families it should, and none that it leaves out:
#                        bugprone-easily-swappable-parameters, bugprone-narrowing-conversions, readability-* and modernize-*.
#   clang_tidy_canary_*  three proofs that the gate bites, on fixtures that are in no compile database.
#
# A lint gate that silently checks nothing passes CI, which is why the gate itself is tested.
function(flywheel_dag_clang_tidy_add_tests tool root extra)
    set(_flywheel_dag_clang_tidy_major 23)

    _flywheel_dag_clang_tidy_walk("${root}" candidates)
    flywheel_dag_clang_tidy_select("${root}" "${PROJECT_BINARY_DIR}" "${candidates}" units problem)
    if(NOT "${problem}" STREQUAL "")
        message(FATAL_ERROR "the clang-tidy gate cannot list its units: ${problem}")
    endif()

    # The compile commands carry -Werror=deprecated-declarations (CMakeLists.txt). With it, a clang-tidy newer than the SDK's
    # libc++ stops 21 units on nlohmann/json 3.12.0's CBOR declarations, which name std::char_traits<unsigned char>, a
    # specialization that libc++ deprecates: the error is in the SDK's headers, from a use in a dependency's. Here a
    # deprecation is a warning, --warnings-as-errors makes it an error where it is in this project's own files, and the
    # header filter drops it in the SDK's and the dependencies'.
    set(args --warnings-as-errors=* --extra-arg=-Wno-error=deprecated-declarations ${extra})
    if(APPLE AND NOT CMAKE_OSX_SYSROOT)
        # /usr/bin/c++ finds the SDK itself, so the compile commands carry no -isysroot, and a clang-tidy that is not
        # Apple's cannot find the C library's headers without one.
        execute_process(COMMAND xcrun --show-sdk-path
                        OUTPUT_VARIABLE sdk OUTPUT_STRIP_TRAILING_WHITESPACE
                        RESULT_VARIABLE sdk_status)
        if(NOT sdk_status EQUAL 0 OR NOT sdk)
            message(FATAL_ERROR "xcrun --show-sdk-path names no SDK, and clang-tidy needs one on this host")
        endif()
        list(APPEND args --extra-arg=-isysroot --extra-arg=${sdk})
    endif()

    foreach(unit IN LISTS units)
        cmake_path(GET unit STEM LAST_ONLY stem)
        add_test(NAME clang_tidy_${stem}
            COMMAND ${tool} -p ${PROJECT_BINARY_DIR} --quiet ${args} ${unit})
    endforeach()

    # The units go to the script bar-separated, because a semicolon would split them into arguments of the command.
    string(REPLACE ";" "|" registered "${units}")
    add_test(NAME clang_tidy_units
        COMMAND ${CMAKE_COMMAND} -DSOURCE_DIR=${root} -DBUILD_DIR=${PROJECT_BINARY_DIR} "-DREGISTERED=${registered}"
                -P ${root}/tests/check_clang_tidy_units.cmake)

    add_test(NAME clang_tidy_version COMMAND ${tool} --version)
    set_tests_properties(clang_tidy_version PROPERTIES
        PASS_REGULAR_EXPRESSION "LLVM version ${_flywheel_dag_clang_tidy_major}\\.")

    add_test(NAME clang_tidy_checks
        COMMAND ${tool} --config-file=${root}/.clang-tidy --list-checks)
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
    set(canary ${PROJECT_BINARY_DIR}/clang_tidy_canary)
    configure_file(${root}/tests/clang_tidy_canary/canary.cpp
                   ${canary}/canary.cpp COPYONLY)
    configure_file(${root}/tests/clang_tidy_canary/canary_engine.hpp
                   ${canary}/include/flywheel/canary_engine.hpp COPYONLY)
    configure_file(${root}/tests/clang_tidy_canary/canary_dep.hpp
                   ${canary}/_deps/canary-src/include/canary_dep.hpp COPYONLY)
    set(canary_command
        ${tool} --config-file=${root}/.clang-tidy ${args}
        ${canary}/canary.cpp -- -std=c++23
        -I${canary}/include -I${canary}/_deps/canary-src/include)
    set(canary_source_error
        "canary\\.cpp:[0-9]+:[0-9]+: error: [^\n]*\\[bugprone-use-after-move[],]")
    add_test(NAME clang_tidy_canary_source COMMAND ${canary_command})
    set_tests_properties(clang_tidy_canary_source PROPERTIES
        PASS_REGULAR_EXPRESSION "${canary_source_error}")
    add_test(NAME clang_tidy_canary_engine_header COMMAND ${canary_command})
    set_tests_properties(clang_tidy_canary_engine_header PROPERTIES
        PASS_REGULAR_EXPRESSION
            "include/flywheel/canary_engine\\.hpp:[0-9]+:[0-9]+: error: [^\n]*\\[performance-unnecessary-value-param[],]")
    add_test(NAME clang_tidy_canary_dependency_header COMMAND ${canary_command})
    set_tests_properties(clang_tidy_canary_dependency_header PROPERTIES
        PASS_REGULAR_EXPRESSION "${canary_source_error}"
        FAIL_REGULAR_EXPRESSION "canary_dep\\.hpp")
endfunction()
