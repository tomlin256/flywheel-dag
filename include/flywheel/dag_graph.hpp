// =============================================================================
// flywheel-dag — A header-only C++23 reactive DAG computation engine
//
// Copyright (c) 2026 Rob Tomlin
//
// Licensed under the MIT License. See LICENSE file in the project root for
// full license information.
// =============================================================================

#pragma once
// dag_graph.hpp — DOT/SVG export for the reactive DAG
//
// GraphExporter traverses the DAG backward from a set of root (output) nodes
// and produces a Graphviz DOT description. toSvg() also runs `dot -Tsvg` to
// render it; if that fails, the .dot file is kept.
//
// toSvg() starts `dot` itself with posix_spawnp and hands it the two paths as
// arguments, so no shell reads them and a path is only ever a path
// (flywheel-dag#62). That ties it to POSIX: where <spawn.h> does not exist
// (Windows) the class has toDot() and no toSvg(), and the header still
// compiles. FLYWHEEL_DAG_HAS_TO_SVG is defined where toSvg() exists. No other
// header of the engine needs POSIX.
//
// Never include this file directly from nodes — it is an opt-in utility.

#include "dag.hpp"
#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <stack>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#if __has_include(<spawn.h>) && __has_include(<sys/wait.h>)
#define FLYWHEEL_DAG_HAS_TO_SVG 1
#include <cerrno>
#include <spawn.h>
#include <sys/wait.h>
#if defined(__APPLE__)
#include <crt_externs.h>  // a shared library cannot name environ on macOS: _NSGetEnviron() finds it
#else
extern "C" char** environ;
#endif
#endif

namespace dag {

class GraphExporter {
public:
    using ConstNodePtr = std::shared_ptr<const INode>;

    /// Build DOT source for the full subgraph reachable upstream from roots.
    static std::string toDot(const std::vector<ConstNodePtr>& roots,
                             const std::string& graphName = "flywheel");

#ifdef FLYWHEEL_DAG_HAS_TO_SVG
    /// Write DOT to a .dot file, run `dot -Tsvg` on it without a shell, write SVG
    /// to outPath. The .dot file sits beside outPath and is named from the file
    /// name alone: its extension becomes ".dot" (".dot" is appended when it has
    /// none), so graph.svg gives graph.dot; an outPath that already ends in .dot,
    /// in any case, gets ".dot" appended instead, so that dot never reads the file
    /// it writes. Returns true on success, and removes the .dot file. On failure
    /// (no `dot` on PATH or non-zero exit) the .dot file is kept and false is
    /// returned. outPath may hold any character, a space, ';' or '$' included, and
    /// may start with '-'; one with a NUL is refused (false, nothing written).
    static bool toSvg(const std::vector<ConstNodePtr>& roots,
                      const std::string& outPath,
                      const std::string& graphName = "flywheel");
#endif

private:
#ifdef FLYWHEEL_DAG_HAS_TO_SVG
    // ── Running dot ──────────────────────────────────────────────────────────

    /// `path` as one argument for `dot` that it cannot take for an option: a
    /// leading '-' becomes "./-", which names the same file.
    static std::string asArgument(const std::string& path) {
        return (!path.empty() && path.front() == '-') ? "./" + path : path;
    }

    static char asciiLower(const char c) { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c; }

    /// Where toSvg() writes the DOT text (flywheel-dag#63): beside outPath, named
    /// from the file name alone, so that a dot in a directory name is never taken
    /// for the extension. The extension of the file name becomes ".dot", or ".dot"
    /// is appended when there is none. When the extension already is .dot, in any
    /// case (a Mac's file system ignores it), that would be outPath itself, which
    /// dot would read as it wrote it, so ".dot" goes on the end of the whole path
    /// instead. The two names share their stem byte for byte, so a .dot extension
    /// is the only way they can meet.
    static std::string dotPathFor(const std::string& outPath) {
        std::string extension = std::filesystem::path{outPath}.extension().string();
        std::ranges::transform(extension, extension.begin(), asciiLower);
        if (extension == ".dot") return outPath + ".dot";
        return std::filesystem::path{outPath}.replace_extension(".dot").string();
    }

    /// Run `dot -Tsvg dotPath -o outPath` and wait for it. True when it started
    /// and exited 0. posix_spawnp, unlike fork and exec, is safe from a process
    /// with other threads, which an engine with feed threads is.
    static bool runDot(const std::string& dotPath, const std::string& outPath);
#endif

    // ── DOT attribute helpers ────────────────────────────────────────────────

    static std::string_view shapeFor(NodeKind k) {
        switch (k) {
            case NodeKind::Input:
            case NodeKind::AsyncInput: return "ellipse";
            case NodeKind::AsyncQueue: return "cylinder";
            case NodeKind::Compute:
            case NodeKind::TimeSeries: return "box";
        }
        return "box";
    }

    static std::string_view fillFor(NodeKind k) {
        switch (k) {
            case NodeKind::Input:      return "#90EE90";
            case NodeKind::AsyncInput: return "#3CB371";
            case NodeKind::AsyncQueue: return "#2E8B57";
            case NodeKind::Compute:    return "#87CEEB";
            case NodeKind::TimeSeries: return "#FFD700";
        }
        return "#FFFFFF";
    }

    static bool isRounded(NodeKind k) { return k == NodeKind::TimeSeries; }

    // ── Cluster helpers ──────────────────────────────────────────────────────

    /// Returns the portion of `name` before the first '.', or "global"
    /// if there is no '.'.
    static std::string clusterOf(const std::string& name) {
        const auto pos = name.find('.');
        return (pos == std::string::npos) ? "global" : name.substr(0, pos);
    }

    // ── Quote helper ─────────────────────────────────────────────────────────

