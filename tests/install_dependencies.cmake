# install_dependencies.cmake — the install_dependencies fixture: builds spdlog
# and nlohmann/json from the sources the engine's build fetched, and installs
# them into an empty prefix, the way a package manager provides them
# (flywheel-dag#2). test_consumer_package finds them there through the
# engine's package config.
#
# Run by ctest as
#
#   cmake -DSPDLOG_SOURCE_DIR=<dir> -DNLOHMANN_JSON_SOURCE_DIR=<dir>
#         -DBINARY_DIR=<dir> -DPREFIX=<dir> -DGENERATOR=<generator>
#         [-DCONFIG=<config>] -P install_dependencies.cmake
#
# The sources are already on disk, so no network is needed. The prefix is
# emptied first; the build directories under BINARY_DIR are kept, so a later
# run rebuilds only what changed.

cmake_minimum_required(VERSION 3.18)

foreach(var IN ITEMS SPDLOG_SOURCE_DIR NLOHMANN_JSON_SOURCE_DIR BINARY_DIR PREFIX GENERATOR)
    if(NOT ${var})
        message(FATAL_ERROR "usage: cmake -DSPDLOG_SOURCE_DIR=<dir> "
                            "-DNLOHMANN_JSON_SOURCE_DIR=<dir> -DBINARY_DIR=<dir> "
                            "-DPREFIX=<dir> -DGENERATOR=<generator> [-DCONFIG=<config>] "
                            "-P install_dependencies.cmake")
    endif()
endforeach()

file(REMOVE_RECURSE "${PREFIX}")

set(config_args "")
if(CONFIG)
    set(config_args --config "${CONFIG}")
endif()

function(run)
    execute_process(COMMAND ${ARGN} RESULT_VARIABLE result)
    if(NOT result EQUAL 0)
        list(JOIN ARGN " " command)
        message(FATAL_ERROR "failed (${result}): ${command}")
    endif()
endfunction()

# Configure, build and install one dependency. Built as the top-level project,
# each installs itself by default.
function(install_dependency name source_dir)
    set(build_dir "${BINARY_DIR}/${name}")
    run("${CMAKE_COMMAND}" -S "${source_dir}" -B "${build_dir}" -G "${GENERATOR}"
        "-DCMAKE_BUILD_TYPE=${CONFIG}" "-DCMAKE_INSTALL_PREFIX=${PREFIX}" ${ARGN})
    run("${CMAKE_COMMAND}" --build "${build_dir}" ${config_args})
    run("${CMAKE_COMMAND}" --install "${build_dir}" ${config_args})
endfunction()

# Neither needs its example or its tests.
install_dependency(spdlog "${SPDLOG_SOURCE_DIR}" -DSPDLOG_BUILD_EXAMPLE=OFF)
install_dependency(nlohmann_json "${NLOHMANN_JSON_SOURCE_DIR}" -DJSON_BuildTests=OFF)
