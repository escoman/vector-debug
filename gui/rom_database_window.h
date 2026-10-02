#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

// Forward declarations
class IDebugBackend;
class RdbController;

// ---------------------------------------------------------------------------
// ROM Database Window — Stage 6.11
//
// Displays and edits the contents of the ROM Database (.rdb).
// Columns: Address | Type | Name | Size | Comment
//
// Features:
//   - View all RDB objects sorted by address
//   - Filter by name/address
//   - Inline editing of name, type, size, comment
//   - Add/remove objects
//   - Dirty indicator in title bar
//   - Save command
// ---------------------------------------------------------------------------

class RomDatabaseWindow
{
public:
    RomDatabaseWindow() = default;

    void render(IDebugBackend &backend);

    void setVisible(bool v) { visible_ = v; }
    bool isVisible() const { return visible_; }
    bool &getVisibleRef() { return visible_; }
    void requestRefresh() { needsRefresh_ = true; }

    // Navigation callbacks
    std::function<void(uint16_t address)> onGoToDisassembly;
    std::function<void(uint16_t address)> onGoToMemoryInspector;

private:
    bool visible_ = true;
    bool needsRefresh_ = true;

    // Filter
    char filterBuffer_[128] = "";

    // Sorting: 0=address, 1=name, 2=type
    int sortColumn_ = 0;
    bool sortReverse_ = false;

    // Inline editing state
    bool editingName_ = false;
    uint16_t editingAddress_ = 0;
    char editNameBuffer_[128] = "";

    bool editingComment_ = false;
    char editCommentBuffer_[4096] = "";

    // Add object dialog
    bool showAddDialog_ = false;
    char addAddrBuffer_[16] = "";
    char addNameBuffer_[128] = "";
    int addTypeIndex_ = 0;  // index into type names

    // Object details dialog (double-click)
    bool showObjectDialog_ = false;
    uint16_t objectDialogAddress_ = 0;
    char objDlgNameBuffer_[128] = "";
    int objDlgTypeIndex_ = 0;
    char objDlgSizeBuffer_[16] = "";
    bool objDlgHasSize_ = false;
    char objDlgCommentBuffer_[4096] = "";

    // Context menu state
    uint16_t contextAddress_ = 0;

    // Cached object list (refreshed on needsRefresh_)
    struct CachedObject
    {
        uint16_t address;
        std::string typeStr;
        std::string name;
        std::string sizeStr;
        std::string comment;
    };
    std::vector<CachedObject> cachedObjects_;

    void refreshCache(const RdbController &rdb);
    bool matchesFilter(const CachedObject &obj) const;

    // Open object details dialog for given address
    void openObjectDialog(const RdbController &rdb, uint16_t address);

    // Render the object details modal dialog
    void renderObjectDialog(RdbController &rdb);
};
