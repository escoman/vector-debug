#include "call_graph_window.h"
#include "idebug_backend.h"
#include "rdb_controller.h"

// imgui-node-editor
#include "imgui_node_editor.h"

// Dear ImGui
#include "imgui.h"

#include <cstdio>
#include <algorithm>
#include <set>

namespace ed = ax::NodeEditor;

// ---------------------------------------------------------------------------
// Layout constants
// ---------------------------------------------------------------------------

static constexpr float NODE_WIDTH       = 200.0f;
static constexpr float NODE_H_SPACING   = 60.0f;
static constexpr float NODE_V_SPACING   = 80.0f;
static constexpr int   NODES_PER_ROW    = 5;

// ---------------------------------------------------------------------------
// Construction / Destruction
// ---------------------------------------------------------------------------

CallGraphWindow::CallGraphWindow()
{
    // Editor is created lazily in recreateEditor() when a ROM is loaded
    // and we know the .rdb.graph file path.
}

CallGraphWindow::~CallGraphWindow()
{
    if (editorContext_)
        ed::DestroyEditor(editorContext_);
}

// ---------------------------------------------------------------------------
// Recreate editor context with current graphFilePath_
// ---------------------------------------------------------------------------

void CallGraphWindow::recreateEditor()
{
    if (editorContext_)
    {
        ed::DestroyEditor(editorContext_);
        editorContext_ = nullptr;
    }

    if (graphFilePath_.empty())
        return;

    ed::Config config;
    config.SettingsFile = graphFilePath_.c_str();
    editorContext_ = ed::CreateEditor(&config);
}

// ---------------------------------------------------------------------------
// onRomLoaded — derive .rdb.graph path, recreate editor
// ---------------------------------------------------------------------------

void CallGraphWindow::onRomLoaded(IDebugBackend &backend)
{
    const RdbController &rdb = backend.rdbController();
    std::string rdbPath = rdb.getPath();

    graphFilePath_ = graphPathFromRdbPath(rdbPath);

    // Recreate editor with new persistence file
    recreateEditor();

    // Reset graph state — old graph is irrelevant for new ROM
    model_.clear();
    lastBuiltAddresses_.clear();
    state_ = State::NotBuilt;
    outdated_ = false;
}

// ---------------------------------------------------------------------------
// Build — populate Graph Model from RDB snapshot
// ---------------------------------------------------------------------------

void CallGraphWindow::build(IDebugBackend &backend)
{
    state_ = State::Building;

    // Ensure editor exists (might not if no ROM was loaded via onRomLoaded)
    if (!editorContext_ && !graphFilePath_.empty())
        recreateEditor();

    // Get RDB snapshot (read-only, thread-safe via existing mechanism)
    const RdbController &rdb = backend.rdbController();
    std::vector<RdbObject> objects = rdb.listObjects();

    // Remember previous addresses to detect new nodes
    std::set<uint16_t> previousAddresses(lastBuiltAddresses_.begin(),
                                          lastBuiltAddresses_.end());

    // Build graph model — O(N + E)
    model_ = buildCallGraphModel(objects);

    // Track current addresses for next build
    lastBuiltAddresses_.clear();
    lastBuiltAddresses_.reserve(model_.nodes.size());
    for (const auto &node : model_.nodes)
        lastBuiltAddresses_.push_back(node.address);

    outdated_ = false;
    state_ = State::Built;

    // Apply grid layout only for NEW nodes (not in previous build).
    // Existing nodes keep their saved positions from .rdb.graph.
    // NodeId = address (stable identity).
    if (editorContext_)
    {
        ed::SetCurrentEditor(editorContext_);

        // Collect new addresses (sorted for deterministic layout)
        std::vector<uint16_t> newAddresses;
        for (const auto &node : model_.nodes)
        {
            if (previousAddresses.find(node.address) == previousAddresses.end())
                newAddresses.push_back(node.address);
        }
        std::sort(newAddresses.begin(), newAddresses.end());

        // Place new nodes in grid below existing ones
        float cellW = NODE_WIDTH + NODE_H_SPACING;
        float cellH = 120.0f + NODE_V_SPACING;

        // Find the Y offset: below all existing nodes
        float maxY = 50.0f;
        if (!previousAddresses.empty())
        {
            for (uint16_t addr : previousAddresses)
            {
                ImVec2 pos = ed::GetNodePosition(ed::NodeId(static_cast<uintptr_t>(addr)));
                if (pos.y + cellH > maxY)
                    maxY = pos.y + cellH;
            }
        }

        for (size_t i = 0; i < newAddresses.size(); i++)
        {
            int row = static_cast<int>(i) / NODES_PER_ROW;
            int col = static_cast<int>(i) % NODES_PER_ROW;

            float x = 50.0f + col * cellW;
            float y = maxY + row * cellH;

            ed::SetNodePosition(ed::NodeId(static_cast<uintptr_t>(newAddresses[i])),
                                ImVec2(x, y));
        }

        ed::SetCurrentEditor(nullptr);
    }
}

