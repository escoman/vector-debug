#pragma once

#include <string>
#include <vector>
#include <map>
#include <functional>

// ---------------------------------------------------------------------------
// WorkspaceManager — GUI-only component for saving/loading window layouts.
//
// Uses Dear ImGui's native serialization (SaveIniSettingsToMemory /
// LoadIniSettingsFromMemory) for dock layout, plus a custom [Visibility]
// section for window show/hide state.
//
// Does NOT depend on Board, Memory, CPU, IO, TV, DebugAdapter, or
// IDebugBackend.  Works only with ImGui and the filesystem.
// ---------------------------------------------------------------------------

class WorkspaceManager
{
public:
    struct WindowVisibility {
        std::string name;
        bool *visiblePtr = nullptr;
    };

    // Lifecycle
    void initialize(const std::string &workspacesDir);
    void shutdown();  // save on exit

    // Register pointers to window visibility booleans.
    // Does NOT load workspace or generate presets — call applyPendingWorkspace()
    // between frames (after endFrame / before beginFrame) to complete setup.
    void setWindowVisibilityRefs(std::vector<WindowVisibility> refs);

    // Returns true if there are pending visibility refs waiting to be applied.
    bool hasPendingWorkspaceInit() const { return pendingVisibilityRefsSet_; }

    // Apply pending refs: generate preset files and load current workspace.
    // MUST be called between frames (after endFrame, before beginFrame).
    void applyPendingWorkspace();

    // Workspace operations
    void switchWorkspace(const std::string &name);
    void saveCurrentWorkspace();
    void saveWorkspaceAs(const std::string &name);
    void deleteWorkspace(const std::string &name);
    void resetWorkspace();

    // Autosave — call every frame; saves if dirty + debounce elapsed
    void autosave();

    // Process any deferred operations that require a temp ImGui context.
    // Call between frames (after endFrame, before beginFrame).
    void processDeferredOps();

    // Queries
    const std::string &currentWorkspaceName() const;
    std::vector<std::string> listWorkspaces() const;
    bool isBuiltIn(const std::string &name) const;
    bool isDirty() const;
    void markDirty();

    // Settings — persistent key-value pairs stored in workspace ini.
    // Used for things like last ROM directory, etc.
    std::string getSetting(const std::string &key, const std::string &defaultVal = "") const;
    void setSetting(const std::string &key, const std::string &value);

    // Preset layout builders (public for testability)
    using LayoutBuilder = std::function<void(unsigned int dockspaceId)>;
    static void buildDefaultLayout(unsigned int dockspaceId);
    static void buildScreenAnalysisLayout(unsigned int dockspaceId);
    static void buildCpuAnalysisLayout(unsigned int dockspaceId);
    static void buildIoAnalysisLayout(unsigned int dockspaceId);
    static void buildMemoryAnalysisLayout(unsigned int dockspaceId);

private:
    std::string workspacesDir_;
    std::string currentName_ = "Default";
    std::string lastLoadedIni_;       // raw ini from last load (for Reset)
    std::map<std::string, bool*> visibilityRefs_;
    std::vector<WindowVisibility> pendingVisibilityRefs_;
    bool pendingVisibilityRefsSet_ = false;
    bool deferredResetNeeded_ = false;
    bool dirty_ = false;

    // Autosave debounce
    double lastDirtyTime_ = 0.0;
    static constexpr double kAutosaveDelay = 1.0;

    // Built-in workspace names and their layout builders
    static const std::vector<std::string> builtInNames_;
    static const std::map<std::string, LayoutBuilder> presets_;

    // File I/O
    std::string workspaceFilePath(const std::string &name) const;
    bool saveToFile(const std::string &name);
    bool loadFromFile(const std::string &name);
    void writeBuiltinIfMissing(const std::string &name);

    // Visibility serialization
    std::string serializeVisibility() const;
    void deserializeVisibility(const std::string &section);

    // Helpers
    static std::string sanitizeFilename(const std::string &name);
    void ensureDirectory() const;
    void saveCurrentVisibility();
    void restoreVisibility();
    std::map<std::string, bool> savedVisibility_;

    // Persistent settings (stored in [Settings] section of workspace ini)
    std::map<std::string, std::string> settings_;
};
