# flywheel_dag_clang_tidy.cmake — which sources are the clang-tidy gate's units.
#
# A unit is a C++ source of this project that a build compiles: a .cpp, .cc or .cxx under the project's root, not under the build
# tree, and not under a directory called _deps, which is where a dependency's sources sit wherever its build tree is. The gate
# holds one clang_tidy_ test to each unit, and a unit that is not in the tree's compile database would be analysed with flags that
# clang-tidy guesses from its neighbours' entries, not the flags a build uses. The function below is the one rule for what a
# unit is, and it reads no disk, so tests/check_clang_tidy_module.cmake runs it under `cmake -P` over a table
# (test_clang_tidy_module).

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
