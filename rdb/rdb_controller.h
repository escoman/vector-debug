#pragma once

#include "rdb_types.h"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

// ---------------------------------------------------------------------------
// RDB Controller — Stage 6.11
//
// Sole owner and gateway to the ROM Database (.rdb).
//
// Responsibilities:
//   - Load / save / reload .rdb files
//   - Manage RDB objects (add, update, remove, query)
//   - Manage comments, properties, links
//   - Track dirty state
//   - Verify ROM identity (SHA-256 match)
//   - Atomic save (write to .tmp, then rename)
//
// Dependencies: none (no ImGui, SDL, OpenGL, Board, MCP, Agent).
// Only depends on nlohmann/json (header-only, in thirdparty/).
// ---------------------------------------------------------------------------

class RdbController
{
public:
    RdbController();
    ~RdbController();

    // -- Lifecycle ----------------------------------------------------------

    // Load RDB from a .rdb file.
    // Returns false on failure (file not found, parse error, etc.).
    bool load(const std::string &path);

    // Reload from the same path that was used in load().
    bool reload();

    // Save RDB to disk.
    // If !dirty_, returns true immediately without writing.
    // If no path was set (never loaded) and !dirty_, returns true.
    // Uses atomic save: write to .tmp, then rename.
    bool save();

    // Save to a specific path (saveAs).
    bool saveAs(const std::string &path);

    // Close the current RDB, resetting to empty state.
    void close();

    // -- State --------------------------------------------------------------

    bool isLoaded() const;
    bool isDirty() const;
    std::string getPath() const;
    std::string getPlatform() const;
    int getVersion() const;
    RdbRomIdentity getRomIdentity() const;

    // Check if the loaded RDB matches the given ROM identity.
    // Returns true if matching, false if mismatch or no RDB loaded.
    bool matchesRom(const RdbRomIdentity &rom) const;

    // Whether the RDB file exists on disk at the current path.
    bool existsOnDisk() const;

    // -- Objects ------------------------------------------------------------

    // Get object at address. Returns nullptr if not found.
    const RdbObject *getObject(uint16_t address) const;

    // Find object by name (case-sensitive). Returns nullptr if not found.
    const RdbObject *findObject(const std::string &name) const;

    // List all objects sorted by address.
    std::vector<RdbObject> listObjects() const;

    // Number of objects.
    size_t objectCount() const;

    // Add a new object. Returns false if address already exists.
    // Sets dirty = true on success.
    bool addObject(const RdbObject &object);

    // Update an existing object (replaces all fields).
    // Returns false if object not found.
    // Sets dirty = true only if data actually changed.
    bool updateObject(const RdbObject &object);

    // Remove object at address. Returns false if not found.
    // Sets dirty = true on success.
    bool removeObject(uint16_t address);

    // -- Comments -----------------------------------------------------------

    // Set comment for object at address.
    // Returns false if object not found.
    // Sets dirty = true only if comment actually changed.
    bool setComment(uint16_t address, const std::string &comment);

    // Get comment for object at address. Returns empty string if not found.
    std::string getComment(uint16_t address) const;

    // -- Properties ---------------------------------------------------------

    // Set a property on an object.
    // Returns false if object not found.
    // Sets dirty = true only if property actually changed.
    bool setProperty(uint16_t address, const std::string &name,
                     const RdbPropertyValue &value);

    // Get a property value. Returns nullptr if object or property not found.
    const RdbPropertyValue *getProperty(uint16_t address,
                                        const std::string &name) const;

    // Remove a property from an object.
    // Returns false if object or property not found.
    // Sets dirty = true on success.
    bool removeProperty(uint16_t address, const std::string &name);

    // -- Links --------------------------------------------------------------

    // Get links for an object. Returns empty vector if not found.
    std::vector<uint16_t> getLinks(uint16_t address) const;

    // Add a link from source to target.
    // Returns false if source object not found.
    // Sets dirty = true only if link was actually added (not duplicate).
    bool addLink(uint16_t address, uint16_t targetAddress);

    // Remove a link from source to target.
    // Returns false if source object or link not found.
    // Sets dirty = true on success.
    bool removeLink(uint16_t address, uint16_t targetAddress);

    // Replace all links for an object.
    // Returns false if source object not found.
    // Sets dirty = true only if links actually changed.
    bool setLinks(uint16_t address, const std::vector<uint16_t> &links);

    // Check if a specific link exists.
    bool hasLink(uint16_t address, uint16_t targetAddress) const;

    // -- ROM identity -------------------------------------------------------

    // Set ROM identity (called when loading a ROM alongside RDB).
    void setRomIdentity(const RdbRomIdentity &identity);

    // -- Initialization (create empty RDB) ----------------------------------

    // Initialize an empty RDB with the given platform and ROM identity.
    // Does NOT write to disk — use save() for that.
    // If path is provided, save() will write to that location.
    void initialize(const std::string &platform, const RdbRomIdentity &rom,
                    const std::string &path = "");

    // Internal state — public forward declaration only.
    // Full definition is in rdb_controller.cpp (pimpl pattern).
    // Users cannot access Impl members without the .cpp definition.
public:
    struct Impl;

private:
    Impl *impl_;

    // Not copyable
    RdbController(const RdbController &) = delete;
    RdbController &operator=(const RdbController &) = delete;
};
