// =============================================================================
// flywheel-dag — A header-only C++17 reactive DAG computation engine
//
// Copyright (c) 2026 Rob Tomlin
//
// Licensed under the MIT License. See LICENSE file in the project root for
// full license information.
// =============================================================================

// dag_state_store.inl — implementation of all dag_state_store.hpp declarations
// Included at the bottom of dag_state_store.hpp; never include this file directly.

#pragma once

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include <chrono>
#include <fstream>
#include <stdexcept>

namespace dag {

// =============================================================================
// INodeState::Value — as<T>() explicit specialisations
// =============================================================================

template<> inline double INodeState::Value::as<double>() const {
    return asDouble();
}
template<> inline bool INodeState::Value::as<bool>() const {
    return asBool();
}
template<> inline std::size_t INodeState::Value::as<std::size_t>() const {
    return asSizeT();
}
template<> inline std::vector<double> INodeState::Value::as<std::vector<double>>() const {
    return asVecDouble();
}

// =============================================================================
// InMemoryStateStore::MapValue
// =============================================================================

inline InMemoryStateStore::MapValue::Value&
InMemoryStateStore::MapValue::operator=(double v) {
    data_ = v; return *this;
}
inline InMemoryStateStore::MapValue::Value&
InMemoryStateStore::MapValue::operator=(bool v) {
    data_ = v; return *this;
}
inline InMemoryStateStore::MapValue::Value&
InMemoryStateStore::MapValue::operator=(std::size_t v) {
    data_ = v; return *this;
}
inline InMemoryStateStore::MapValue::Value&
InMemoryStateStore::MapValue::operator=(const std::vector<double>& v) {
    data_ = v; return *this;
}

inline double InMemoryStateStore::MapValue::asDouble() const {
    if (!data_.has_value())
        throw std::runtime_error("INodeState: key not set (reading as double)");
    return std::any_cast<double>(data_);
}
inline bool InMemoryStateStore::MapValue::asBool() const {
    if (!data_.has_value())
        throw std::runtime_error("INodeState: key not set (reading as bool)");
    return std::any_cast<bool>(data_);
}
inline std::size_t InMemoryStateStore::MapValue::asSizeT() const {
    if (!data_.has_value())
        throw std::runtime_error("INodeState: key not set (reading as size_t)");
    return std::any_cast<std::size_t>(data_);
}
inline std::vector<double> InMemoryStateStore::MapValue::asVecDouble() const {
    if (!data_.has_value())
        throw std::runtime_error("INodeState: key not set (reading as vector<double>)");
    return std::any_cast<std::vector<double>>(data_);
}

// =============================================================================
// InMemoryStateStore::MapNodeState
// =============================================================================

inline INodeState::Value&
InMemoryStateStore::MapNodeState::operator[](std::string_view key) {
    return values_[std::string(key)];
}

inline const INodeState::Value&
InMemoryStateStore::MapNodeState::operator[](std::string_view key) const {
    auto it = values_.find(std::string(key));
    if (it == values_.end() || !it->second.hasValue())
        throw std::runtime_error(
            "INodeState: key absent: '" + std::string(key) + "'");
    return it->second;
}

inline INodeState&
InMemoryStateStore::MapNodeState::sub(std::string_view key) {
    auto skey = std::string(key);
    auto& ptr = subs_[skey];
    if (!ptr) ptr = std::make_unique<MapNodeState>();
    return *ptr;
}

inline const INodeState&
InMemoryStateStore::MapNodeState::sub(std::string_view key) const {
    auto it = subs_.find(std::string(key));
    if (it == subs_.end() || !it->second)
        throw std::runtime_error(
            "INodeState: sub-bag absent: '" + std::string(key) + "'");
    return *it->second;
}

// =============================================================================
// InMemoryStateStore
// =============================================================================

inline void InMemoryStateStore::save(
    const std::vector<std::shared_ptr<IStatefulNode>>& nodes)
{
    store_.clear();
    for (auto& sn : nodes) {
        auto* n = dynamic_cast<const INode*>(sn.get());
        if (!n)
            throw std::runtime_error(
                "InMemoryStateStore::save: IStatefulNode is not an INode");
        sn->saveState(store_[n->name()]);
    }
    hasSaved_ = true;
}

inline bool InMemoryStateStore::restore(
    const std::vector<std::shared_ptr<IStatefulNode>>& nodes)
{
    if (!hasSaved_) return false;
    for (auto& sn : nodes) {
        auto* n = dynamic_cast<const INode*>(sn.get());
        if (!n)
            throw std::runtime_error(
                "InMemoryStateStore::restore: IStatefulNode is not an INode");
        auto it = store_.find(n->name());
        if (it == store_.end()) {
            spdlog::warn("InMemoryStateStore: no saved state for node '{}'", n->name());
            continue;
        }
        sn->restoreState(it->second);
    }
    return true;
}

inline bool InMemoryStateStore::hasSavedState() const { return hasSaved_; }
inline void InMemoryStateStore::reset() { store_.clear(); hasSaved_ = false; }

// =============================================================================
// JsonFileStateStore — private JsonNodeState implementation
// =============================================================================

namespace detail {

// JsonValue — proxy INodeState::Value backed by a nlohmann::json key
class JsonValue : public INodeState::Value {
public:
    JsonValue(nlohmann::json& obj, std::string key)
        : obj_(obj), key_(std::move(key)) {}

