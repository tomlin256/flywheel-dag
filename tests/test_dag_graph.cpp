// =============================================================================
// flywheel-dag — A header-only C++23 reactive DAG computation engine
//
// Copyright (c) 2026 Rob Tomlin
//
// Licensed under the MIT License. See LICENSE file in the project root for
// full license information.
// =============================================================================

#include <gtest/gtest.h>
#include "flywheel/dag_graph.hpp"
#include "flywheel/dag_async.hpp"
#include "flywheel/dag_timeseries.hpp"

#ifdef FLYWHEEL_DAG_HAS_TO_SVG
#include <csignal>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#endif

using namespace dag;

// ── Helpers ───────────────────────────────────────────────────────────────────

static std::shared_ptr<Input<double>> makeInput(const std::string& name, double val = 0.0) {
    return Input<double>::make(name, val);
}

static ComputeNodePtr<double, double, double> makeCompute(
    const std::string& name,
    const NodePtr& a,
    const NodePtr& b)
{
    return ComputeNode<double, double, double>::make(
        name, { a, b },
        [](const double& x, const double& y) { return x + y; });
}

static ComputeNodePtr<double, double> makePassthrough(
    const std::string& name,
    const NodePtr& in)
{
    return ComputeNode<double, double>::make(
        name, { in },
        [](const double& x) { return x; });
}

// ── Test 1: all four nodes appear in the DOT output ──────────────────────────

TEST(DagGraph, AllNodesPresent) {
    auto a = makeInput("a");
    auto b = makeInput("b");
    auto c = makeCompute("c", a, b);
    auto d = makePassthrough("d", c);

    std::string dot = GraphExporter::toDot({ d });

    EXPECT_NE(dot.find("\"a\""), std::string::npos);
    EXPECT_NE(dot.find("\"b\""), std::string::npos);
    EXPECT_NE(dot.find("\"c\""), std::string::npos);
    EXPECT_NE(dot.find("\"d\""), std::string::npos);
}

// ── Test 2: edges appear in the DOT output ───────────────────────────────────

TEST(DagGraph, EdgesPresent) {
    auto a = makeInput("a");
    auto b = makeInput("b");
    auto c = makeCompute("c", a, b);
    auto d = makePassthrough("d", c);

    std::string dot = GraphExporter::toDot({ d });

    EXPECT_NE(dot.find("\"a\" -> \"c\""), std::string::npos);
    EXPECT_NE(dot.find("\"b\" -> \"c\""), std::string::npos);
    EXPECT_NE(dot.find("\"c\" -> \"d\""), std::string::npos);
}

// ── Test 3: Input nodes use ellipse shape ─────────────────────────────────────

TEST(DagGraph, InputNodeShape) {
    auto a = makeInput("a");
    auto b = makeInput("b");
    auto c = makeCompute("c", a, b);

    std::string dot = GraphExporter::toDot({ c });

    // Both 'a' and 'b' should have ellipse shape
    // The DOT attribute appears in the cluster block for the node
    EXPECT_NE(dot.find("shape=ellipse"), std::string::npos);
}

// ── Test 4: Compute nodes use box shape ───────────────────────────────────────

TEST(DagGraph, ComputeNodeShape) {
    auto a = makeInput("a");
    auto c = makePassthrough("c", a);

    std::string dot = GraphExporter::toDot({ c });

    EXPECT_NE(dot.find("shape=box"), std::string::npos);
}

// ── Test 5: Clustering by name prefix ────────────────────────────────────────

TEST(DagGraph, ClusteringByPrefix) {
    // X.a, X.b, X.c → cluster_X
    // Y.d           → cluster_Y
    auto xa = makeInput("X.a");
    auto xb = makeInput("X.b");
    auto xc = makeCompute("X.c", xa, xb);
    auto yd = makePassthrough("Y.d", xc);

    std::string dot = GraphExporter::toDot({ yd });

    EXPECT_NE(dot.find("cluster_X"), std::string::npos);
    EXPECT_NE(dot.find("cluster_Y"), std::string::npos);
}

