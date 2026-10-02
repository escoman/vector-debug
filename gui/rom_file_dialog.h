#pragma once

#include <string>
#include <vector>
#include <functional>

// Forward declarations
class IDebugBackend;

// ---------------------------------------------------------------------------
// File Dialog
//
// Custom file browser for selecting files with configurable extensions.
// Replaces the blocking zenity-based native dialog with a non-blocking
// ImGui-based file browser.
// ---------------------------------------------------------------------------

class RomFileDialog
{
public:
    RomFileDialog() {}

    // Show the dialog (call once to open)
    // extensions: list of accepted extensions, e.g. {".rom", ".r0m"} or {".wav"}
    void show(const std::string &startDir = "",
              const std::string &title = "Open ROM File",
              const std::vector<std::string> &extensions = {".rom", ".r0m"});

    // Render the dialog (call every frame). Returns true if a file was selected.
    // The callback is invoked with the selected file path.
    bool render();

    // Check if dialog is currently open
    bool isOpen() const { return open_; }

    // Get the last selected file path (valid after render() returns true)
    const std::string &selectedPath() const { return selectedPath_; }

    // Callback when file is selected
    std::function<void(const std::string &path)> onFileSelected;

private:
    bool open_ = false;
    std::string currentPath_;
    std::string selectedPath_;
    char pathInput_[1024] = "";
    char filenameInput_[256] = "";

    struct FileEntry {
        std::string name;
        bool isDir;
    };
    std::vector<FileEntry> entries_;
    int selectedEntry_ = -1;
    bool needsRefresh_ = true;

    // Refresh the file list for current directory
    void refreshEntries();

    // Check if filename matches the configured extensions
    bool matchesExtensions(const std::string &name) const;

    // Navigate to a directory
    void navigateTo(const std::string &path);

    // Go up one directory level
    void goUp();

    std::string title_;
    std::vector<std::string> extensions_;
};