    INodeState::Value& operator=(double v) override {
        obj_[key_] = v; return *this;
    }
    INodeState::Value& operator=(bool v) override {
        obj_[key_] = v; return *this;
    }
    INodeState::Value& operator=(std::size_t v) override {
        obj_[key_] = v; return *this;
    }
    INodeState::Value& operator=(const std::vector<double>& v) override {
        obj_[key_] = v; return *this;
    }

private:
    double asDouble() const override {
        checkKey();
        return obj_[key_].get<double>();
    }
    bool asBool() const override {
        checkKey();
        return obj_[key_].get<bool>();
    }
    std::size_t asSizeT() const override {
        checkKey();
        return obj_[key_].get<std::size_t>();
    }
    std::vector<double> asVecDouble() const override {
        checkKey();
        return obj_[key_].get<std::vector<double>>();
    }

    void checkKey() const {
        if (!obj_.contains(key_))
            throw std::runtime_error(
                "INodeState: key absent: '" + key_ + "'");
    }

    nlohmann::json& obj_;
    std::string key_;
};

// JsonNodeState — INodeState backed by a nlohmann::json object reference
class JsonNodeState : public INodeState {
public:
    explicit JsonNodeState(nlohmann::json& obj) : obj_(obj) {}

    INodeState::Value& operator[](std::string_view key) override {
        return getOrCreate(std::string(key));
    }
    const INodeState::Value& operator[](std::string_view key) const override {
        return getOrCreate(std::string(key));
    }

    INodeState& sub(std::string_view key) override {
        auto skey = std::string(key);
        if (!obj_.contains(skey)) obj_[skey] = nlohmann::json::object();
        auto& ptr = subs_[skey];
        if (!ptr) ptr = std::make_unique<JsonNodeState>(obj_[skey]);
        return *ptr;
    }
    const INodeState& sub(std::string_view key) const override {
        auto skey = std::string(key);
        if (!obj_.contains(skey))
            throw std::runtime_error(
                "INodeState: sub-bag absent: '" + skey + "'");
        auto& ptr = subs_[skey];
        if (!ptr) ptr = std::make_unique<JsonNodeState>(obj_[skey]);
        return *ptr;
    }

private:
    JsonValue& getOrCreate(const std::string& key) const {
        auto it = proxies_.find(key);
        if (it != proxies_.end()) return *it->second;
        proxies_[key] = std::make_unique<JsonValue>(obj_, key);
        return *proxies_[key];
    }

