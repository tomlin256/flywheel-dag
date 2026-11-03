// =============================================================================
// flywheel-dag — A header-only C++23 reactive DAG computation engine
//
// Copyright (c) 2026 Rob Tomlin
//
// Licensed under the MIT License. See LICENSE file in the project root for
// full license information.
// =============================================================================

// canary_dep.hpp — a header that clang-tidy must not report on, because it sits where a dependency's would
// (flywheel-dag#60). See canary.cpp.

#pragma once

#include <cstddef>
#include <string>

// The same defect as canary_engine.hpp: performance-unnecessary-value-param, if the path filter let it through.
inline std::size_t canary_dep_size(std::string s) { return s.size(); }
