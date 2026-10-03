// =============================================================================
// flywheel-dag — A header-only C++17 reactive DAG computation engine
//
// Copyright (c) 2026 Rob Tomlin
//
// Licensed under the MIT License. See LICENSE file in the project root for
// full license information.
// =============================================================================

#pragma once
// dag_state_store.hpp — Snapshot/restore interfaces for stateful DAG nodes
//
// Three interfaces and two stores that let a restart resume from saved state
// instead of starting cold:
//
//   INodeState         — per-node key/value state bag with typed access.
//                        Nodes never see JSON; they only see this interface.
//   IStatefulNode      — mixin that stateful time-series nodes implement.
//                        Declares saveState / restoreState against INodeState.
//   IStateStore        — abstraction over the serialization medium.
//                        Engine::saveState / restoreState delegate to this.
//   InMemoryStateStore — in-process store for tests; no filesystem, no JSON.
//   JsonFileStateStore — production store; atomic JSON file write/read.
//
// dag_timeseries.hpp includes this header, so this header must not include it.

#include "dag.hpp"

#include <any>
#include <filesystem>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace dag {

// =============================================================================
// INodeState
//
// Per-node key/value state bag passed to IStatefulNode::saveState and
// restoreState. Backed by the concrete store; nodes never see the backing type.
//
// CONTRACT:  Value& returned by operator[] is owned by this INodeState and
// valid only for the lifetime of this object. Do not store the reference.
// =============================================================================
class INodeState {
public:
    virtual ~INodeState() = default;

    // ── Value proxy ──────────────────────────────────────────────────────────
    // Returned by operator[]. Supports typed assignment (write path) and
    // typed extraction via as<T>() (read path).
    struct Value {
        virtual ~Value() = default;

        virtual Value& operator=(double)                     = 0;
        virtual Value& operator=(bool)                       = 0;
        virtual Value& operator=(std::size_t)                = 0;
        virtual Value& operator=(const std::vector<double>&) = 0;

        /// Typed extraction. Explicit specializations defined in the .inl
        /// dispatch to the protected virtual getters below.
        template<typename T> T as() const;

    protected:
        virtual double              asDouble()    const = 0;
        virtual bool                asBool()      const = 0;
        virtual std::size_t         asSizeT()     const = 0;
        virtual std::vector<double> asVecDouble() const = 0;
    };

    virtual Value&       operator[](std::string_view key)       = 0;
    virtual const Value& operator[](std::string_view key) const = 0;

    /// Return a scoped sub-bag for a named child.
    /// Used when a node delegates serialisation to an owned inner node
    /// (e.g. ZScoreNode → sub("stats") → RollingStats).
    /// The sub-bag is owned by this INodeState and valid for its lifetime.
    virtual INodeState&       sub(std::string_view key)       = 0;
    virtual const INodeState& sub(std::string_view key) const = 0;
};

// =============================================================================
// IStatefulNode
//
// Mixin for DAG nodes that accumulate state over time.
//
// NAME CONTRACT: node names (INode::name()) must be unique across the entire
// DAG and stable across restarts. Do not embed timestamps, pointer addresses,
// or other runtime-variable data in node names.
// =============================================================================
class IStatefulNode {
public:
    virtual ~IStatefulNode() = default;

    /// Write accumulated state into the provided state bag.
    /// All keys written here must round-trip through restoreState without data
    /// loss.
    virtual void saveState(INodeState& state) const = 0;

    /// Restore state from the provided state bag.
    /// Throws std::runtime_error if a required key is absent (rather than
    /// silently leaving the node in a half-restored state).
    /// Must call this->invalidate() after restoring so downstream re-evaluates.
    virtual void restoreState(const INodeState& state) = 0;

    /// Opt-out for stateful nodes that should not be persisted.
    /// Default: persisted. Override to return false for diagnostic / transient
    /// nodes whose accumulated state has no meaning across restarts.
    virtual bool persistState() const { return true; }
};

