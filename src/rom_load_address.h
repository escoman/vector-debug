#pragma once

#include <string>
#include <cstdint>

// ---------------------------------------------------------------------------
// getRomLoadAddress — determine ROM load address from file extension.
//
// Pure function with no dependencies on Board, CPU, GUI, or ImGui.
// Reproduces the original vector06sdl logic from RomListItem.getOrg():
//   .rom  → 0x0100
//   .r0m  → 0x0000
//   .r1m  → 0x0100
//   ...
//   .r9m  → 0x0900
//
// Extension comparison is case-insensitive.
// Returns 0 for unknown extensions (default to .r0m behavior).
// ---------------------------------------------------------------------------

inline uint16_t getRomLoadAddress(const std::string &path)
{
    // Find the last dot in the filename
    size_t dot = path.rfind('.');
    if (dot == std::string::npos) {
        return 0x0000;  // no extension → default 0
    }

    std::string ext = path.substr(dot);

    // Convert to lowercase
    for (char &c : ext) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }

    // .rom → 0x0100
    if (ext == ".rom") {
        return 0x0100;
    }

    // .rNm → N * 0x0100, where N = 0..9
    if (ext.size() == 4 && ext[0] == '.' && ext[1] == 'r' &&
        ext[2] >= '0' && ext[2] <= '9' && ext[3] == 'm') {
        return static_cast<uint16_t>((ext[2] - '0') * 0x0100);
    }

    return 0x0000;  // unknown extension → default 0
}
