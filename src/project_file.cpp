#include "project_file.h"
#include "symbol_database.h"
#include "backend.h"  // for DebuggerBreakpoint

#include <fstream>
#include <sstream>
#include <cstdio>
#include <cstring>

// ---------------------------------------------------------------------------
// Helper: escape JSON string
// ---------------------------------------------------------------------------

static std::string escapeJson(const std::string &s)
{
    std::string result;
    for (char c : s) {
        switch (c) {
            case '"':  result += "\\\""; break;
            case '\\': result += "\\\\"; break;
            case '\n': result += "\\n"; break;
            case '\r': result += "\\r"; break;
            case '\t': result += "\\t"; break;
            default:   result += c; break;
        }
    }
    return result;
}

// ---------------------------------------------------------------------------
// Helper: format address as hex string
// ---------------------------------------------------------------------------

static std::string addrToHex(uint16_t addr)
{
    char buf[8];
    snprintf(buf, sizeof(buf), "%04X", addr);
    return buf;
}

// ---------------------------------------------------------------------------
// Helper: parse hex string to address
// ---------------------------------------------------------------------------

static bool hexToAddr(const std::string &s, uint16_t &addr)
{
    unsigned int a = 0;
    if (sscanf(s.c_str(), "%x", &a) != 1) return false;
    if (a > 0xFFFF) return false;
    addr = static_cast<uint16_t>(a);
    return true;
}

// ---------------------------------------------------------------------------
// Generate .dbg path from ROM path
// ---------------------------------------------------------------------------

std::string ProjectFile::dbgPathFromRom(const std::string &romPath)
{
    size_t dotPos = romPath.rfind('.');
    if (dotPos == std::string::npos) {
        return romPath + ".dbg";
    }
    return romPath.substr(0, dotPos) + ".dbg";
}

// ---------------------------------------------------------------------------
// Save
// ---------------------------------------------------------------------------

bool ProjectFile::save(const std::string &path,
                       const SymbolDatabase &db,
                       const std::vector<DebuggerBreakpoint> &breakpoints,
                       const std::string &romPath)
{
    std::ofstream file(path);
    if (!file.is_open()) return false;

    file << "{\n";
    file << "    \"version\": 1,\n";
    file << "    \"rom_path\": \"" << escapeJson(romPath) << "\",\n";

    // Symbols
    file << "    \"symbols\": [\n";
    auto symbols = db.allSymbols();
    for (size_t i = 0; i < symbols.size(); ++i) {
        const auto &sym = symbols[i];
        file << "        {\"address\": \"" << addrToHex(sym.address)
             << "\", \"name\": \"" << escapeJson(sym.name)
             << "\", \"type\": \"" << (sym.type == SymbolType::Function ? "function" : "label")
             << "\", \"comment\": \"" << escapeJson(sym.comment) << "\"}";
        if (i + 1 < symbols.size()) file << ",";
        file << "\n";
    }
    file << "    ],\n";

    // Regions
    file << "    \"regions\": [\n";
    auto regions = db.allRegions();
    for (size_t i = 0; i < regions.size(); ++i) {
        const auto &reg = regions[i];
        const char *typeStr = "unknown";
        if (reg.type == MemoryRegionType::Code) typeStr = "code";
        else if (reg.type == MemoryRegionType::Data) typeStr = "data";

        file << "        {\"start\": \"" << addrToHex(reg.start)
             << "\", \"end\": \"" << addrToHex(reg.end)
             << "\", \"type\": \"" << typeStr
             << "\", \"comment\": \"" << escapeJson(reg.comment) << "\"}";
        if (i + 1 < regions.size()) file << ",";
        file << "\n";
    }
    file << "    ],\n";

    // Breakpoints
    file << "    \"breakpoints\": [";
    for (size_t i = 0; i < breakpoints.size(); ++i) {
        if (breakpoints[i].enabled) {
            file << "\"" << addrToHex(breakpoints[i].address) << "\"";
            if (i + 1 < breakpoints.size()) file << ", ";
        }
    }
    file << "]\n";

    file << "}\n";

    return true;
}

