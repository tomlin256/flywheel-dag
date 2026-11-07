# flywheel_dag_platform.cmake — which platform a build tree is for, and whether
# the tree's name tells the truth about it.
#
# A CMake build tree belongs to one platform: it records the absolute path of
# its source, the compiler, the target and the flags. One checkout that is built
# natively on macOS and in a Linux container needs a tree for each, and a tree
# named for one platform but configured on another is a label that lies, found
# out later by a link error or a test that dies with "Exec format error". The
# root CMakeLists.txt calls the two functions below before project(), for the
# top-level project only, and stops the configure on a problem. They are pure,
# so test_platform_name can feed them any host.
#
# A tree is named <kind>-<os>-<arch>, under build/ by convention, and
# CMakePresets.json spells out the ones in use. <kind> is one lower-case word,
# usually a CMake build type, <os> is macos or linux, and <arch> is arm64 or
# x86_64. The tree's name is the last component of its path.
#
# A kind may also be a sanitizer, asan or tsan, and the tree must then be
# configured for it (FLYWHEEL_DAG_SANITIZE, flywheel_dag_sanitize.cmake): a tree
# named for a sanitizer that is not on is a label that lies, and its tests run
# uninstrumented and pass.

# flywheel_dag_platform(<out> <system-name> <machine>)
#
# Sets <out> to <os>-<arch> for a host. CMake's system name Darwin is macos and
# the others are lower-cased (Linux is linux). The machine is lower-cased, with
# aarch64 spelled arm64 and amd64 spelled x86_64, so the Mac and a Linux
# container on the same chip both say arm64.
function(flywheel_dag_platform out system_name machine)
    if(system_name STREQUAL "Darwin")
        set(os macos)
    else()
        string(TOLOWER "${system_name}" os)
    endif()

    string(TOLOWER "${machine}" arch)
    if(arch STREQUAL "aarch64")
        set(arch arm64)
    elseif(arch STREQUAL "amd64")
        set(arch x86_64)
    endif()

    set(${out} "${os}-${arch}" PARENT_SCOPE)
endfunction()

# flywheel_dag_tree_problem(<out> <name> <platform> <recorded> <sanitize>)
#
# Sets <out> to empty, or to the error text for the first problem with a tree
# called <name> on a host of <platform>. <recorded> is the platform the tree
# remembers from its first configure, or empty on a first configure. <sanitize>
# is the tree's FLYWHEEL_DAG_SANITIZE, or empty for none. There are three kinds
# of problem, and the guard refuses a false claim and nothing else:
#
#  - The name claims a platform that is not the host's. A name claims one when
#    it has the shape of a platform tree, an optional kind and then macos or
#    linux (release-linux-arm64, linux-x86_64, asan-macos). Such a name must be
#    [<kind>-]<os>-<arch> for the host, and anything after the architecture is
#    free (release-linux-arm64-gcc13), so a truthful name for a variant passes.
#    Names are compared in lower case, because the bind mount a container sees
#    is case-insensitive. Any other name (build, scratch, release-linuxbrew)
#    claims nothing and is not examined.
#  - The name is a platform tree for the host whose kind is asan or tsan, and
#    <sanitize> is not that mode. The mode is compared exactly, as the option
#    compares it, and nothing else is required of a sanitizer tree:
#    release-linux-arm64 may be configured for a sanitizer, and a name that
#    claims no platform (asan-scratch) is not examined.
#  - The tree remembers a platform that is not the host's. That covers a name
#    that claims nothing, and two hosts that see the checkout at the same path,
#    which CMake's own check of the source path cannot tell apart.
function(flywheel_dag_tree_problem out name platform recorded sanitize)
    set(${out} "" PARENT_SCOPE)
    string(TOLOWER "${name}" lowered)

    # The name to suggest: the tree's own kind, or the project's default build.
    set(kind relwithdebinfo)
    if(lowered MATCHES "^([a-z][a-z0-9]*)-(macos|linux)(-|$)")
        set(kind "${CMAKE_MATCH_1}")
    endif()
    set(advice "A build tree belongs to one OS and one architecture: use \"${kind}-${platform}\" under build/ (cmake --list-presets shows the presets for this host).")

    if(lowered MATCHES "^([a-z][a-z0-9]*-)?(macos|linux)(-|$)")
        if(NOT lowered MATCHES "^([a-z][a-z0-9]*-)?${platform}(-|$)")
            string(REGEX MATCH "(macos|linux)(-[a-z0-9_]+)?" claimed "${lowered}")
            set(${out} "flywheel-dag: the build directory \"${name}\" is for ${claimed}, but this host is ${platform}. ${advice}" PARENT_SCOPE)
            return()
        elseif(lowered MATCHES "^(asan|tsan)-")
            set(mode "${CMAKE_MATCH_1}")
            if(NOT "${sanitize}" STREQUAL "${mode}")
                set(shown "${sanitize}")
                if("${shown}" STREQUAL "")
                    set(shown OFF)
                endif()
                set(${out} "flywheel-dag: the build directory \"${name}\" is named for the ${mode} sanitizer, but FLYWHEEL_DAG_SANITIZE is '${shown}'. A tree named for a sanitizer must be configured for it, or its tests run uninstrumented and pass for nothing: use -DFLYWHEEL_DAG_SANITIZE=${mode}, or name the tree for what it is." PARENT_SCOPE)
                return()
            endif()
        endif()
    endif()

    if(NOT "${recorded}" STREQUAL "" AND NOT "${recorded}" STREQUAL "${platform}")
        set(${out} "flywheel-dag: the build directory \"${name}\" was first configured on ${recorded}, but this host is ${platform}. ${advice}" PARENT_SCOPE)
        return()
    endif()
endfunction()