// ── Test 6: Nodes without a dot go into cluster_global ───────────────────────

TEST(DagGraph, GlobalCluster) {
    auto a = makeInput("fees");
    auto b = makePassthrough("total", a);

    std::string dot = GraphExporter::toDot({ b });

    EXPECT_NE(dot.find("cluster_global"), std::string::npos);
}

// ── Test 7: Multiple roots — all are included ────────────────────────────────

TEST(DagGraph, MultipleRoots) {
    auto a = makeInput("a");
    auto b = makePassthrough("b", a);
    auto c = makePassthrough("c", a);

    std::string dot = GraphExporter::toDot({ b, c });

    EXPECT_NE(dot.find("\"a\""), std::string::npos);
    EXPECT_NE(dot.find("\"b\""), std::string::npos);
    EXPECT_NE(dot.find("\"c\""), std::string::npos);
}

// ── Test 8: Shared upstream node is deduplicated ──────────────────────────────

TEST(DagGraph, SharedUpstreamDeduplicated) {
    auto a = makeInput("shared");
    auto b = makePassthrough("b", a);
    auto c = makePassthrough("c", a);
    auto d = makeCompute("d", b, c);

    std::string dot = GraphExporter::toDot({ d });

    // "shared" should appear exactly once as a node declaration. The edges
    // "shared" -> "b" and "shared" -> "c" also contain the name, so count the
    // [shape= ... ] attribute lines instead — only one for "shared".
    std::size_t attrCount = 0;
    std::size_t pos = 0;
    std::string declNeedle = "\"shared\" [shape=";
    while ((pos = dot.find(declNeedle, pos)) != std::string::npos) {
        ++attrCount;
        pos += declNeedle.size();
    }
    EXPECT_EQ(attrCount, 1u);
}

// ── Test 9: NodeKind::Input returns correct kind ──────────────────────────────

TEST(DagGraph, NodeKindInput) {
    auto a = Input<double>::make("a");
    EXPECT_EQ(a->kind(), NodeKind::Input);
}

// ── Test 10: NodeKind::Compute returns correct kind ───────────────────────────

TEST(DagGraph, NodeKindCompute) {
    auto a = Input<double>::make("a");
    auto b = ComputeNode<double, double>::make("b", { a },
                 [](const double& x) { return x; });
    EXPECT_EQ(b->kind(), NodeKind::Compute);
}

// ── Test 11: NodeKind::AsyncInput returns correct kind ───────────────────────

TEST(DagGraph, NodeKindAsyncInput) {
    auto n = dag::async::AsyncInput<double>::make("ai");
    EXPECT_EQ(n->kind(), NodeKind::AsyncInput);
}

// ── Test 12: NodeKind::AsyncQueue returns correct kind ───────────────────────

TEST(DagGraph, NodeKindAsyncQueue) {
    auto n = dag::async::AsyncQueue<double>::make("aq");
    EXPECT_EQ(n->kind(), NodeKind::AsyncQueue);
}

// ── Test 13: NodeKind::TimeSeries returns correct kind ───────────────────────

TEST(DagGraph, NodeKindTimeSeries) {
    auto inp = Input<double>::make("x");
    auto ewma = dag::ts::EWMANode::make("ewma", inp, 0.1);
    EXPECT_EQ(ewma->kind(), NodeKind::TimeSeries);
}

// ── Test 14: slash in node name is replaced with underscore in DOT output ────

