#include "map_import.h"
#include "symbol_database.h"

#include <cstdio>

// ---------------------------------------------------------------------------
// MAP → RDB import
// ---------------------------------------------------------------------------

namespace MapImport
{

int importMapToRdb(const MapLoadResult &mapResult, RdbController &rdb)
{
    if (!mapResult.success) return 0;

    int added = 0;
    for (const auto &ms : mapResult.symbols) {
        // Classify: addr + public → Function, otherwise → Label
        RdbObjectType type = (ms.isAddress && ms.visibility == MapSymbolVisibility::Public)
            ? RdbObjectType::Function
            : RdbObjectType::Label;

        RdbObject obj;
        obj.address = ms.address;
        obj.type    = type;
        obj.name    = ms.name;

        // Store source location as properties (if present)
        if (!ms.sourceFile.empty()) {
            obj.properties["source_file"] = RdbPropertyValue::fromString(ms.sourceFile);
        }
        if (ms.sourceLine > 0) {
            obj.properties["source_line"] = RdbPropertyValue::fromInt(ms.sourceLine);
        }

        // addObject returns false if address already taken (first wins)
        if (rdb.addObject(obj)) {
            added++;
        }
    }

    return added;
}

// ---------------------------------------------------------------------------
// RDB → SymbolDatabase sync (backward compatibility)
// ---------------------------------------------------------------------------

int syncRdbToSymbolDatabase(const RdbController &rdb, SymbolDatabase &db)
{
    auto objects = rdb.listObjects();
    int added = 0;

    for (const auto &obj : objects) {
        // Skip if address already has a symbol
        if (db.findSymbol(obj.address)) continue;

        // Map RDB type → SymbolDatabase type
        SymbolType symType = (obj.type == RdbObjectType::Function)
            ? SymbolType::Function
            : SymbolType::Label;

        if (db.addSymbol(obj.address, obj.name, symType)) {
            // Set MAP-specific fields via const_cast (same pattern as existing code)
            auto *p = const_cast<DebugSymbol*>(db.findSymbol(obj.address));
            if (p) {
                p->fromMap = true;

                // Restore source location from RDB properties
                auto itFile = obj.properties.find("source_file");
                if (itFile != obj.properties.end() &&
                    itFile->second.type == RdbPropertyValue::Type::String) {
                    p->sourceFile = itFile->second.stringValue;
                }

                auto itLine = obj.properties.find("source_line");
                if (itLine != obj.properties.end() &&
                    itLine->second.type == RdbPropertyValue::Type::Integer) {
                    p->sourceLine = static_cast<int>(itLine->second.intValue);
                }
            }

            // Restore comment if present
            if (!obj.comment.empty()) {
                db.setComment(obj.address, obj.comment);
            }

            added++;
        }
    }

    return added;
}

} // namespace MapImport