// ---------------------------------------------------------------------------
// Render node content
// ---------------------------------------------------------------------------

void CallGraphWindow::renderNodeContent(const CallGraphNode &node)
{
    if (node.unresolved)
    {
        ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.5f, 1.0f), "<unresolved>");
        ImGui::Text("%04X", node.address);
    }
    else
    {
        // Name
        if (!node.name.empty())
            ImGui::TextUnformatted(node.name.c_str());
        else
            ImGui::Text("%04X", node.address);

        // Address
        ImGui::TextColored(ImVec4(0.6f, 0.6f, 0.6f, 1.0f), "0x%04X", node.address);

        // Type
        ImGui::TextColored(ImVec4(0.5f, 0.8f, 0.5f, 1.0f), "%s",
                           rdbObjectTypeToString(node.type));
    }
}

// ---------------------------------------------------------------------------
// Render context menu for a node
// ---------------------------------------------------------------------------

void CallGraphWindow::renderNodeContextMenu(const CallGraphNode &node)
{
    char label[128];
    snprintf(label, sizeof(label), "Go to Disassembly (%04X)", node.address);
    if (ImGui::MenuItem(label))
    {
        pendingGoToDisassembly_ = node.address;
        hasPendingDisassembly_ = true;
    }

    snprintf(label, sizeof(label), "Go to Memory (%04X)", node.address);
    if (ImGui::MenuItem(label))
    {
        pendingGoToMemory_ = node.address;
        hasPendingMemory_ = true;
    }

    // Additional info
    ImGui::Separator();
    if (!node.comment.empty())
        ImGui::TextDisabled("Comment: %s", node.comment.c_str());
    if (node.hasSize)
        ImGui::TextDisabled("Size: %u", node.size);
    if (!node.properties.empty())
    {
        ImGui::TextDisabled("Properties:");
        for (const auto &kv : node.properties)
            ImGui::TextDisabled("  %s: %s", kv.first.c_str(),
                                kv.second.stringValue.c_str());
    }
}

// ---------------------------------------------------------------------------
// Render node editor
// ---------------------------------------------------------------------------

