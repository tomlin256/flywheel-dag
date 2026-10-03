// =============================================================================
// flywheel-dag — A header-only C++17 reactive DAG computation engine
//
// Copyright (c) 2026 Rob Tomlin
//
// Licensed under the MIT License. See LICENSE file in the project root for
// full license information.
// =============================================================================

#include <gtest/gtest.h>

#include "flywheel/dag.hpp"
#include "flywheel/dag_state_store.hpp"
#include "flywheel/dag_timeseries.hpp"

#include <filesystem>
#include <fstream>
#include <memory>
#include <system_error>

using namespace dag;
using namespace dag::ts;

// ─────────────────────────────────────────────────────────────────────────────
// Helper: temp directory scoped to a test
// ─────────────────────────────────────────────────────────────────────────────
class TempDir {
public:
    TempDir() {
        dir_ = std::filesystem::temp_directory_path()
             / ("flywheel_test_" + std::to_string(std::rand()));
        std::filesystem::create_directories(dir_);
    }
    ~TempDir() { std::filesystem::remove_all(dir_); }
    std::filesystem::path path() const { return dir_; }
    std::filesystem::path file(const std::string& name) const { return dir_ / name; }
private:
    std::filesystem::path dir_;
};

// ─────────────────────────────────────────────────────────────────────────────
// Helper: switch the working directory for one test, and switch back.
// Declare it after the TempDir it enters, so it leaves before that is removed.
// ─────────────────────────────────────────────────────────────────────────────
class WorkingDirectory {
public:
    explicit WorkingDirectory(const std::filesystem::path& dir)
        : previous_(std::filesystem::current_path()) {
        std::filesystem::current_path(dir);
    }
    ~WorkingDirectory() {
        std::error_code ec;   // a destructor must not throw
        std::filesystem::current_path(previous_, ec);
    }
    WorkingDirectory(const WorkingDirectory&)            = delete;
    WorkingDirectory& operator=(const WorkingDirectory&) = delete;
private:
    std::filesystem::path previous_;
};

// ─────────────────────────────────────────────────────────────────────────────
// Helper: build a warmed EWMANode
// ─────────────────────────────────────────────────────────────────────────────
static std::pair<std::shared_ptr<Input<double>>, std::shared_ptr<EWMANode>>
makeWarmEwma(const std::string& name, double alpha, const std::vector<double>& values)
{
    auto inp  = Input<double>::make(name + "_inp", 0.0);
    auto ewma = EWMANode::make(name, inp, alpha);
    EvalContext ctx;
    for (double v : values) { inp->set(v); ewma->eval(ctx); }
    return {inp, ewma};
}

// ─────────────────────────────────────────────────────────────────────────────
// InMemoryStateStore — comprehensive tests
// ─────────────────────────────────────────────────────────────────────────────

TEST(InMemoryStoreTests, SaveRestoreRoundTrip) {
    auto [inp, ewma] = makeWarmEwma("ewma", 0.2, {10, 20, 30, 25});

    InMemoryStateStore store;
    store.save({ewma});
    EXPECT_TRUE(store.hasSavedState());

    auto inp2  = Input<double>::make("ewma_inp2", 0.0);
    auto ewma2 = EWMANode::make("ewma", inp2, 0.2);
    EXPECT_TRUE(store.restore({ewma2}));

    EvalContext ctx;
    inp->set(15.0);  ewma->eval(ctx);
    inp2->set(15.0); ewma2->eval(ctx);
    EXPECT_NEAR(get_value<double>(ewma->eval(ctx)),
                get_value<double>(ewma2->eval(ctx)), 1e-9);
}

TEST(InMemoryStoreTests, RestoreBeforeSaveReturnsFalse) {
    InMemoryStateStore store;
    auto inp  = Input<double>::make("x", 0.0);
    auto ewma = EWMANode::make("ewma", inp, 0.5);
    EXPECT_FALSE(store.restore({ewma}));
}