using StatefulNodePtr = std::shared_ptr<IStatefulNode>;

// =============================================================================
// IStateStore
//
// Abstraction over the mechanism used to persist and restore stateful node
// state. The engine holds a shared_ptr<IStateStore> and calls save/restore
// without knowing anything about serialisation format or storage medium.
// =============================================================================
class IStateStore {
public:
    virtual ~IStateStore() = default;

    /// Persist state for all provided nodes.
    virtual void save(const std::vector<std::shared_ptr<IStatefulNode>>& nodes) = 0;

    /// Restore state into the provided nodes from a previously saved snapshot.
    /// Returns true  if a snapshot was available and applied.
    /// Returns false if no snapshot exists (first run — nodes left cold).
    /// Throws std::runtime_error on unrecoverable parse/version errors.
    virtual bool restore(const std::vector<std::shared_ptr<IStatefulNode>>& nodes) = 0;
};

// =============================================================================
// InMemoryStateStore
//
// In-process store for unit tests. Backed by an unordered_map of
// MapNodeState objects (each implementing INodeState with std::any values).
// No filesystem access, no JSON.
// =============================================================================
class InMemoryStateStore : public IStateStore {
public:
    InMemoryStateStore() = default;

    void save(const std::vector<std::shared_ptr<IStatefulNode>>& nodes) override;
    bool restore(const std::vector<std::shared_ptr<IStatefulNode>>& nodes) override;

    /// True if save() has been called since construction or the last reset().
    bool hasSavedState() const;

    /// Reset to empty — allows a test to call restore() before any save().
    void reset();

private:
    // ── MapValue —————————————————————————————————————————————————————————────
    // Concrete INodeState::Value backed by std::any.
    class MapValue : public INodeState::Value {
    public:
        Value& operator=(double v)                     override;
        Value& operator=(bool v)                       override;
        Value& operator=(std::size_t v)                override;
        Value& operator=(const std::vector<double>& v) override;

        bool hasValue() const { return data_.has_value(); }

    private:
        std::any data_;
        friend class MapNodeState;

        double              asDouble()    const override;
        bool                asBool()      const override;
        std::size_t         asSizeT()     const override;
        std::vector<double> asVecDouble() const override;
    };

    // ── MapNodeState ──────────────────────────────────────────────────────────
    // Concrete INodeState backed by unordered_maps.
    class MapNodeState : public INodeState {
    public:
        Value&       operator[](std::string_view key)       override;
        const Value& operator[](std::string_view key) const override;
        INodeState&       sub(std::string_view key)       override;
        const INodeState& sub(std::string_view key) const override;

    private:
        std::unordered_map<std::string, MapValue>                     values_;
        std::unordered_map<std::string, std::unique_ptr<MapNodeState>> subs_;
    };

    std::unordered_map<std::string, MapNodeState> store_;
    bool hasSaved_ = false;
};

// =============================================================================
// JsonFileStateStore
//
// Production store backed by a JSON file. Uses nlohmann::json internally; no
// JSON type appears in a public signature.
//
// save()    — atomic write: creates parent dirs, writes to <file>.tmp, then
//             renames into place.
// restore() — returns false if the file does not exist (cold start).
//             Throws std::runtime_error on malformed JSON or version mismatch.
//             Missing nodes are logged at warn level but do not abort restore.
// =============================================================================
class JsonFileStateStore : public IStateStore {
public:
    /// file — path to the snapshot file (e.g. "state/graph_state.json"). A
    /// relative path is taken from the working directory. The parent directory
    /// is created on first save if it does not exist. A bare filename has no
    /// parent directory, so it is written straight into the working directory.
    explicit JsonFileStateStore(std::filesystem::path file);

    void save(const std::vector<std::shared_ptr<IStatefulNode>>& nodes) override;
    bool restore(const std::vector<std::shared_ptr<IStatefulNode>>& nodes) override;

private:
    std::filesystem::path file_;
};

} // namespace dag

#include "dag_state_store.inl"
