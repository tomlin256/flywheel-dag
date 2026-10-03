# check_install.cmake — test_install: installs this build into an empty prefix,
# the way a user would, and checks what lands there.
#
# Run by ctest as
#
#   cmake -DBINARY_DIR=<build> -DSOURCE_DIR=<source> -DPREFIX=<dir>
#         -DINCLUDEDIR=<dir> -DDATADIR=<dir> [-DCONFIG=<config>]
#         -P check_install.cmake
#
# where INCLUDEDIR and DATADIR are the build's CMAKE_INSTALL_INCLUDEDIR and
# CMAKE_INSTALL_DATADIR, relative to the prefix.
#
# The install is the engine alone. The prefix must hold every file under
# include/flywheel/, the three package config files and the LICENSE file, and
# nothing else. The headers expected are listed from the source tree, so a
# header the install rules leave out (one with another extension, say) is
# reported. The prefix is emptied first, so a file from an earlier run can
# satisfy neither this check nor a consumer test that finds the package there.

cmake_minimum_required(VERSION 3.18)

foreach(var IN ITEMS BINARY_DIR SOURCE_DIR PREFIX INCLUDEDIR DATADIR)
    if(NOT ${var})
        message(FATAL_ERROR "usage: cmake -DBINARY_DIR=<build> -DSOURCE_DIR=<source> "
                            "-DPREFIX=<dir> -DINCLUDEDIR=<dir> -DDATADIR=<dir> "
                            "[-DCONFIG=<config>] -P check_install.cmake")
    endif()
endforeach()

file(REMOVE_RECURSE "${PREFIX}")

set(config_args "")
if(CONFIG)
    set(config_args --config "${CONFIG}")
endif()
execute_process(
    COMMAND "${CMAKE_COMMAND}" --install "${BINARY_DIR}" --prefix "${PREFIX}" ${config_args}
    RESULT_VARIABLE result)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "cmake --install ${BINARY_DIR} failed: ${result}")
endif()

# A hidden file, such as a .DS_Store, is never a header.
file(GLOB headers RELATIVE "${SOURCE_DIR}/include" "${SOURCE_DIR}/include/flywheel/*")
list(FILTER headers EXCLUDE REGEX "/\\.[^/]*$")
list(TRANSFORM headers PREPEND "${INCLUDEDIR}/")
set(expected
    ${headers}
    ${DATADIR}/cmake/flywheel_dag/flywheel_dagConfig.cmake
    ${DATADIR}/cmake/flywheel_dag/flywheel_dagConfigVersion.cmake
    ${DATADIR}/cmake/flywheel_dag/flywheel_dagTargets.cmake
    ${DATADIR}/doc/flywheel_dag/LICENSE)

file(GLOB_RECURSE installed RELATIVE "${PREFIX}" "${PREFIX}/*")
if(NOT installed)
    message(FATAL_ERROR "cmake --install put nothing into ${PREFIX}")
endif()

set(missing ${expected})
list(REMOVE_ITEM missing ${installed})
set(unexpected ${installed})
list(REMOVE_ITEM unexpected ${expected})

if(missing OR unexpected)
    list(JOIN missing "\n  " missing_report)
    list(JOIN unexpected "\n  " unexpected_report)
    message(FATAL_ERROR "${PREFIX} does not hold the engine alone.\n"
                        "Not installed:\n  ${missing_report}\n"
                        "Installed, but not the engine's:\n  ${unexpected_report}")
endif()

list(LENGTH installed count)
message(STATUS "${PREFIX} holds the engine alone: ${count} files")
