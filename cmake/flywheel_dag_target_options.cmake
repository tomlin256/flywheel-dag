# flywheel_dag_target_options.cmake — compile and link options for targets, not for directories.
#
# CMake 4.5 reports add_compile_options(), add_link_options(), include_directories(), link_directories(), link_libraries(),
# add_compile_definitions() and add_definitions() as non-target directives, and the strict configure that CI runs (-Werror=author)
# makes each report an error. A project that wants an option on every target of a directory tree, a dependency's included, gives
# the options to the targets themselves, and this is the one place that does it. tests/check_target_options.cmake
# (test_target_options) holds it.
#
# flywheel_dag_add_target_options([IN <directory>...] [COMPILE <option>...] [LINK <option>...])
#
#   Which targets: those that the current directory and its subdirectories made, or with IN only those of the named directories,
#   each relative to the current directory or absolute, and of the directories they added. A name that is no directory on disk is
#   an error. A directory that exists and that this tree never added, as the tests' is when they are off, contributes nothing.
#   An alias, an imported target, an INTERFACE library and a custom target have no translation unit or link step to give an option
#   to, and are left alone.
#
#   What they get, as PRIVATE options, so that nothing which links a target inherits them: COMPILE goes to executables and to
#   static, shared, module and object libraries, and LINK to executables, shared libraries and modules, because a static library
#   has no link step. At least one of COMPILE and LINK is required. Two calls add up, in call order.
#
#   Only the targets that exist when it is called get the options, so a caller calls it after the last target is made.

# _flywheel_dag_directory_targets(<out> <directory>)
#
# Sets <out> to every target that <directory> and the directories below it made, in the order CMake lists them.
function(_flywheel_dag_directory_targets out directory)
    set(pending "${directory}")
    set(found "")
    while(NOT "${pending}" STREQUAL "")
        list(POP_FRONT pending current)
        get_property(subdirectories DIRECTORY "${current}" PROPERTY SUBDIRECTORIES)
        list(APPEND pending ${subdirectories})
        get_property(made DIRECTORY "${current}" PROPERTY BUILDSYSTEM_TARGETS)
        list(APPEND found ${made})
    endwhile()
    set(${out} "${found}" PARENT_SCOPE)
endfunction()

# _flywheel_dag_tree_directories(<out> <directory>)
#
# Sets <out> to <directory> and every directory below it, as CMake spells them.
function(_flywheel_dag_tree_directories out directory)
    get_property(known DIRECTORY "${directory}" PROPERTY SOURCE_DIR)
    set(pending "${known}")
    set(found "")
    while(NOT "${pending}" STREQUAL "")
        list(POP_FRONT pending current)
        list(APPEND found "${current}")
        get_property(subdirectories DIRECTORY "${current}" PROPERTY SUBDIRECTORIES)
        list(APPEND pending ${subdirectories})
    endwhile()
    set(${out} "${found}" PARENT_SCOPE)
endfunction()

function(flywheel_dag_add_target_options)
    cmake_parse_arguments(PARSE_ARGV 0 arg "" "" "IN;COMPILE;LINK")
    if(arg_UNPARSED_ARGUMENTS)
        string(REPLACE ";" " " words "${arg_UNPARSED_ARGUMENTS}")
        message(FATAL_ERROR "flywheel_dag_add_target_options: unknown argument ${words}; the keywords are IN, COMPILE and LINK")
    endif()
    foreach(keyword IN LISTS arg_KEYWORDS_MISSING_VALUES)
        if(keyword STREQUAL "IN")
            set(what directory)
        else()
            set(what option)
        endif()
        message(FATAL_ERROR "flywheel_dag_add_target_options: ${keyword} has no ${what}")
    endforeach()
    if(NOT DEFINED arg_COMPILE AND NOT DEFINED arg_LINK)
        message(FATAL_ERROR "flywheel_dag_add_target_options: needs COMPILE or LINK, or both")
    endif()

    # The directories to start from: the current one, or each of those named that this tree holds.
    set(root "${CMAKE_CURRENT_SOURCE_DIR}")
    if(DEFINED arg_IN)
        _flywheel_dag_tree_directories(held "${root}")
        set(starts "")
        foreach(relative IN LISTS arg_IN)
            get_filename_component(directory "${relative}" ABSOLUTE BASE_DIR "${root}")
            if(NOT IS_DIRECTORY "${directory}")
                message(FATAL_ERROR "flywheel_dag_add_target_options: IN ${relative}: ${directory} is not a directory")
            endif()
            if(directory IN_LIST held)
                list(APPEND starts "${directory}")
            endif()
        endforeach()
    else()
        set(starts "${root}")
    endif()

    set(targets "")
    foreach(start IN LISTS starts)
        _flywheel_dag_directory_targets(made "${start}")
        list(APPEND targets ${made})
    endforeach()
    list(REMOVE_DUPLICATES targets)

    foreach(target IN LISTS targets)
        get_target_property(type ${target} TYPE)
        if(DEFINED arg_COMPILE AND type MATCHES "^(EXECUTABLE|STATIC_LIBRARY|SHARED_LIBRARY|MODULE_LIBRARY|OBJECT_LIBRARY)$")
            target_compile_options(${target} PRIVATE ${arg_COMPILE})
        endif()
        if(DEFINED arg_LINK AND type MATCHES "^(EXECUTABLE|SHARED_LIBRARY|MODULE_LIBRARY)$")
            target_link_options(${target} PRIVATE ${arg_LINK})
        endif()
    endforeach()
endfunction()