    /// Wrap `s` in DOT double quotes, escaping '"' and replacing '/' with '_'.
    static std::string quoted(const std::string_view s) {
        std::string out = "\"";
        for (char c : s) {
            if (c == '"') out += "\\\"";
            else if (c == '/') out += '_';
            else          out += c;
        }
        out += '"';
        return out;
    }
};

// ─────────────────────────────────────────────────────────────────────────────
// Implementation
// ─────────────────────────────────────────────────────────────────────────────

inline std::string GraphExporter::toDot(const std::vector<ConstNodePtr>& roots,
                                        const std::string& graphName)
{
    // ── 1. DFS backward from every root ─────────────────────────────────────
    std::unordered_map<std::string, ConstNodePtr>          nodes;  // name → node
    std::vector<std::pair<std::string, std::string>>       edges;  // {parent, child}
    std::unordered_set<std::string>                        visited;

    std::stack<ConstNodePtr> stack;
    for (const auto& r : roots) if (r) stack.push(r);

    while (!stack.empty()) {
        ConstNodePtr cur = stack.top(); stack.pop();
        const std::string cname = cur->name();
        if (visited.count(cname)) continue;
        visited.insert(cname);
        nodes[cname] = cur;

        for (const NodePtr& inp : cur->inputs()) {
            if (!inp) continue;
            edges.push_back({ inp->name(), cname });
            if (!visited.count(inp->name()))
                stack.push(ConstNodePtr{inp});
        }
    }

    // ── 2. Group nodes by cluster ────────────────────────────────────────────
    std::unordered_map<std::string, std::vector<std::string>> clusters;
    for (const auto& [name, _] : nodes)
        clusters[clusterOf(name)].push_back(name);

    // ── 3. Build DOT source ──────────────────────────────────────────────────
    std::ostringstream out;
    out << "digraph " << quoted(graphName) << " {\n"
        << "    rankdir=LR;\n"
        << "    node [fontname=\"Helvetica\" fontsize=10];\n"
        << "    edge [arrowhead=normal];\n"
        << "    compound=true;\n\n";

    // Clusters (sorted for deterministic output)
    std::vector<std::string> clusterNames;
    clusterNames.reserve(clusters.size());
    for (const auto& [cn, _] : clusters) clusterNames.push_back(cn);
    std::sort(clusterNames.begin(), clusterNames.end());

    for (const auto& cn : clusterNames) {
        auto& members = clusters[cn];
        std::sort(members.begin(), members.end());

        out << "    subgraph " << quoted("cluster_" + cn) << " {\n"
            << "        label=" << quoted(cn) << ";\n"
            << "        style=filled;\n"
            << "        fillcolor=\"#f4f8ff\";\n";

        for (const auto& mname : members) {
            const ConstNodePtr& n = nodes.at(mname);
            NodeKind k = n->kind();
            out << "        " << quoted(mname)
                << " [shape=" << shapeFor(k)
                << " fillcolor=" << quoted(fillFor(k))
                << " style=\"filled";
            if (isRounded(k)) out << ",rounded";
            out << "\"];\n";
        }
        out << "    }\n\n";
    }

    // Edges (deduplicated and sorted for deterministic output)
    std::vector<std::pair<std::string, std::string>> sortedEdges = edges;
    std::sort(sortedEdges.begin(), sortedEdges.end());
    sortedEdges.erase(std::unique(sortedEdges.begin(), sortedEdges.end()),
                      sortedEdges.end());

    for (const auto& [from, to] : sortedEdges)
        out << "    " << quoted(from) << " -> " << quoted(to) << ";\n";

    out << "}\n";
    return out.str();
}

#ifdef FLYWHEEL_DAG_HAS_TO_SVG
inline bool GraphExporter::runDot(const std::string& dotPath, const std::string& outPath)
{
    // One string per argument, kept in a vector that outlives the call: posix_spawnp takes char*, not const char*.
    std::vector<std::string> args{ "dot", "-Tsvg", asArgument(dotPath), "-o", asArgument(outPath) };
    std::vector<char*> argv;
    argv.reserve(args.size() + 1);
    for (std::string& arg : args) argv.push_back(arg.data());
    argv.push_back(nullptr);

#if defined(__APPLE__)
    char* const* const env = *_NSGetEnviron();
#else
    char* const* const env = environ;
#endif

    // The caller's PATH finds dot, and the child gets the caller's environment and standard streams, as it did under
    // std::system. A non-zero return is the spawn failing, no dot on PATH included; a libc that reports a failed exec
    // as exit status 127 instead is caught by the status check below.
    pid_t pid = 0;
    if (posix_spawnp(&pid, "dot", nullptr, nullptr, argv.data(), env) != 0) return false;

    int status = 0;
    pid_t reaped = 0;
    do {
        reaped = waitpid(pid, &status, 0);
    } while (reaped == -1 && errno == EINTR);
    // Any other waitpid failure leaves the status unknown, and status starts at 0, which reads as success, so the
    // child's pid is checked first. A process that ignores SIGCHLD gets ECHILD here, as it did from std::system.
    return reaped == pid && WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

inline bool GraphExporter::toSvg(const std::vector<ConstNodePtr>& roots,
                                 const std::string& outPath,
                                 const std::string& graphName)
{
    // A NUL ends a path where the OS reads it, so the .dot name and outPath could then be one file.
    if (outPath.find('\0') != std::string::npos) return false;

    const std::string dot = toDot(roots, graphName);
    const std::string dotPath = dotPathFor(outPath);

    {
        std::ofstream f(dotPath);
        if (!f.is_open()) return false;
        f << dot;
    }

    if (runDot(dotPath, outPath)) {
        std::remove(dotPath.c_str());
        return true;
    }
    return false;  // .dot file kept; caller prints fallback message
}
#endif

} // namespace dag