TEST(InMemoryStoreTests, UnknownNodeSkipped) {
    // Snapshot contains "x.unknown" not in node list — logged, no throw
    InMemoryStateStore store;
    auto inp1  = Input<double>::make("a_inp", 0.0);
    auto ewma1 = EWMANode::make("ewma.a", inp1, 0.5);
    EvalContext ctx;
    inp1->set(10.0); ewma1->eval(ctx);
    store.save({ewma1});

    // Restore with a different node — ewma1's saved state is ignored
    auto inp2  = Input<double>::make("b_inp", 0.0);
    auto ewma2 = EWMANode::make("ewma.b", inp2, 0.5);
    EXPECT_NO_THROW(store.restore({ewma2}));
    // ewma2 starts cold (no matching key)
    EXPECT_FALSE(ewma2->isInitialized());
}

TEST(InMemoryStoreTests, MissingNodeWarnedNodeCold) {
    // Node list has entry not in snapshot — logged at warn, node stays cold
    InMemoryStateStore store;
    store.save({});  // empty save

    auto inp  = Input<double>::make("x", 0.0);
    auto ewma = EWMANode::make("ewma", inp, 0.5);
    EXPECT_TRUE(store.restore({ewma}));  // restore returns true (state exists, even if empty)
    EXPECT_FALSE(ewma->isInitialized());
}

// ─────────────────────────────────────────────────────────────────────────────
// JsonFileStateStore
// ─────────────────────────────────────────────────────────────────────────────

TEST(JsonStoreTests, SaveRestoreRoundTrip) {
    TempDir tmp;
    auto snapFile = tmp.file("snap.json");

    auto [inp, ewma] = makeWarmEwma("ewma", 0.3, {5, 10, 8, 12, 9});

    {
        JsonFileStateStore store(snapFile);
        store.save({ewma});
        EXPECT_TRUE(std::filesystem::exists(snapFile));
    }

    auto inp2  = Input<double>::make("ewma_inp2", 0.0);
    auto ewma2 = EWMANode::make("ewma", inp2, 0.3);
    {
        JsonFileStateStore store(snapFile);
        EXPECT_TRUE(store.restore({ewma2}));
    }

    EvalContext ctx;
    inp->set(7.0);  ewma->eval(ctx);
    inp2->set(7.0); ewma2->eval(ctx);
    EXPECT_NEAR(get_value<double>(ewma->eval(ctx)),
                get_value<double>(ewma2->eval(ctx)), 1e-9);
}

TEST(JsonStoreTests, NoFileReturnsFalse) {
    TempDir tmp;
    JsonFileStateStore store(tmp.file("nonexistent.json"));
    auto inp  = Input<double>::make("x", 0.0);
    auto ewma = EWMANode::make("ewma", inp, 0.5);
    EXPECT_FALSE(store.restore({ewma}));
}

TEST(JsonStoreTests, AtomicWrite) {
    // save() writes <file>.tmp and renames it into place, so a snapshot is never
    // half-written.
    TempDir tmp;
    auto snapFile = tmp.file("snap.json");

    auto [inp, ewma] = makeWarmEwma("ewma", 0.4, {1, 2, 3});
    JsonFileStateStore store(snapFile);
    store.save({ewma});

    std::ifstream f(snapFile);
    ASSERT_TRUE(f.is_open());
    std::string content((std::istreambuf_iterator<char>(f)),
                         std::istreambuf_iterator<char>());
    EXPECT_FALSE(content.empty());
    // The .tmp was renamed away.
    auto tmpFile = snapFile; tmpFile += ".tmp";
    EXPECT_FALSE(std::filesystem::exists(tmpFile));
}

TEST(JsonStoreTests, VersionMismatchThrows) {
    TempDir tmp;
    auto snapFile = tmp.file("snap.json");
    // Write a file with an unsupported version
    {
        std::ofstream out(snapFile);
        out << R"({"version": 99, "saved_at": "2026-01-01T00:00:00Z", "nodes": {}})";
    }
    JsonFileStateStore store(snapFile);
    auto inp  = Input<double>::make("x", 0.0);
    auto ewma = EWMANode::make("ewma", inp, 0.5);
    EXPECT_THROW(store.restore({ewma}), std::runtime_error);
}

TEST(JsonStoreTests, MalformedJsonThrows) {
    TempDir tmp;
    auto snapFile = tmp.file("snap.json");
    {
        std::ofstream out(snapFile);
        out << "this is not json {{{";
    }
    JsonFileStateStore store(snapFile);
    auto inp  = Input<double>::make("x", 0.0);
    auto ewma = EWMANode::make("ewma", inp, 0.5);
    EXPECT_THROW(store.restore({ewma}), std::runtime_error);
}

