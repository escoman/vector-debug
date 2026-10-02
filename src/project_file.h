#pragma once

#include <string>
#include <vector>

// Forward declarations
class SymbolDatabase;
struct DebuggerBreakpoint;

// ---------------------------------------------------------------------------
// Project File — Stage 4.9
//
// Save/load analysis results (.dbg files) in JSON format.
// Format:
// {
//     "version": 1,
//     "rom_path": "/path/to/game.rom",
//     "symbols": [
//         {"address": "0345", "name": "DRAW_SPRITE", "type": "function", "comment": "..."},
//         ...
//     ],
//     "regions": [
//         {"start": "0000", "end": "03FF", "type": "code", "comment": "..."},
//         ...
//     ],
//     "breakpoints": ["0345", "7A20"]
// }
// ---------------------------------------------------------------------------

class ProjectFile
{
public:
    // Save symbol database and breakpoints to a .dbg file
    static bool save(const std::string &path,
                     const SymbolDatabase &db,
                     const std::vector<DebuggerBreakpoint> &breakpoints,
                     const std::string &romPath = "");

    // Load symbol database and breakpoints from a .dbg file
    static bool load(const std::string &path,
                     SymbolDatabase &db,
                     std::vector<DebuggerBreakpoint> &breakpoints,
                     std::string &romPath);

    // Generate .dbg path from ROM path (game.rom -> game.dbg)
    static std::string dbgPathFromRom(const std::string &romPath);
};
