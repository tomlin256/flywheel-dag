# check_crtp_access.cmake — test_crtp_access: the engine's three CRTP bases keep their
# constructors private to the class they are the base of (flywheel-dag#60).
#
# Run by ctest as
#
#   cmake -DCXX=<compiler> -DINCLUDE_DIR=<source>/include -DSPDLOG_SOURCE_DIR=<dir>
#         -DNLOHMANN_JSON_SOURCE_DIR=<dir> -DWORK_DIR=<dir> -P check_crtp_access.cmake
#
# and registered at the top level for GCC and Clang, whose -std= and -fsyntax-only it
# passes. OpNodeImpl<Derived>, NodeImpl<Derived> and StatefulNodeBase<Derived, ...> are
# meant to be derived with the derived class itself as Derived. A constructor that is
# public or protected lets `class Wrong : public OpNodeImpl<Other>` compile, and the
# mistake shows only when the two disagree at run time; with the constructor private
# and Derived its friend (clang-tidy's bugprone-crtp-constructor-accessibility), it
# stops compiling. The suite already derives the bases correctly, in the engine's nodes
# and in test_stateful_node_base, so no case here repeats that.
#
# The control compiles two real nodes, to show that the flags and include paths work.
# Each other case derives a base with a real class that is not the one the base is for,
# gives the new class a constructor of its own, and must fail with an access error: a
# case that compiles has found a constructor that a stranger can reach, and one that
# fails for another reason has not shown that it refuses.

cmake_minimum_required(VERSION 4.0)

foreach(var IN ITEMS CXX INCLUDE_DIR SPDLOG_SOURCE_DIR NLOHMANN_JSON_SOURCE_DIR WORK_DIR)
    if(NOT ${var})
        message(FATAL_ERROR "usage: cmake -DCXX=<compiler> -DINCLUDE_DIR=<dir> -DSPDLOG_SOURCE_DIR=<dir> "
                            "-DNLOHMANN_JSON_SOURCE_DIR=<dir> -DWORK_DIR=<dir> -P check_crtp_access.cmake")
    endif()
endforeach()

file(REMOVE_RECURSE "${WORK_DIR}")
file(MAKE_DIRECTORY "${WORK_DIR}")

set(preamble [[
#include <flywheel/dag_ops.hpp>
#include <flywheel/dag_timeseries.hpp>

#include <functional>

// Real classes the bases are for.
using RealOp = dag::ops::UnaryOpNode<double, std::negate<double>>;
using RealTs = dag::ts::EWMANode;
using RealTsBase = dag::ts::StatefulNodeBase<RealTs, double, double, dag::ts::EWMANodeState>;
]])

set(body_control [[
void control() {
    auto op = RealOp::make("op", nullptr);
    auto ts = RealTs::make("ts", nullptr, 0.5);
}
]])
set(body_op_node_impl [[
struct Bad : dag::ops::OpNodeImpl<RealOp> { Bad() {} };
]])
set(body_node_impl [[
struct Bad : dag::ts::NodeImpl<RealTs> { Bad() {} };
]])
set(body_stateful_node_base [[
struct Bad : RealTsBase { Bad() : RealTsBase("bad", nullptr) {} };
]])

set(problems "")
foreach(case IN ITEMS control op_node_impl node_impl stateful_node_base)
    set(unit "${WORK_DIR}/${case}.cpp")
    file(WRITE "${unit}" "${preamble}\n${body_${case}}")
    execute_process(
        COMMAND "${CXX}" -std=c++23 -fsyntax-only -DSPDLOG_COMPILED_LIB
                "-I${INCLUDE_DIR}" "-I${SPDLOG_SOURCE_DIR}/include" "-I${NLOHMANN_JSON_SOURCE_DIR}/include"
                "${unit}"
        RESULT_VARIABLE result
        ERROR_VARIABLE  errors
        OUTPUT_QUIET)
    string(SUBSTRING "${errors}" 0 400 shown)
    if(case STREQUAL "control")
        if(NOT result EQUAL 0)
            string(APPEND problems "  the control does not compile, so the cases below prove nothing:\n${shown}\n")
        endif()
    elseif(result EQUAL 0)
        string(APPEND problems "  ${case}: compiled; a class that is not Derived can reach the constructor\n")
    elseif(NOT errors MATCHES "private")
        string(APPEND problems "  ${case}: failed, but not with an access error:\n${shown}\n")
    endif()
endforeach()

if(NOT problems STREQUAL "")
    message(FATAL_ERROR "a CRTP base does not keep its constructor private to Derived:\n${problems}")
endif()
message(STATUS "OpNodeImpl, NodeImpl and StatefulNodeBase refuse a class that is not their Derived")