TEST(DagGraph, SlashInNameReplacedWithUnderscore) {
    auto raw    = makeInput("left/right.raw");
    auto smooth = makePassthrough("left/right.smooth", raw);

    std::string dot = GraphExporter::toDot({ smooth });

    // The slash should not appear anywhere in the DOT source
    EXPECT_EQ(dot.find('/'), std::string::npos);

    // The sanitised forms should appear instead
    EXPECT_NE(dot.find("left_right.raw"),    std::string::npos);
    EXPECT_NE(dot.find("left_right.smooth"), std::string::npos);

    // Edge between the two nodes uses sanitised names
    EXPECT_NE(dot.find("\"left_right.raw\" -> \"left_right.smooth\""), std::string::npos);
}

// ── Test 15: hyphen in cluster prefix is quoted, not left as a bare DOT id ───
// A node's cluster is its name up to the first '.', so "my-module/signal.fast"
// lands in "my-module/signal". An unquoted "subgraph cluster_my-module_signal" is
// invalid DOT (dot rejects unquoted ids containing '-'), so the id is quoted.

TEST(DagGraph, HyphenInClusterPrefixIsQuoted) {
    auto fast = makeInput("my-module/signal.fast");
    auto slow = makePassthrough("my-module/signal.slow", fast);

    std::string dot = GraphExporter::toDot({ slow });

    // Cluster identifier must be quoted (valid DOT even with '-' inside).
    EXPECT_NE(dot.find("subgraph \"cluster_my-module_signal\""),
              std::string::npos);

    // The bare, unquoted form must not appear.
    EXPECT_EQ(dot.find("subgraph cluster_my-module"), std::string::npos);
}

// ── Test 16: each NodeKind has its own shape and fill ────────────────────────
// A node's whole attribute line is pinned, so a shape, a fill or the rounded
// style given to the wrong kind shows up here.

