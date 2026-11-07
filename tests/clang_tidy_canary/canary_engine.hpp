// =============================================================================
// flywheel-dag — A header-only C++23 reactive DAG computation engine
//
// Copyright (c) 2026 Rob Tomlin
//
// Licensed under the MIT License. See LICENSE file in the project root for
// full license information.
// =============================================================================

// canary_engine.hpp — a header that clang-tidy must report on when it sits under include/flywheel/.
// See canary.cpp.

#pragma once

#include <cstddef>
#include <string>

// The string is copied for each call and only read: performance-unnecessary-value-param.
inline std::size_t canary_engine_size(std::string s) { return s.size(); }
