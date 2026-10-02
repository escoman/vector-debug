#pragma once

#include "rdb_types.h"

#include <cstdint>
#include <string>
#include <vector>
#include <unordered_map>
#include <unordered_set>

// ---------------------------------------------------------------------------
// Call Graph Model — Stage 6.12
//
// Read-only snapshot of RDB data for visual Call Graph rendering.
// Built from RDB objects + links via buildCallGraphModel().
//
// No GUI, SDL, OpenGL, Board, or ImGui dependency.
// Fully testable in isolation.
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// Graph node — corresponds to one RDB object or one unresolved target
// ---------------------------------------------------------------------------

struct CallGraphNode
{
    uint16_t     address   = 0;
    std::string  name;
    RdbObjectType type     = RdbObjectType::Unknown;
    std::string  comment;
    uint32_t     size      = 0;
    bool         hasSize   = false;
    bool         unresolved = false;

    // Properties snapshot (for tooltip/details panel)
    std::map<std::string, RdbPropertyValue> properties;
};

// ---------------------------------------------------------------------------
// Graph edge — corresponds to one unique RDB link
// ---------------------------------------------------------------------------

struct GraphEdge
{
    uint16_t source = 0;
    uint16_t target = 0;
};

// ---------------------------------------------------------------------------
// Graph Model — complete snapshot for rendering
// ---------------------------------------------------------------------------

struct CallGraphModel
{
    std::vector<CallGraphNode> nodes;
    std::vector<GraphEdge> edges;

    // address → index in nodes[] for O(1) lookup
    std::unordered_map<uint16_t, size_t> addressToIndex;

    // Statistics
    size_t unresolvedCount = 0;

    void clear()
    {
        nodes.clear();
        edges.clear();
        addressToIndex.clear();
        unresolvedCount = 0;
    }
};

// ---------------------------------------------------------------------------
// Build graph model from RDB objects.
//
// Algorithm: O(N + E) where N = objects, E = unique links.
//
// - Each RDB object becomes a node.
// - Each unique link becomes a directed edge.
// - Links targeting addresses without RDB objects create unresolved nodes.
// - Duplicate links are ignored.
// - Self-links are allowed.
//
// Returns the populated CallGraphModel.
// ---------------------------------------------------------------------------

CallGraphModel buildCallGraphModel(const std::vector<RdbObject> &objects);
