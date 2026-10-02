#pragma once

#include "call_graph_model.h"

#include <cstdint>
#include <functional>
#include <string>

// Forward declarations
class IDebugBackend;
namespace ax { namespace NodeEditor { struct EditorContext; } }

// ---------------------------------------------------------------------------
// Call Graph Window — Stage 6.12 + Persistence
//
// Visual node graph built from RDB objects + links.
// Uses imgui-node-editor for rendering and persistence.
//
// Graph is built only on explicit "Build" button press.
// RDB is the sole data source.
// Visual state (positions, zoom, pan) saved to <rom>.rdb.graph
// by imgui-node-editor built-in persistence mechanism.
// ---------------------------------------------------------------------------

class CallGraphWindow
{
public:
    CallGraphWindow();
    ~CallGraphWindow();

    void render(IDebugBackend &backend);

    void setVisible(bool v) { visible_ = v; }
    bool isVisible() const { return visible_; }
    bool &getVisibleRef() { return visible_; }

    // Mark graph as outdated (called when RDB changes externally).
    void markOutdated() { outdated_ = true; }

    // Notify that a new ROM was loaded — recreate editor with new .rdb.graph path.
    void onRomLoaded(IDebugBackend &backend);

    // Navigation callbacks (same pattern as other windows)
    std::function<void(uint16_t address)> onGoToDisassembly;
    std::function<void(uint16_t address)> onGoToMemoryInspector;

private:
    bool visible_ = true;

    // Graph state
    enum class State { NotBuilt, Built, Outdated, Building };
    State state_ = State::NotBuilt;
    bool  outdated_ = false;

    // Current graph model (read-only snapshot after Build)
    CallGraphModel model_;

    // imgui-node-editor context
    ax::NodeEditor::EditorContext *editorContext_ = nullptr;

    // Path to .rdb.graph file for current ROM (empty if no ROM loaded)
    std::string graphFilePath_;

    // Track addresses from last build to detect new nodes for layout
    std::vector<uint16_t> lastBuiltAddresses_;

    // Currently selected node for context menu
    uint16_t selectedNodeAddress_ = 0;

    // Deferred navigation actions (processed inside ed::Begin/End)
    bool navigateToShowAll_    = false;
    bool navigateToSelection_  = false;

    // Cached zoom level (updated during renderNodeEditor)
    float currentZoom_ = 1.0f;

    // Navigation callbacks (set by GUI for right-click menu)
    uint16_t pendingGoToDisassembly_ = 0;
    uint16_t pendingGoToMemory_      = 0;
    bool     hasPendingDisassembly_  = false;
    bool     hasPendingMemory_       = false;

    // Create/recreate the editor context with current graphFilePath_
    void recreateEditor();

    // Build the graph model from RDB
    void build(IDebugBackend &backend);

    // Render the node editor with all nodes and links
    void renderNodeEditor(IDebugBackend &backend);

    // Render a single node's content
    void renderNodeContent(const CallGraphNode &node);

    // Render context menu for a node
    void renderNodeContextMenu(const CallGraphNode &node);
};

// ---------------------------------------------------------------------------
// Utility: derive .rdb.graph path from .rdb path
// ---------------------------------------------------------------------------

inline std::string graphPathFromRdbPath(const std::string &rdbPath)
{
    if (rdbPath.empty())
        return "";
    return rdbPath + ".graph";
}