TEST(JsonStoreTests, UnknownNodeInFileSkipped) {
    // Snapshot contains node "x.unknown" not in node list — no throw
    TempDir tmp;
    auto snapFile = tmp.file("snap.json");
    {
        std::ofstream out(snapFile);
        out << R"({
          "version": 1,
          "saved_at": "2026-01-01T00:00:00Z",
          "nodes": {
            "x.unknown": {"ewma": 42.0, "initialized": true}
          }
        })";
    }
    JsonFileStateStore store(snapFile);
    auto inp  = Input<double>::make("x", 0.0);
    auto ewma = EWMANode::make("ewma", inp, 0.5);
    EXPECT_NO_THROW(store.restore({ewma}));
}

TEST(JsonStoreTests, MissingNodeInFileWarnedNodeCold) {
    // Node in DAG list has no entry in snapshot — node starts cold, no throw
    TempDir tmp;
    auto snapFile = tmp.file("snap.json");
    {
        // Save an empty nodes map
        std::ofstream out(snapFile);
        out << R"({"version": 1, "saved_at": "2026-01-01T00:00:00Z", "nodes": {}})";
    }
    JsonFileStateStore store(snapFile);
    auto inp  = Input<double>::make("x", 0.0);
    auto ewma = EWMANode::make("ewma_missing", inp, 0.5);
    EXPECT_TRUE(store.restore({ewma}));
    EXPECT_FALSE(ewma->isInitialized());
}

TEST(JsonStoreTests, CreatesParentDirectory) {
    TempDir tmp;
    // Point to a deeply nested path that doesn't exist yet
    auto snapFile = tmp.path() / "a" / "b" / "c" / "snap.json";
    EXPECT_FALSE(std::filesystem::exists(snapFile.parent_path()));

    auto [inp, ewma] = makeWarmEwma("ewma", 0.5, {1.0, 2.0});
    JsonFileStateStore store(snapFile);
    EXPECT_NO_THROW(store.save({ewma}));
    EXPECT_TRUE(std::filesystem::exists(snapFile));
}

TEST(JsonStoreTests, BareFilenameSaveRestoreRoundTrip) {
    // A bare filename has no parent directory to create, and
    // create_directories("") throws. The snapshot goes to the working directory.
    TempDir tmp;
    WorkingDirectory cwd(tmp.path());

    auto [inp, ewma] = makeWarmEwma("ewma", 0.5, {10.0, 20.0});
    JsonFileStateStore store("snap.json");
    ASSERT_NO_THROW(store.save({ewma}));
    EXPECT_TRUE(std::filesystem::exists(tmp.file("snap.json")));

    auto inp2  = Input<double>::make("ewma_inp2", 0.0);
    auto ewma2 = EWMANode::make("ewma", inp2, 0.5);
    EXPECT_TRUE(store.restore({ewma2}));

    EvalContext ctx;
    inp->set(30.0);  ewma->eval(ctx);
    inp2->set(30.0); ewma2->eval(ctx);
    EXPECT_NEAR(get_value<double>(ewma->eval(ctx)),
                get_value<double>(ewma2->eval(ctx)), 1e-9);
}

TEST(JsonStoreTests, RollingStatsSaveRestoreRoundTrip) {
    // Verify a more complex node survives a JSON file round-trip
    TempDir tmp;
    auto snapFile = tmp.file("snap.json");

    auto inp   = Input<double>::make("x", 0.0);
    auto stats = RollingStats::make("stats", inp, 5);
    EvalContext ctx;
    for (double v : {10.0, 11.0, 12.0, 13.0, 14.0}) {
        inp->set(v); stats->eval(ctx);
    }

    JsonFileStateStore store1(snapFile);
    store1.save({stats});

    auto inp2   = Input<double>::make("x2", 0.0);
    auto stats2 = RollingStats::make("stats", inp2, 5);
    JsonFileStateStore store2(snapFile);
    EXPECT_TRUE(store2.restore({stats2}));

    inp->set(15.0);  stats->eval(ctx);
    inp2->set(15.0); stats2->eval(ctx);
    EXPECT_NEAR(stats->mean(),   stats2->mean(),   1e-9);
    EXPECT_NEAR(stats->stddev(), stats2->stddev(), 1e-9);
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
