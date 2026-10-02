#pragma once

#include <string>
#include <map>
#include <vector>

// ---------------------------------------------------------------------------
// ConfigManager — manages config.ini (application-level settings).
//
// Unlike WorkspaceManager (which saves window layouts and is only saved
// on explicit user action), ConfigManager handles internal application
// data like Recent ROMs list, last ROM directory, etc.
//
// config.ini is stored next to the executable and is saved automatically
// when data changes (with debounce).
// ---------------------------------------------------------------------------

class ConfigManager
{
public:
    static constexpr int MAX_RECENT_ROMS = 10;

    // Initialize with path to directory containing config.ini
    // (typically next to the executable).
    void initialize(const std::string &configDir);

    // Save on exit (flush if dirty)
    void shutdown();

    // Call every frame — saves if dirty + debounce elapsed
    void autosave();

    // Key-value access
    std::string get(const std::string &key, const std::string &defaultVal = "") const;
    void set(const std::string &key, const std::string &value);

    // Recent ROMs management
    void addRecentRom(const std::string &path);
    const std::vector<std::string>& getRecentRoms() const { return recentRoms_; }

    bool isDirty() const { return dirty_; }

private:
    std::string configDir_;
    std::map<std::string, std::string> data_;
    std::vector<std::string> recentRoms_;
    bool dirty_ = false;
    double lastDirtyTime_ = 0.0;

    static constexpr double kAutosaveDelay = 1.0;

    std::string configFilePath() const;
    bool loadFromFile();
    bool saveToFile();
};
