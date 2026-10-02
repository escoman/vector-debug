#include "config_manager.h"

#include "imgui.h"

#include <fstream>
#include <sstream>
#include <cstdio>
#include <algorithm>
#include <sys/stat.h>

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

void ConfigManager::initialize(const std::string &configDir)
{
    configDir_ = configDir;
    loadFromFile();
}

void ConfigManager::shutdown()
{
    if (dirty_) {
        saveToFile();
    }
}

// ---------------------------------------------------------------------------
// Autosave (debounced)
// ---------------------------------------------------------------------------

void ConfigManager::autosave()
{
    if (!dirty_) return;

    double now = ImGui::GetTime();
    if (lastDirtyTime_ == 0.0) {
        lastDirtyTime_ = now;
        return;
    }

    if (now - lastDirtyTime_ >= kAutosaveDelay) {
        saveToFile();
        lastDirtyTime_ = 0.0;
    }
}

// ---------------------------------------------------------------------------
// Key-value access
// ---------------------------------------------------------------------------

std::string ConfigManager::get(const std::string &key,
                                const std::string &defaultVal) const
{
    auto it = data_.find(key);
    return (it != data_.end()) ? it->second : defaultVal;
}

void ConfigManager::set(const std::string &key, const std::string &value)
{
    auto it = data_.find(key);
    if (it != data_.end() && it->second == value) return;
    data_[key] = value;
    dirty_ = true;
    lastDirtyTime_ = 0.0;  // reset debounce timer
}

// ---------------------------------------------------------------------------
// Recent ROMs management
// ---------------------------------------------------------------------------

void ConfigManager::addRecentRom(const std::string &path)
{
    if (path.empty()) return;

    // Remove ALL occurrences if already in list (not just the first one)
    recentRoms_.erase(
        std::remove(recentRoms_.begin(), recentRoms_.end(), path),
        recentRoms_.end());

    // Insert at front
    recentRoms_.insert(recentRoms_.begin(), path);

    // Cap at maximum
    if (static_cast<int>(recentRoms_.size()) > MAX_RECENT_ROMS) {
        recentRoms_.resize(MAX_RECENT_ROMS);
    }

    dirty_ = true;
    lastDirtyTime_ = 0.0;
}

// ---------------------------------------------------------------------------
// File I/O
// ---------------------------------------------------------------------------

std::string ConfigManager::configFilePath() const
{
    return configDir_ + "/config.ini";
}

bool ConfigManager::loadFromFile()
{
    std::string path = configFilePath();
    std::ifstream file(path);
    if (!file.good()) return false;

    std::string line;
    while (std::getline(file, line)) {
        // Trim trailing whitespace
        while (!line.empty() && (line.back() == '\n' || line.back() == '\r' ||
                                  line.back() == ' ')) {
            line.pop_back();
        }
        if (line.empty() || line[0] == '[' || line[0] == '#' || line[0] == ';')
            continue;

        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;

        std::string key = line.substr(0, eq);
        std::string value = line.substr(eq + 1);

        // Handle RecentRoms specially — parse pipe-separated list
        if (key == "RecentRoms") {
            recentRoms_.clear();
            size_t start = 0;
            while (start < value.size()) {
                size_t pos = value.find('|', start);
                if (pos == std::string::npos) pos = value.size();
                std::string entry = value.substr(start, pos - start);
                // Validate: must be a non-empty path to an existing file
                if (!entry.empty() && entry.size() < 4096 && entry[0] == '/') {
                    struct stat st;
                    if (stat(entry.c_str(), &st) == 0 && S_ISREG(st.st_mode)) {
                        // Deduplicate: skip if already in list
                        if (std::find(recentRoms_.begin(), recentRoms_.end(), entry)
                                == recentRoms_.end()) {
                            recentRoms_.push_back(entry);
                        }
                    }
                }
                start = pos + 1;
            }
        } else {
            data_[key] = value;
        }
    }

    return true;
}

bool ConfigManager::saveToFile()
{
    std::string path = configFilePath();

    // Build content
    std::string content;
    content += "[Config]\n";
    for (const auto &pair : data_) {
        content += pair.first + "=" + pair.second + "\n";
    }

    // Serialize RecentRoms as pipe-separated list
    if (!recentRoms_.empty()) {
        content += "RecentRoms=";
        for (size_t i = 0; i < recentRoms_.size(); ++i) {
            if (i > 0) content += '|';
            content += recentRoms_[i];
        }
        content += '\n';
    }

    // Skip write if content hasn't changed
    {
        std::ifstream existing(path);
        if (existing.good()) {
            std::string oldContent((std::istreambuf_iterator<char>(existing)),
                                    std::istreambuf_iterator<char>());
            if (oldContent == content) {
                dirty_ = false;
                return true;
            }
        }
    }

    std::ofstream file(path);
    if (!file.good()) return false;
    file << content;
    dirty_ = false;
    return true;
}
