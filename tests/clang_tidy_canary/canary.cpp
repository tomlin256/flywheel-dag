// =============================================================================
// flywheel-dag — A header-only C++23 reactive DAG computation engine
//
// Copyright (c) 2026 Rob Tomlin
//
// Licensed under the MIT License. See LICENSE file in the project root for
// full license information.
// =============================================================================

// canary.cpp — a unit with one defect of its own, which clang-tidy must report (flywheel-dag#60).
//
// The clang_tidy_canary_* tests run clang-tidy on this file, with the real .clang-tidy, after configure has copied
// it and the two headers under the build tree. It is in no compile database and no target, so the gate over the
// project's own sources never sees it. It proves the gate bites:
//
//   - here: a string read after it was moved, which bugprone-use-after-move reports as an error;
//   - canary_engine.hpp, copied to .../include/flywheel/: the same unit includes a header whose path the header
//     filter admits as the engine's, with a defect of its own;
//   - canary_dep.hpp, copied to .../_deps/canary-src/include/: a header with that defect at a path the filter must
//     refuse, included by -I as spdlog is.

#include "canary_dep.hpp"
#include "flywheel/canary_engine.hpp"

#include <string>
#include <utility>

int main() {
    std::string s = "x";
    std::string t = std::move(s);
    return static_cast<int>(s.size() + t.size() + canary_engine_size("e") + canary_dep_size("d"));
}