TEST(DagGraph, EachNodeKindHasItsShapeAndFill) {
    auto input      = Input<double>::make("input");
    auto asyncInput = dag::async::AsyncInput<double>::make("async_input");
    auto asyncQueue = dag::async::AsyncQueue<double>::make("async_queue");
    auto compute    = makePassthrough("compute", input);
    auto series     = dag::ts::EWMANode::make("series", input, 0.1);

    const std::string dot =
        GraphExporter::toDot({ input, asyncInput, asyncQueue, compute, series });

    const auto declares = [&dot](const std::string& attributes) {
        return dot.find(attributes) != std::string::npos;
    };
    EXPECT_TRUE(declares(R"("input" [shape=ellipse fillcolor="#90EE90" style="filled"];)")) << dot;
    EXPECT_TRUE(declares(R"("async_input" [shape=ellipse fillcolor="#3CB371" style="filled"];)"))
        << dot;
    EXPECT_TRUE(declares(R"("async_queue" [shape=cylinder fillcolor="#2E8B57" style="filled"];)"))
        << dot;
    EXPECT_TRUE(declares(R"("compute" [shape=box fillcolor="#87CEEB" style="filled"];)")) << dot;
    EXPECT_TRUE(declares(R"("series" [shape=box fillcolor="#FFD700" style="filled,rounded"];)"))
        << dot;
}

// ── Test 17: a kind with no entry is drawn as a white box ────────────────────
// The switches in shapeFor() and fillFor() name every NodeKind there is, so
// only a kind() the library never returns reaches the lines after them.

namespace {

class UnknownKindNode : public NodeBase {
public:
    static std::shared_ptr<UnknownKindNode> make() {
        return std::shared_ptr<UnknownKindNode>(new UnknownKindNode);
    }
    ValuePtr eval(EvalContext&) override { return make_value(0.0); }
    std::string name() const override { return "unknown"; }
    std::vector<NodePtr> inputs() const override { return {}; }
    NodeKind kind() const override { return static_cast<NodeKind>(99); }

private:
    UnknownKindNode() = default;
};

}  // namespace

TEST(DagGraph, AKindWithNoEntryIsDrawnAsAWhiteBox) {
    const std::string dot = GraphExporter::toDot({ UnknownKindNode::make() });

    EXPECT_NE(dot.find(R"("unknown" [shape=box fillcolor="#FFFFFF" style="filled"];)"),
              std::string::npos) << dot;
}

// ── toSvg() ───────────────────────────────────────────────────────────────────
// toSvg() starts `dot` without a shell, so the paths it is given are arguments
// and nothing else (flywheel-dag#62). Each test builds a scratch directory of
// its own and puts a stand-in `dot` first on PATH: a /bin/sh script that
// records the arguments it receives, NUL-separated so that a newline in a path
// survives, and the variable FLYWHEEL_DAG_TEST_ENV; writes "<svg/>" to the file
// after -o; and exits with the code in the file `exit` beside it, 0 when there
// is none. A stand-in `pwn` sits beside it and leaves a marker file: a path
// that reached a shell would run it. No Graphviz, no sleep. The stand-ins are
// shell scripts, so these tests exist where toSvg() does, which is POSIX.

#ifdef FLYWHEEL_DAG_HAS_TO_SVG

namespace {

namespace fs = std::filesystem;

// Ignores SIGCHLD for as long as it lives. A process that does has its
// children reaped for it, and waitpid() then fails with ECHILD.
class SigchldIgnored {
public:
    SigchldIgnored() {
        struct sigaction ignore {};
        ignore.sa_handler = SIG_IGN;
        sigemptyset(&ignore.sa_mask);
        sigaction(SIGCHLD, &ignore, &previous_);
    }
    ~SigchldIgnored() { sigaction(SIGCHLD, &previous_, nullptr); }
    SigchldIgnored(const SigchldIgnored&) = delete;
    SigchldIgnored& operator=(const SigchldIgnored&) = delete;

private:
    struct sigaction previous_ {};
};

class DagGraphToSvg : public ::testing::Test {
protected:
    void SetUp() override {
        std::string pattern = (fs::temp_directory_path() / "flywheel_dag_graph_XXXXXX").string();
        ASSERT_NE(mkdtemp(pattern.data()), nullptr);
        root_ = pattern;
        bin_  = root_ / "bin";
        work_ = root_ / "work";
        fs::create_directories(bin_);
        fs::create_directories(work_);
        fs::create_directories(root_ / "empty");

        writeScript(bin_ / "dot",
            "here=${0%/*}\n"
            "printf '%s\\0' \"$FLYWHEEL_DAG_TEST_ENV\" > \"$here/env\"\n"
            ": > \"$here/args\"\n"
            "out=\n"
            "while [ $# -gt 0 ]; do\n"
            "    printf '%s\\0' \"$1\" >> \"$here/args\"\n"
            "    if [ \"$1\" = -o ]; then out=$2; fi\n"
            "    shift\n"
            "done\n"
            "if [ -n \"$out\" ]; then printf '<svg/>' > \"$out\"; fi\n"
            "code=0\n"
            "if [ -r \"$here/exit\" ]; then read code < \"$here/exit\"; fi\n"
            "exit \"$code\"\n");
        writeScript(bin_ / "pwn", ": > \"${0%/*}/pwned\"\n");

        // What TearDown() puts back: PATH, the working directory, and the variable the tests set.
        const char* const path = std::getenv("PATH");
        hadPath_ = path != nullptr;
        if (hadPath_) path_ = path;
        pathSaved_ = true;
        setenv("PATH", (hadPath_ ? bin_.string() + ":" + path_ : bin_.string()).c_str(), 1);

        std::error_code error;
        cwd_ = fs::current_path(error);
        ASSERT_FALSE(error) << error.message();
        fs::current_path(work_, error);  // so that a relative path lands in the scratch directory
        ASSERT_FALSE(error) << error.message();
    }

    void TearDown() override {
        std::error_code error;
        if (!cwd_.empty()) fs::current_path(cwd_, error);
        if (pathSaved_) {
            if (hadPath_) setenv("PATH", path_.c_str(), 1);
            else          unsetenv("PATH");
        }
        unsetenv("FLYWHEEL_DAG_TEST_ENV");
        if (!root_.empty()) fs::remove_all(root_, error);
    }

    static void writeScript(const fs::path& file, const std::string& body) {
        {
            std::ofstream script(file);
            script << "#!/bin/sh\n" << body;
        }
        fs::permissions(file, fs::perms::owner_all);
    }

    static std::string readAll(const fs::path& file) {
        std::ifstream in(file, std::ios::binary);
        std::ostringstream all;
        all << in.rdbuf();
        return all.str();
    }

    /// What the stand-in dot was given, one string per argument.
    std::vector<std::string> recordedArguments() const {
        std::vector<std::string> arguments;
        std::string current;
        for (const char c : readAll(bin_ / "args")) {
            if (c == '\0') {
                arguments.push_back(current);
                current.clear();
            } else {
                current += c;
            }
        }
        return arguments;
    }

    bool dotRan() const { return fs::exists(bin_ / "args"); }
    bool pwnRan() const { return fs::exists(bin_ / "pwned"); }

    /// Forget what an earlier call left, so that one test can render several paths.
    void forgetRuns() const {
        std::error_code error;
        fs::remove(bin_ / "args", error);
        fs::remove(bin_ / "pwned", error);
    }

    /// Where toSvg() writes the DOT text for an output path that ends in ".svg".
    static std::string dotPathOf(const std::string& svgPath) {
        return svgPath.substr(0, svgPath.size() - 4) + ".dot";
    }

    /// toSvg(outPath) renders: it returns true; dot was given exactly -Tsvg,
    /// dotArgument, -o and outArgument; the SVG is there and the .dot file is
    /// not; pwn did not run. outPath ends in ".svg".
    void expectRendered(const std::string& outPath, const std::string& dotArgument,
                        const std::string& outArgument) const {
        forgetRuns();
        EXPECT_TRUE(GraphExporter::toSvg({ node_ }, outPath));
        const std::vector<std::string> expected{ "-Tsvg", dotArgument, "-o", outArgument };
        EXPECT_EQ(recordedArguments(), expected);
        EXPECT_TRUE(fs::exists(outPath)) << outPath;
        EXPECT_FALSE(fs::exists(dotPathOf(outPath))) << dotPathOf(outPath);
        EXPECT_FALSE(pwnRan());
    }

    std::shared_ptr<Input<double>> node_ = makeInput("a");
    fs::path    root_;  // the scratch directory, with bin/ (the stand-ins), work/ (the working directory) and empty/
    fs::path    bin_;
    fs::path    work_;
    fs::path    cwd_;   // the working directory to go back to
    std::string path_;  // PATH to go back to, when there was one
    bool        hadPath_   = false;
    bool        pathSaved_ = false;
};

}  // namespace

// ── Test 18: dot is given the two paths as its arguments ─────────────────────

TEST_F(DagGraphToSvg, RunsDotWithThePathsAsArguments) {
    const std::string out = (work_ / "graph.svg").string();

    expectRendered(out, dotPathOf(out), out);
}

// ── Test 19: whatever a file name holds stays inside one argument ────────────
// Names a shell would split, expand or run. pwn is on PATH, so a name that
// reached a shell as a command would leave its marker.

TEST_F(DagGraphToSvg, PassesAnyNameAsOneArgument) {
    const std::vector<std::string> names = {
        "with space.svg", "semi;pwn;x.svg",  "sub$(pwn)x.svg", "tick`pwn`x.svg",
        "pipe|pwn|x.svg", "amp&pwn&x.svg",   "quote'x.svg",    "dquote\"x.svg",
        "new\npwn\nline.svg", "glob*?[x].svg",
    };
    for (const std::string& name : names) {
        SCOPED_TRACE(name);
        const std::string out = (work_ / name).string();
        expectRendered(out, dotPathOf(out), out);
    }
}

// ── Test 20: so does a directory name ────────────────────────────────────────

TEST_F(DagGraphToSvg, PassesAnyDirectoryNameAsOneArgument) {
    const fs::path dir = work_ / "dir with space;pwn";
    fs::create_directory(dir);
    const std::string out = (dir / "graph.svg").string();

    expectRendered(out, dotPathOf(out), out);
}

// ── Test 21: a path that starts with '-' is not an option ────────────────────
// dot reads `-x.dot` as the option -x, exits 0 and writes nothing, so such a
// path reaches it as ./-x.dot. The test runs in the scratch directory, which is
// where the relative paths land.

TEST_F(DagGraphToSvg, HandsDotAPathThatStartsWithADashAsAPath) {
    expectRendered("-graph.svg", "./-graph.dot", "./-graph.svg");
}

// ── Test 22: dot exits non-zero ──────────────────────────────────────────────

TEST_F(DagGraphToSvg, KeepsTheDotFileWhenDotFails) {
    {
        std::ofstream exitCode(bin_ / "exit");
        exitCode << "3\n";
    }
    const std::string out = (work_ / "graph.svg").string();

    EXPECT_FALSE(GraphExporter::toSvg({ node_ }, out));
    EXPECT_TRUE(dotRan());
    EXPECT_TRUE(fs::exists(dotPathOf(out))) << dotPathOf(out);
}

// ── Test 23: there is no dot on PATH ─────────────────────────────────────────

TEST_F(DagGraphToSvg, KeepsTheDotFileWhenThereIsNoDot) {
    setenv("PATH", (root_ / "empty").c_str(), 1);
    const std::string out = (work_ / "graph.svg").string();

    EXPECT_FALSE(GraphExporter::toSvg({ node_ }, out));
    EXPECT_FALSE(dotRan());
    EXPECT_TRUE(fs::exists(dotPathOf(out))) << dotPathOf(out);
}

// ── Test 24: the exit status cannot be read ──────────────────────────────────
// With SIGCHLD ignored, waitpid() fails with ECHILD. There is no status to
// trust, and the status variable starts at 0, which reads as success, so the
// answer is false and the .dot file stays, although the stand-in wrote the SVG.

TEST_F(DagGraphToSvg, ReturnsFalseWhenTheExitStatusCannotBeRead) {
    const std::string out = (work_ / "graph.svg").string();

    bool rendered = true;
    {
        const SigchldIgnored ignored;
        rendered = GraphExporter::toSvg({ node_ }, out);
    }

    EXPECT_FALSE(rendered);
    EXPECT_TRUE(dotRan());
    EXPECT_TRUE(fs::exists(out)) << out;
    EXPECT_TRUE(fs::exists(dotPathOf(out))) << dotPathOf(out);
}

// ── Test 25: the .dot file cannot be written ─────────────────────────────────
// Nothing is run, and nothing is left behind.

TEST_F(DagGraphToSvg, RunsNothingWhenTheDirectoryIsMissing) {
    const std::string out = (work_ / "no_such_dir" / "graph.svg").string();

    EXPECT_FALSE(GraphExporter::toSvg({ node_ }, out));
    EXPECT_FALSE(dotRan());
    EXPECT_TRUE(fs::is_empty(work_));
}

// ── Test 26: dot gets the caller's environment ───────────────────────────────

TEST_F(DagGraphToSvg, HandsDotTheCallersEnvironment) {
    setenv("FLYWHEEL_DAG_TEST_ENV", "from the caller", 1);
    const std::string out = (work_ / "graph.svg").string();

    EXPECT_TRUE(GraphExporter::toSvg({ node_ }, out));
    const std::string recorded = readAll(bin_ / "env");
    EXPECT_EQ(recorded.substr(0, recorded.find('\0')), "from the caller");
}

#endif  // FLYWHEEL_DAG_HAS_TO_SVG

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