void CallGraphWindow::renderNodeEditor(IDebugBackend &backend)
{
    if (!editorContext_) return;

    ed::SetCurrentEditor(editorContext_);
    ed::Begin("Call Graph Editor", ImVec2(0.0f, 0.0f));

    // --- Draw nodes -------------------------------------------------------
    // NodeId = address (stable identity across rebuilds).
    // PinId  = address * 4 (+ 0 for input, + 1 for output).
    // LinkId = (source << 16) | target (stable, unique per edge).

    for (const auto &node : model_.nodes)
    {
        uintptr_t addr = static_cast<uintptr_t>(node.address);
        ed::BeginNode(ed::NodeId(addr));

        renderNodeContent(node);

        // Input pin (for incoming edges)
        ed::BeginPin(ed::PinId(addr * 4), ed::PinKind::Input);
        ImGui::Text(" in");
        ed::EndPin();

        ImGui::SameLine();

        // Output pin (for outgoing edges)
        ed::BeginPin(ed::PinId(addr * 4 + 1), ed::PinKind::Output);
        ImGui::Text("out ");
        ed::EndPin();

        ed::EndNode();

        // Context menu on right-click
        ed::NodeId contextNodeId;
        if (ed::ShowNodeContextMenu(&contextNodeId))
        {
            // NodeId IS the address (stable identity)
            selectedNodeAddress_ = static_cast<uint16_t>(
                static_cast<uintptr_t>(contextNodeId));
            ImGui::OpenPopup("node_context");
        }
    }

    // Render context menu popup if open
    if (ImGui::IsPopupOpen("node_context"))
    {
        if (ImGui::BeginPopup("node_context"))
        {
            auto it = model_.addressToIndex.find(selectedNodeAddress_);
            if (it != model_.addressToIndex.end())
            {
                const auto &node = model_.nodes[it->second];
                renderNodeContextMenu(node);
            }
            ImGui::EndPopup();
        }
    }

    // --- Draw links -------------------------------------------------------

    for (const auto &edge : model_.edges)
    {
        uintptr_t srcAddr = static_cast<uintptr_t>(edge.source);
        uintptr_t tgtAddr = static_cast<uintptr_t>(edge.target);

        // Stable LinkId from edge endpoints
        ed::LinkId linkId(static_cast<uintptr_t>(
            (static_cast<uint32_t>(edge.source) << 16) | static_cast<uint32_t>(edge.target)));

        // Output pin of source → Input pin of target
        ed::Link(linkId,
                 ed::PinId(srcAddr * 4 + 1),  // output pin
                 ed::PinId(tgtAddr * 4));      // input pin
    }

    // Cache current zoom for toolbar display
    currentZoom_ = ed::GetCurrentZoom();

    // --- Deferred navigation actions (after nodes submitted) --------------

    if (navigateToShowAll_)
    {
        ed::NavigateToContent(0.3f);
        navigateToShowAll_ = false;
    }
    if (navigateToSelection_)
    {
        ed::NavigateToSelection(true, 0.3f);
        navigateToSelection_ = false;
    }

    ed::End();
    ed::SetCurrentEditor(nullptr);
}

// ---------------------------------------------------------------------------
// Main render
// ---------------------------------------------------------------------------

void CallGraphWindow::render(IDebugBackend &backend)
{
    if (!visible_) return;

    // Window title with state indicator
    std::string title = "Call Graph";
    if (state_ == State::Outdated)
        title += " — outdated";
    else if (state_ == State::NotBuilt)
        title += " (not built)";

    ImGui::SetNextWindowSize(ImVec2(800, 600), ImGuiCond_FirstUseEver);

    if (!ImGui::Begin(title.c_str(), &visible_))
    {
        ImGui::End();
        return;
    }

    // Check if RDB changed externally → mark outdated
    if (state_ == State::Built && outdated_)
    {
        state_ = State::Outdated;
    }

    // --- Toolbar ----------------------------------------------------------

    if (ImGui::Button("Build"))
    {
        build(backend);
    }

    // Navigation toolbar (only when graph is built)
    if (state_ == State::Built || state_ == State::Outdated)
    {
        ImGui::SameLine();
        if (ImGui::Button("Show All"))
        {
            navigateToShowAll_ = true;
        }

        ImGui::SameLine();
        if (ImGui::Button("Selection"))
        {
            navigateToSelection_ = true;
        }
    }

    ImGui::SameLine();

    // Statistics
    if (state_ == State::Built || state_ == State::Outdated)
    {
        ImGui::Text("Nodes: %zu  |  Links: %zu  |  Unresolved: %zu  |  Zoom: %.0f%%",
                    model_.nodes.size(), model_.edges.size(), model_.unresolvedCount,
                    currentZoom_ * 100.0f);
    }

    ImGui::Separator();

    // --- Content based on state -------------------------------------------

    switch (state_)
    {
    case State::NotBuilt:
        ImGui::TextDisabled("No graph built.");
        ImGui::TextDisabled("Click 'Build' to create Call Graph from RDB.");
        break;

    case State::Building:
        ImGui::TextDisabled("Building...");
        break;

    case State::Built:
    case State::Outdated:
        if (model_.nodes.empty())
        {
            ImGui::TextDisabled("No objects in RDB.");
        }
        else
        {
            renderNodeEditor(backend);
        }
        break;
    }

    ImGui::End();

    // --- Process pending navigation (outside ImGui::Begin/End) ------------

    if (hasPendingDisassembly_ && onGoToDisassembly)
    {
        onGoToDisassembly(pendingGoToDisassembly_);
        hasPendingDisassembly_ = false;
    }
    if (hasPendingMemory_ && onGoToMemoryInspector)
    {
        onGoToMemoryInspector(pendingGoToMemory_);
        hasPendingMemory_ = false;
    }
}
