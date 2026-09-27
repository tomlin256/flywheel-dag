// =============================================================================
// flywheel-dag — A header-only C++17 reactive DAG computation engine
//
// Copyright (c) 2026 Rob Tomlin
//
// Licensed under the MIT License. See LICENSE file in the project root for
// full license information.
// =============================================================================

#pragma once
// dag_compute_module.hpp — IComputeModule interface for composable DAG subgraphs
//
// A compute module encapsulates a self-contained subset of the reactive DAG:
// the nodes it builds, the upstream inputs it consumes (injected at
// construction), and any snapshot state it maintains for the reporting thread.
//
// Usage:
//   auto mod = MyModule::make(inputs..., cfg);
//   engine.install(mod);    // calls mod->wire(engine) exactly once
//   engine.run();
//
// After install(), output node handles (accessors on the concrete module type)
// are valid and may be passed to downstream module constructors.
//
// Design notes
// ────────────
// • IComputeModule only forward-declares Engine to avoid a circular include:
//   dag_engine.hpp includes this file, and this file must not include
//   dag_engine.hpp.  Concrete implementations #include dag_engine.hpp
//   themselves so they can call engine.addOutput() and friends inside wire().
//
// • wire() is the only method that touches the Engine.  All internal node
//   construction happens inside wire() so that output handles remain null
//   (and thus unusable) until the module has been properly installed.

#include "dag_state_store.hpp"
#include <memory>
#include <string>
#include <vector>

namespace dag::async {

class Engine;  // forward declaration — include dag_engine.hpp for the full API

// ─────────────────────────────────────────────────────────────────────────────
// IComputeModule
//
// Contract:
//   • wire() is called exactly once, by Engine::install().
//   • Modules must not call wire() themselves.
//   • Modules build their internal nodes and register engine outputs inside
//     wire().  After wire() returns, all node accessors are valid.
//   • Module output node handles may be passed into downstream module
//     constructors after install() returns on the upstream module.
// ─────────────────────────────────────────────────────────────────────────────
class IComputeModule {
public:
    virtual ~IComputeModule() = default;

    /// Unique name for this module instance (used for node naming and logging).
    virtual std::string name() const = 0;

    /// Called by Engine::install(). Build internal nodes, wire them to upstream
    /// inputs provided at construction, and register output callbacks on the
    /// engine.
    ///
    /// State persistence is automatic: Engine::saveState / restoreState walk
    /// the DAG from every registered output via INode::inputs() and find every
    /// reachable IStatefulNode. To exclude a stateful node, override
    /// IStatefulNode::persistState() on that node to return false.
    virtual void wire(Engine& engine) = 0;
};

} // namespace dag::async
