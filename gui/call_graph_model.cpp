#include "call_graph_model.h"

#include <utility>

// ---------------------------------------------------------------------------
// Build Call Graph Model from RDB objects — Stage 6.12
//
// Algorithm: O(N + E)
//   Pass 1: create nodes from all RDB objects, build address→index map.
//   Pass 2: iterate links, deduplicate, create edges + unresolved nodes.
// ---------------------------------------------------------------------------

CallGraphModel buildCallGraphModel(const std::vector<RdbObject> &objects)
{
    CallGraphModel model;

    // --- Pass 1: objects → nodes, build lookup ----------------------------

    model.nodes.reserve(objects.size());
    model.addressToIndex.reserve(objects.size());

    for (const auto &obj : objects)
    {
        // Skip duplicate addresses (RDB should not have them, but be safe)
        if (model.addressToIndex.count(obj.address))
            continue;

        CallGraphNode node;
        node.address    = obj.address;
        node.name       = obj.name;
        node.type       = obj.type;
        node.comment    = obj.comment;
        node.size       = obj.size;
        node.hasSize    = obj.hasSize;
        node.unresolved = false;
        node.properties = obj.properties;

        model.addressToIndex[obj.address] = model.nodes.size();
        model.nodes.push_back(std::move(node));
    }

    // --- Pass 2: links → edges (deduplicated) -----------------------------

    // Track unique edges: (source, target) → already added
    // Use a set of packed uint32_t keys for O(1) dedup.
    std::unordered_set<uint32_t> seenEdges;
    seenEdges.reserve(objects.size());  // rough estimate

    for (const auto &obj : objects)
    {
        // Source must exist as a node (it was created from an RDB object)
        auto srcIt = model.addressToIndex.find(obj.address);
        if (srcIt == model.addressToIndex.end())
            continue;

        for (uint16_t targetAddr : obj.links)
        {
            // Deduplicate: pack source+target into 32-bit key
            uint32_t edgeKey = (static_cast<uint32_t>(obj.address) << 16)
                             | static_cast<uint32_t>(targetAddr);

            if (!seenEdges.insert(edgeKey).second)
                continue;  // duplicate link — skip

            // If target has no node yet, create unresolved node
            if (model.addressToIndex.find(targetAddr) == model.addressToIndex.end())
            {
                CallGraphNode unresolvedNode;
                unresolvedNode.address    = targetAddr;
                unresolvedNode.unresolved = true;

                model.addressToIndex[targetAddr] = model.nodes.size();
                model.nodes.push_back(std::move(unresolvedNode));
                model.unresolvedCount++;
            }

            GraphEdge edge;
            edge.source = obj.address;
            edge.target = targetAddr;
            model.edges.push_back(edge);
        }
    }

    return model;
}
