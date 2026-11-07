// =============================================================================
// flywheel-dag — A header-only C++23 reactive DAG computation engine
//
// Copyright (c) 2026 Rob Tomlin
//
// Licensed under the MIT License. See LICENSE file in the project root for
// full license information.
// =============================================================================

// A fixture: comments that state the present, and one that cites an open issue the run names.
#pragma once

// The square of x.
inline int square(int x) { return x * x; }

// The sum is not synchronised: flywheel-dag#31 tracks the missing thread contract.
inline int sum(int a, int b) { return a + b; }
