#include "rom_file_dialog.h"

// Dear ImGui
#include "imgui.h"

// Directory listing
#include <dirent.h>
#include <sys/stat.h>
#include <cstring>
#include <algorithm>
#include <cctype>
#include <unistd.h>
#include <climits>

// ---------------------------------------------------------------------------
// Public interface
// ---------------------------------------------------------------------------

void RomFileDialog::show(const std::string &startDir,
                         const std::string &title,
                         const std::vector<std::string> &extensions)
{
    title_ = title;
    extensions_ = extensions;
    if (startDir.empty()) {
        char cwd[PATH_MAX];
        if (getcwd(cwd, sizeof(cwd))) {
            currentPath_ = cwd;
        } else {
            currentPath_ = "/";
        }
    } else if (startDir[0] != '/') {
        // Resolve relative path to absolute
        char resolved[PATH_MAX];
        if (realpath(startDir.c_str(), resolved)) {
            currentPath_ = resolved;
        } else {
            currentPath_ = startDir;
        }
    } else {
        currentPath_ = startDir;
    }
    selectedPath_.clear();
    selectedEntry_ = -1;
    filenameInput_[0] = '\0';
    needsRefresh_ = true;
    open_ = true;
}

bool RomFileDialog::render()
{
    if (!open_) return false;

    // Center the dialog
    ImGui::OpenPopup(title_.c_str());
    ImGui::SetNextWindowSize(ImVec2(500, 400), ImGuiCond_FirstUseEver);

    bool fileSelected = false;

    if (ImGui::BeginPopupModal(title_.c_str(), &open_, ImGuiWindowFlags_NoScrollbar)) {
        // Refresh entries if needed
        if (needsRefresh_) {
            refreshEntries();
            needsRefresh_ = false;
        }

        // Path input field
        ImGui::Text("Path:");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - 60);
        if (ImGui::InputText("##path", pathInput_, sizeof(pathInput_))) {
            // User edited path - update current path
            currentPath_ = pathInput_;
            needsRefresh_ = true;
        }
        ImGui::SameLine();
        if (ImGui::Button("Go")) {
            needsRefresh_ = true;
        }

        ImGui::Spacing();

        // File list with columns: Name
        ImGui::BeginChild("FileList", ImVec2(0, -40), true, ImGuiWindowFlags_None);

        // Header
        ImGui::TextColored(ImVec4(0.4f, 0.8f, 1.0f, 1.0f), "%-40s  %s", "Name", "Type");
        ImGui::Separator();

        // Parent directory entry
        bool parentClicked = false;
        if (ImGui::Selectable("..", selectedEntry_ == -2, ImGuiSelectableFlags_AllowDoubleClick)) {
            selectedEntry_ = -2;
            if (ImGui::IsMouseDoubleClicked(0)) {
                parentClicked = true;
            }
        }

        // File entries
        for (size_t i = 0; i < entries_.size(); ++i) {
            const auto &entry = entries_[i];
            char label[512];
            if (entry.isDir) {
                snprintf(label, sizeof(label), "[ %s ]", entry.name.c_str());
            } else {
                snprintf(label, sizeof(label), "  %s", entry.name.c_str());
            }

            bool isSelected = (selectedEntry_ == static_cast<int>(i));
            if (ImGui::Selectable(label, isSelected, ImGuiSelectableFlags_AllowDoubleClick)) {
                selectedEntry_ = static_cast<int>(i);

                // Update filename input
                snprintf(filenameInput_, sizeof(filenameInput_), "%s", entry.name.c_str());

                // Double-click on directory: navigate into it
                if (entry.isDir && ImGui::IsMouseDoubleClicked(0)) {
                    std::string newPath = currentPath_;
                    if (newPath.back() != '/') newPath += '/';
                    newPath += entry.name;
                    navigateTo(newPath);
                }
                // Double-click on file: select it
                else if (!entry.isDir && ImGui::IsMouseDoubleClicked(0)) {
                    fileSelected = true;
                }
            }
        }

        ImGui::EndChild();

        // Filename input
        ImGui::Text("File:");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
        ImGui::InputText("##filename", filenameInput_, sizeof(filenameInput_));

        // Buttons
        ImGui::SameLine();
        if (ImGui::Button("Open") || fileSelected) {
            // Build full path
            std::string fullPath;
            if (filenameInput_[0] == '/') {
                // Absolute path
                fullPath = filenameInput_;
            } else {
                fullPath = currentPath_;
                if (fullPath.back() != '/') fullPath += '/';
                fullPath += filenameInput_;
            }
            selectedPath_ = fullPath;
            if (onFileSelected) {
                onFileSelected(selectedPath_);
            }
            open_ = false;
            ImGui::EndPopup();
            return true;
        }

        ImGui::SameLine();
        if (ImGui::Button("Cancel") || parentClicked) {
            if (parentClicked) {
                goUp();
            } else {
                open_ = false;
            }
        }

        ImGui::EndPopup();
    } else {
        // Popup was closed via the X button
        open_ = false;
    }

    return false;
}