    nlohmann::json& obj_;
    mutable std::unordered_map<std::string, std::unique_ptr<JsonValue>>     proxies_;
    mutable std::unordered_map<std::string, std::unique_ptr<JsonNodeState>> subs_;
};

} // namespace detail

// =============================================================================
// JsonFileStateStore
// =============================================================================

inline JsonFileStateStore::JsonFileStateStore(std::filesystem::path file)
    : file_(std::move(file)) {}

inline void JsonFileStateStore::save(
    const std::vector<std::shared_ptr<IStatefulNode>>& nodes)
{
    // ── Build JSON envelope ────────────────────────────────────────────────────
    nlohmann::json envelope;
    envelope["version"] = 1;

    // UTC timestamp (seconds precision is enough)
    auto now      = std::chrono::system_clock::now();
    auto t        = std::chrono::system_clock::to_time_t(now);
    char timebuf[32] = {};
    std::strftime(timebuf, sizeof(timebuf), "%Y-%m-%dT%H:%M:%SZ", std::gmtime(&t));
    envelope["saved_at"] = timebuf;

    nlohmann::json nodesJson = nlohmann::json::object();

    for (auto& sn : nodes) {
        auto* n = dynamic_cast<const INode*>(sn.get());
        if (!n)
            throw std::runtime_error(
                "JsonFileStateStore::save: IStatefulNode is not an INode");
        nlohmann::json nodeObj = nlohmann::json::object();
        detail::JsonNodeState state(nodeObj);
        sn->saveState(state);
        nodesJson[n->name()] = std::move(nodeObj);
    }
    envelope["nodes"] = std::move(nodesJson);

    // ── Atomic write (temp → rename) ───────────────────────────────────────────
    std::filesystem::create_directories(file_.parent_path());
    auto tmp = file_;
    tmp += ".tmp";
    {
        std::ofstream out(tmp);
        if (!out)
            throw std::runtime_error(
                "JsonFileStateStore: cannot open '" + tmp.string() + "' for writing");
        out << envelope.dump(2);
        if (!out)
            throw std::runtime_error(
                "JsonFileStateStore: write failed for '" + tmp.string() + "'");
    }
    std::filesystem::rename(tmp, file_);
    spdlog::info("JsonFileStateStore: saved {} nodes to '{}'", nodes.size(), file_.string());
}

inline bool JsonFileStateStore::restore(
    const std::vector<std::shared_ptr<IStatefulNode>>& nodes)
{
    if (!std::filesystem::exists(file_)) return false;

    // ── Parse ──────────────────────────────────────────────────────────────────
    nlohmann::json envelope;
    {
        std::ifstream in(file_);
        if (!in)
            throw std::runtime_error(
                "JsonFileStateStore: cannot open '" + file_.string() + "' for reading");
        try {
            in >> envelope;
        } catch (const nlohmann::json::exception& e) {
            throw std::runtime_error(
                std::string("JsonFileStateStore: malformed JSON in '")
                + file_.string() + "': " + e.what());
        }
    }

    if (!envelope.contains("version") || !envelope["version"].is_number_integer())
        throw std::runtime_error(
            "JsonFileStateStore: missing or invalid 'version' field in '"
            + file_.string() + "'");

    int version = envelope["version"].get<int>();
    if (version != 1)
        throw std::runtime_error(
            "JsonFileStateStore: unsupported snapshot version "
            + std::to_string(version) + " in '" + file_.string() + "'");

    if (!envelope.contains("nodes") || !envelope["nodes"].is_object())
        throw std::runtime_error(
            "JsonFileStateStore: missing 'nodes' object in '" + file_.string() + "'");

    const auto& nodesJson = envelope["nodes"];

    // ── Restore ────────────────────────────────────────────────────────────────
    int restored = 0;
    for (auto& sn : nodes) {
        auto* n = dynamic_cast<const INode*>(sn.get());
        if (!n)
            throw std::runtime_error(
                "JsonFileStateStore::restore: IStatefulNode is not an INode");
        if (!nodesJson.contains(n->name())) {
            spdlog::warn("JsonFileStateStore: no saved state for node '{}' — starting cold",
                         n->name());
            continue;
        }
        // const_cast is safe: we never modify the json object inside restoreState
        nlohmann::json& nodeObj =
            const_cast<nlohmann::json&>(nodesJson[n->name()]);
        detail::JsonNodeState state(nodeObj);
        sn->restoreState(state);
        ++restored;
    }

    spdlog::info("JsonFileStateStore: restored {}/{} nodes from '{}'",
                 restored, nodes.size(), file_.string());
    return true;
}

} // namespace dag
