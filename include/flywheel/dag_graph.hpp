// =============================================================================
// flywheel-dag — A header-only C++17 reactive DAG computation engine
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
// and produces a Graphviz DOT description. toSvg() also shells out to
// `dot -Tsvg` to render it; if that fails, the .dot file is kept.
//
// Never include this file directly from nodes — it is an opt-in utility.

#include "dag.hpp"
#include <algorithm>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <stack>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace dag {

class GraphExporter {
public:
    using ConstNodePtr = std::shared_ptr<const INode>;

    /// Build DOT source for the full subgraph reachable upstream from roots.
    static std::string toDot(const std::vector<ConstNodePtr>& roots,
                             const std::string& graphName = "flywheel");

    /// Write DOT to a .dot file, call `dot -Tsvg`, write SVG to outPath.
    /// Returns true on success, and removes the .dot file. On failure (graphviz
    /// absent or non-zero exit) the .dot file is kept alongside outPath and false
    /// is returned.
    static bool toSvg(const std::vector<ConstNodePtr>& roots,
                      const std::string& outPath,
                      const std::string& graphName = "flywheel");

private:
    // ── DOT attribute helpers ────────────────────────────────────────────────

    static const char* shapeFor(NodeKind k) {
        switch (k) {
            case NodeKind::Input:      return "ellipse";
            case NodeKind::AsyncInput: return "ellipse";
            case NodeKind::AsyncQueue: return "cylinder";
            case NodeKind::Compute:    return "box";
            case NodeKind::TimeSeries: return "box";
        }
        return "box";
    }

    static const char* fillFor(NodeKind k) {
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
    static std::string quoted(const std::string& s) {
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

inline bool GraphExporter::toSvg(const std::vector<ConstNodePtr>& roots,
                                 const std::string& outPath,
                                 const std::string& graphName)
{
    const std::string dot = toDot(roots, graphName);

    // Derive .dot path: replace/append extension
    std::string dotPath;
    const auto dotPos = outPath.rfind('.');
    dotPath = (dotPos != std::string::npos) ? outPath.substr(0, dotPos) + ".dot"
                                            : outPath + ".dot";

    {
        std::ofstream f(dotPath);
        if (!f.is_open()) return false;
        f << dot;
    }

    const std::string cmd = "dot -Tsvg " + dotPath + " -o " + outPath;
    const int rc = std::system(cmd.c_str());  // NOLINT(cert-env33-c)
    if (rc == 0) {
        std::remove(dotPath.c_str());
        return true;
    }
    return false;  // .dot file kept; caller prints fallback message
}

} // namespace dag