// ---------------------------------------------------------------------------
// Load
// ---------------------------------------------------------------------------

bool ProjectFile::load(const std::string &path,
                       SymbolDatabase &db,
                       std::vector<DebuggerBreakpoint> &breakpoints,
                       std::string &romPath)
{
    std::ifstream file(path);
    if (!file.is_open()) return false;

    // Read entire file
    std::stringstream buffer;
    buffer << file.rdbuf();
    std::string content = buffer.str();

    // Simple JSON parser (very basic, handles our specific format)
    // This is a minimal parser — not robust against malformed JSON

    db.clear();
    breakpoints.clear();
    romPath.clear();

    // Find rom_path
    size_t pos = content.find("\"rom_path\"");
    if (pos != std::string::npos) {
        pos = content.find(":", pos);
        if (pos != std::string::npos) {
            size_t start = content.find("\"", pos + 1);
            size_t end = content.find("\"", start + 1);
            if (start != std::string::npos && end != std::string::npos) {
                romPath = content.substr(start + 1, end - start - 1);
            }
        }
    }

    // Parse symbols array
    pos = content.find("\"symbols\"");
    if (pos != std::string::npos) {
        size_t arrStart = content.find("[", pos);
        size_t arrEnd = content.find("]", arrStart);
        if (arrStart != std::string::npos && arrEnd != std::string::npos) {
            std::string symbolsSection = content.substr(arrStart + 1, arrEnd - arrStart - 1);

            // Parse each symbol object
            size_t objPos = 0;
            while ((objPos = symbolsSection.find("{", objPos)) != std::string::npos) {
                size_t objEnd = symbolsSection.find("}", objPos);
                if (objEnd == std::string::npos) break;

                std::string obj = symbolsSection.substr(objPos, objEnd - objPos + 1);

                // Extract fields
                uint16_t addr = 0;
                std::string name;
                SymbolType type = SymbolType::Label;
                std::string comment;

                // Address
                size_t addrPos = obj.find("\"address\"");
                if (addrPos != std::string::npos) {
                    size_t start = obj.find("\"", obj.find(":", addrPos) + 1);
                    size_t end = obj.find("\"", start + 1);
                    if (start != std::string::npos && end != std::string::npos) {
                        std::string addrStr = obj.substr(start + 1, end - start - 1);
                        hexToAddr(addrStr, addr);
                    }
                }

                // Name
                size_t namePos = obj.find("\"name\"");
                if (namePos != std::string::npos) {
                    size_t start = obj.find("\"", obj.find(":", namePos) + 1);
                    size_t end = obj.find("\"", start + 1);
                    if (start != std::string::npos && end != std::string::npos) {
                        name = obj.substr(start + 1, end - start - 1);
                    }
                }

                // Type
                size_t typePos = obj.find("\"type\"");
                if (typePos != std::string::npos) {
                    size_t start = obj.find("\"", obj.find(":", typePos) + 1);
                    size_t end = obj.find("\"", start + 1);
                    if (start != std::string::npos && end != std::string::npos) {
                        std::string typeStr = obj.substr(start + 1, end - start - 1);
                        type = (typeStr == "function") ? SymbolType::Function : SymbolType::Label;
                    }
                }

                // Comment
                size_t commentPos = obj.find("\"comment\"");
                if (commentPos != std::string::npos) {
                    size_t start = obj.find("\"", obj.find(":", commentPos) + 1);
                    size_t end = obj.find("\"", start + 1);
                    if (start != std::string::npos && end != std::string::npos) {
                        comment = obj.substr(start + 1, end - start - 1);
                    }
                }

                if (!name.empty()) {
                    db.addSymbol(addr, name, type);
                    if (!comment.empty()) {
                        db.setComment(addr, comment);
                    }
                }

                objPos = objEnd + 1;
            }
        }
    }

    // Parse regions array
    pos = content.find("\"regions\"");
    if (pos != std::string::npos) {
        size_t arrStart = content.find("[", pos);
        size_t arrEnd = content.find("]", arrStart);
        if (arrStart != std::string::npos && arrEnd != std::string::npos) {
            std::string regionsSection = content.substr(arrStart + 1, arrEnd - arrStart - 1);

            size_t objPos = 0;
            while ((objPos = regionsSection.find("{", objPos)) != std::string::npos) {
                size_t objEnd = regionsSection.find("}", objPos);
                if (objEnd == std::string::npos) break;

                std::string obj = regionsSection.substr(objPos, objEnd - objPos + 1);

                uint16_t start = 0, end = 0;
                MemoryRegionType type = MemoryRegionType::Unknown;
                std::string comment;

                // Start
                size_t startPos = obj.find("\"start\"");
                if (startPos != std::string::npos) {
                    size_t s = obj.find("\"", obj.find(":", startPos) + 1);
                    size_t e = obj.find("\"", s + 1);
                    if (s != std::string::npos && e != std::string::npos) {
                        hexToAddr(obj.substr(s + 1, e - s - 1), start);
                    }
                }

                // End
                size_t endPos = obj.find("\"end\"");
                if (endPos != std::string::npos) {
                    size_t s = obj.find("\"", obj.find(":", endPos) + 1);
                    size_t e = obj.find("\"", s + 1);
                    if (s != std::string::npos && e != std::string::npos) {
                        hexToAddr(obj.substr(s + 1, e - s - 1), end);
                    }
                }

                // Type
                size_t typePos = obj.find("\"type\"");
                if (typePos != std::string::npos) {
                    size_t s = obj.find("\"", obj.find(":", typePos) + 1);
                    size_t e = obj.find("\"", s + 1);
                    if (s != std::string::npos && e != std::string::npos) {
                        std::string typeStr = obj.substr(s + 1, e - s - 1);
                        if (typeStr == "code") type = MemoryRegionType::Code;
                        else if (typeStr == "data") type = MemoryRegionType::Data;
                    }
                }

                // Comment
                size_t commentPos = obj.find("\"comment\"");
                if (commentPos != std::string::npos) {
                    size_t s = obj.find("\"", obj.find(":", commentPos) + 1);
                    size_t e = obj.find("\"", s + 1);
                    if (s != std::string::npos && e != std::string::npos) {
                        comment = obj.substr(s + 1, e - s - 1);
                    }
                }

                db.setRegion(start, end, type);
                if (!comment.empty()) {
                    // Note: MemoryRegion doesn't have a setComment method in current API
                    // We could add one, but for now just set the region
                }

                objPos = objEnd + 1;
            }
        }
    }

    // Parse breakpoints array
    pos = content.find("\"breakpoints\"");
    if (pos != std::string::npos) {
        size_t arrStart = content.find("[", pos);
        size_t arrEnd = content.find("]", arrStart);
        if (arrStart != std::string::npos && arrEnd != std::string::npos) {
            std::string bpSection = content.substr(arrStart + 1, arrEnd - arrStart - 1);

            size_t bpPos = 0;
            while ((bpPos = bpSection.find("\"", bpPos)) != std::string::npos) {
                size_t bpEnd = bpSection.find("\"", bpPos + 1);
                if (bpEnd == std::string::npos) break;

                std::string addrStr = bpSection.substr(bpPos + 1, bpEnd - bpPos - 1);
                uint16_t addr = 0;
                if (hexToAddr(addrStr, addr)) {
                    DebuggerBreakpoint bp;
                    bp.address = addr;
                    bp.enabled = true;
                    breakpoints.push_back(bp);
                }

                bpPos = bpEnd + 1;
            }
        }
    }

    return true;
}
