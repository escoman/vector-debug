#include "map_loader.h"

#include <fstream>
#include <sstream>
#include <algorithm>
#include <cctype>
#include <cstdio>

// ---------------------------------------------------------------------------
// Helper: trim whitespace from both ends
// ---------------------------------------------------------------------------

static std::string trim(const std::string &s)
{
    size_t start = s.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) return "";
    size_t end = s.find_last_not_of(" \t\r\n");
    return s.substr(start, end - start + 1);
}

// ---------------------------------------------------------------------------
// Helper: case-insensitive string search for substring
// ---------------------------------------------------------------------------

static bool containsCI(const std::string &haystack, const std::string &needle)
{
    std::string h = haystack, n = needle;
    std::transform(h.begin(), h.end(), h.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    std::transform(n.begin(), n.end(), n.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    return h.find(n) != std::string::npos;
}

// ---------------------------------------------------------------------------
// Helper: parse hex address from "$0252" or "$C000" format
// Returns true on success, sets addr.
// ---------------------------------------------------------------------------

static bool parseHexAddress(const std::string &s, uint16_t &addr)
{
    // Expect "$" prefix followed by hex digits
    if (s.empty() || s[0] != '$') return false;

    std::string hex = s.substr(1);
    if (hex.empty()) return false;

    // Validate hex digits
    for (char c : hex) {
        if (!std::isxdigit(static_cast<unsigned char>(c))) return false;
    }

    unsigned int value = 0;
    if (sscanf(hex.c_str(), "%x", &value) != 1) return false;

    // Reject addresses beyond 16-bit range
    if (value > 0xFFFF) return false;

    addr = static_cast<uint16_t>(value);
    return true;
}

// ---------------------------------------------------------------------------
// Helper: extract source location from MAP comment
//
// Z88DK source locations look like:
//   main.c::main::0::1:109
//   ../../lib/gfx/mode.c::gfx_set_mode::0::0:39
//   clr.asm:161
//
// We extract: file = everything before the last "::" or ":",
//             line = number after the last ":"
// ---------------------------------------------------------------------------

static void extractSourceLocation(const std::string &comment,
                                  std::string &sourceFile, int &sourceLine)
{
    sourceFile.clear();
    sourceLine = 0;

    // Look for patterns like "file::func::...:line" or "file:line"
    // The source location is typically after "addr, public, " or "addr, local, "
    // and before any other metadata

    // Find the first token that looks like a source reference
    // (contains .c, .asm, .h, etc. followed by : or ::)
    size_t pos = 0;
    while (pos < comment.size()) {
        // Skip whitespace and commas
        while (pos < comment.size() &&
               (comment[pos] == ' ' || comment[pos] == ',' || comment[pos] == '\t'))
            pos++;

        // Find end of this token (next comma or end)
        size_t tokenEnd = comment.find(',', pos);
        if (tokenEnd == std::string::npos) tokenEnd = comment.size();

        std::string token = trim(comment.substr(pos, tokenEnd - pos));

        // Check if this token contains a source location
        // Look for patterns: "file::...:line" or "file:line"
        if (!token.empty()) {
            // Find the last colon — the number after it should be the line
            size_t lastColon = token.rfind(':');
            if (lastColon != std::string::npos && lastColon + 1 < token.size()) {
                // Check if what follows the last colon is a number
                std::string lineStr = token.substr(lastColon + 1);
                bool isNumber = !lineStr.empty();
                for (char c : lineStr) {
                    if (!std::isdigit(static_cast<unsigned char>(c))) {
                        isNumber = false;
                        break;
                    }
                }

                if (isNumber) {
                    sourceLine = atoi(lineStr.c_str());

                    // Extract the file name — everything before the first "::" or ":"
                    // that precedes the line number
                    // For "main.c::main::0::1:109", file = "main.c"
                    // For "clr.asm:161", file = "clr.asm"
                    size_t firstSep = token.find("::");
                    if (firstSep != std::string::npos) {
                        sourceFile = token.substr(0, firstSep);
                    } else {
                        // Simple "file:line" format
                        sourceFile = token.substr(0, lastColon);
                    }

                    // Trim any leading path components if desired
                    // (ТЗ says "at least source file + line")
                    return;
                }
            }
        }

        pos = tokenEnd;
    }
}

// ---------------------------------------------------------------------------
// Format validation: check if content looks like a Z88DK MAP file
//
// Strategy: scan for the characteristic "= $" pattern that appears in
// every Z88DK MAP symbol definition. Old keyboard mapping .map files
// will not contain this pattern.
// ---------------------------------------------------------------------------

static bool validateZ88dkFormat(const std::string &content)
{
    // A valid Z88DK MAP must contain at least one line with "= $"
    // (symbol = $address)
    std::istringstream stream(content);
    std::string line;
    int nonEmptyLines = 0;
    int formatMatches = 0;

    while (std::getline(stream, line)) {
        std::string trimmed = trim(line);
        if (trimmed.empty()) continue;
        nonEmptyLines++;

        // Check for "= $" pattern
        if (trimmed.find("= $") != std::string::npos ||
            trimmed.find("=$") != std::string::npos) {
            formatMatches++;
        }
    }

    // Require at least one "= $" match to consider it a Z88DK MAP
    if (formatMatches == 0) {
        return false;
    }

    // Also check that the file doesn't look like a keyboard mapping config
    // (old .map files typically have sections like [keyboard], key = value, etc.)
    if (containsCI(content, "[keyboard]") ||
        containsCI(content, "[mapping]") ||
        containsCI(content, "[keys]")) {
        return false;
    }

    return true;
}

// ---------------------------------------------------------------------------
// Parse a single MAP line
//
// Expected format:
//   _main = $0252 ; addr, public, main.c::main::0::1:109
//   fp_c000_inner = $01D1 ; addr, local
//
// Returns true if the line was successfully parsed.
// ---------------------------------------------------------------------------

static bool parseMapLine(const std::string &line, MapSymbol &sym)
{
    std::string trimmed = trim(line);
    if (trimmed.empty() || trimmed[0] == ';') return false;

    // Find "= $" or "=$" — the core pattern of a MAP symbol definition
    size_t eqPos = std::string::npos;
    size_t dollarPos = std::string::npos;

    // Look for "= $" pattern
    eqPos = trimmed.find("= $");
    if (eqPos != std::string::npos) {
        dollarPos = eqPos + 2;  // position of '$'
    } else {
        // Try "=$" (no space)
        eqPos = trimmed.find("=$");
        if (eqPos != std::string::npos) {
            dollarPos = eqPos + 1;
        }
    }

    if (eqPos == std::string::npos) return false;

    // Extract symbol name (everything before '=', trimmed)
    std::string name = trim(trimmed.substr(0, eqPos));
    if (name.empty()) return false;

    // Extract address: find the end of the $hex token
    size_t addrEnd = dollarPos + 1;  // skip '$'
    while (addrEnd < trimmed.size() &&
           std::isxdigit(static_cast<unsigned char>(trimmed[addrEnd]))) {
        addrEnd++;
    }

    std::string addrStr = trimmed.substr(dollarPos, addrEnd - dollarPos);
    uint16_t address = 0;
    if (!parseHexAddress(addrStr, address)) return false;

    // Extract comment (everything after address, after optional ';')
    std::string comment;
    size_t semicolonPos = trimmed.find(';', addrEnd);
    if (semicolonPos != std::string::npos) {
        comment = trim(trimmed.substr(semicolonPos + 1));
    }

    // Parse comment fields: "addr, public, source_location"
    bool isAddr = false;
    bool isConst = false;
    MapSymbolVisibility visibility = MapSymbolVisibility::Public;

    if (!comment.empty()) {
        // Split by commas
        std::istringstream cs(comment);
        std::string field;
        while (std::getline(cs, field, ',')) {
            std::string f = trim(field);
            if (f == "addr") {
                isAddr = true;
            } else if (f == "const") {
                isConst = true;
            } else if (f == "public") {
                visibility = MapSymbolVisibility::Public;
            } else if (f == "local") {
                visibility = MapSymbolVisibility::Local;
            }
            // Other fields (like "defn", "globl", "def") are ignored
        }
    }

    // Stage 6.2.1: Skip const entries — they are compile-time constants,
    // not memory addresses. Loading them would block real addr symbols
    // at the same address (e.g. STACK_TOP=$0100 const blocks start=$0100 addr).
    if (isConst) return false;

    // Extract source location from comment
    std::string sourceFile;
    int sourceLine = 0;
    extractSourceLocation(comment, sourceFile, sourceLine);

    // Fill result
    sym.name = name;
    sym.address = address;
    sym.visibility = visibility;
    sym.isAddress = isAddr;
    sym.sourceFile = sourceFile;
    sym.sourceLine = sourceLine;

    return true;
}

// ---------------------------------------------------------------------------
// MapLoader::parseMapContent
// ---------------------------------------------------------------------------

MapLoadResult MapLoader::parseMapContent(const std::string &content)
{
    MapLoadResult result;

    // Step 1: Format validation
    if (!validateZ88dkFormat(content)) {
        result.success = false;
        result.errorMessage = "not a Z88DK MAP file (format validation failed)";
        return result;
    }

    // Step 2: Parse line by line
    std::istringstream stream(content);
    std::string line;

    while (std::getline(stream, line)) {
        std::string trimmed = trim(line);
        if (trimmed.empty() || trimmed[0] == ';') continue;

        MapSymbol sym;
        if (parseMapLine(trimmed, sym)) {
            result.symbols.push_back(sym);
            result.parsedCount++;
        } else {
            result.skippedLines++;
        }
    }

    result.success = true;
    return result;
}

// ---------------------------------------------------------------------------
// MapLoader::loadMapFile
// ---------------------------------------------------------------------------

MapLoadResult MapLoader::loadMapFile(const std::string &path)
{
    MapLoadResult result;

    std::ifstream file(path);
    if (!file.is_open()) {
        result.success = false;
        result.errorMessage = "cannot open file: " + path;
        return result;
    }

    // Read entire file
    std::stringstream buffer;
    buffer << file.rdbuf();
    std::string content = buffer.str();

    if (content.empty()) {
        result.success = false;
        result.errorMessage = "empty MAP file";
        return result;
    }

    return parseMapContent(content);
}

// ---------------------------------------------------------------------------
// MapLoader::mapPathFromRom
// ---------------------------------------------------------------------------

std::string MapLoader::mapPathFromRom(const std::string &romPath)
{
    size_t dotPos = romPath.rfind('.');
    if (dotPos == std::string::npos) {
        return romPath + ".map";
    }
    return romPath.substr(0, dotPos) + ".map";
}
