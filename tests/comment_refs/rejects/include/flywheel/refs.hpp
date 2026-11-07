// =============================================================================
// flywheel-dag — A header-only C++23 reactive DAG computation engine
//
// Copyright (c) 2026 Rob Tomlin
//
// Licensed under the MIT License. See LICENSE file in the project root for
// full license information.
// =============================================================================

// A fixture whose comments cite issues the run does not name.
#pragma once

// The queue was flushed twice; fixed in flywheel-dag#12.
inline int first(const int values[]) { return values[0]; }
// A second reference, after semicolons and brackets: flywheel-dag#13.
// By address: https://github.com/example/flywheel-dag/issues/21
// A pull request: https://github.com/example/flywheel-dag/pull/22
// The number is longer than an allowed one: flywheel-dag#310.
