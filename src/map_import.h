#pragma once

#include "map_loader.h"
#include "rdb_controller.h"

#include <cstdint>
#include <string>

// Forward declaration
class SymbolDatabase;

// ---------------------------------------------------------------------------
// MAP Import — Stage 6.11
//
// Adapter between Z88DK MAP Loader and RDB Controller.
// Converts parsed MAP symbols into RDB objects.
//
// MAP Loader knows nothing about RDB internals.
// RDB Controller knows nothing about MAP syntax.
// This module bridges them.
// ---------------------------------------------------------------------------

namespace MapImport
{
    // Import MAP symbols into an RDB Controller.
    // Skips symbols whose address already exists in RDB (no overwrite).
    // Returns the number of objects actually added.
    int importMapToRdb(const MapLoadResult &mapResult, RdbController &rdb);

    // Sync RDB objects to a SymbolDatabase (for backward compatibility
    // with GUI windows that still read from SymbolDatabase).
    // Skips symbols whose address already exists in the database.
    // Returns the number of symbols actually added.
    int syncRdbToSymbolDatabase(const RdbController &rdb, SymbolDatabase &db);
}