// ---------------------------------------------------------------------------
// Private helpers
// ---------------------------------------------------------------------------

bool RomFileDialog::matchesExtensions(const std::string &name) const
{
    // Find last dot
    size_t dotPos = name.rfind('.');
    if (dotPos == std::string::npos) return false;

    std::string ext = name.substr(dotPos);

    // Convert to lowercase for comparison
    std::string extLower;
    extLower.reserve(ext.size());
    for (char c : ext) {
        extLower += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }

    for (const auto &e : extensions_) {
        std::string eLower;
        eLower.reserve(e.size());
        for (char c : e) {
            eLower += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        }
        if (extLower == eLower) return true;
    }
    return false;
}

void RomFileDialog::refreshEntries()
{
    entries_.clear();
    selectedEntry_ = -1;

    // Update path input to reflect current path
    snprintf(pathInput_, sizeof(pathInput_), "%s", currentPath_.c_str());

    DIR *dir = opendir(currentPath_.c_str());
    if (!dir) {
        // Cannot open directory - stay where we are
        return;
    }

    // Collect directories and files separately
    std::vector<FileEntry> dirs;
    std::vector<FileEntry> files;

    struct dirent *entry;
    while ((entry = readdir(dir)) != nullptr) {
        std::string name = entry->d_name;

        // Skip hidden files and . / ..
        if (name.empty() || name[0] == '.') continue;

        // Build full path for stat
        std::string fullPath = currentPath_;
        if (fullPath.back() != '/') fullPath += '/';
        fullPath += name;

        struct stat st;
        if (stat(fullPath.c_str(), &st) != 0) continue;

        FileEntry fe;
        fe.name = name;
        fe.isDir = S_ISDIR(st.st_mode);

        if (fe.isDir) {
            dirs.push_back(fe);
        } else if (matchesExtensions(name)) {
            files.push_back(fe);
        }
    }
    closedir(dir);

    // Sort directories alphabetically
    std::sort(dirs.begin(), dirs.end(), [](const FileEntry &a, const FileEntry &b) {
        return a.name < b.name;
    });

    // Sort files alphabetically
    std::sort(files.begin(), files.end(), [](const FileEntry &a, const FileEntry &b) {
        return a.name < b.name;
    });

    // Combine: directories first, then files
    entries_.insert(entries_.end(), dirs.begin(), dirs.end());
    entries_.insert(entries_.end(), files.begin(), files.end());
}

void RomFileDialog::navigateTo(const std::string &path)
{
    currentPath_ = path;
    needsRefresh_ = true;
    filenameInput_[0] = '\0';
}

void RomFileDialog::goUp()
{
    // Find last slash
    size_t lastSlash = currentPath_.rfind('/');
    if (lastSlash == std::string::npos || lastSlash == 0) {
        // Already at root or no slash found
        return;
    }

    // Remove trailing slash if present
    std::string path = currentPath_;
    if (path.back() == '/') {
        path.pop_back();
        lastSlash = path.rfind('/');
        if (lastSlash == std::string::npos) {
            currentPath_ = "/";
        } else {
            currentPath_ = path.substr(0, lastSlash);
        }
    } else {
        currentPath_ = path.substr(0, lastSlash);
    }

    needsRefresh_ = true;
    filenameInput_[0] = '\0';
}
