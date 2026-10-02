#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <map>

// ---------------------------------------------------------------------------
// RDB Types — Stage 6.11
//
// Core data structures for ROM Database:
//   - RDB object types (function, variable, data, etc.)
//   - RDB property (key-value with typed values)
//   - RDB object (address, type, name, size, comment, properties, links)
//   - ROM identity (file, size, SHA-256)
//   - RDB metadata (format, platform, version)
//
// No GUI, Board, MCP, or Agent dependency — fully testable in isolation.
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// Object types
// ---------------------------------------------------------------------------

enum class RdbObjectType
{
    Function,
    Variable,
    Data,
    Table,
    String,
    Code,
    Label,
    Unknown
};

// Convert to/from JSON string representation.
const char *rdbObjectTypeToString(RdbObjectType type);
RdbObjectType rdbObjectTypeFromString(const std::string &str);

// ---------------------------------------------------------------------------
// Text normalization — single source of truth for RDB free-form text
//
// Guarantees that no comment / string-property value can ever carry a line
// break into layout.asm, into the .rdb on disk, or into tool output. A line
// break (CR/LF, CRLF, or the UTF-8 forms U+0085, U+2028, U+2029) together with
// any spaces/tabs hugging it collapses to a single space; runs of 2+ spaces
// collapse to one; the result is trimmed. Everything else (Cyrillic, ';',
// U+2192, U+00D7, tabs not adjacent to a break) is preserved verbatim.
//
// Properties:
//   * idempotent: normalizeRdbText(normalizeRdbText(s)) == normalizeRdbText(s)
//   * lossless:   only line breaks and their adjacent whitespace are removed
//   * the result never contains '\n' or '\r'
// ---------------------------------------------------------------------------
inline std::string normalizeRdbText(const std::string &s)
{
    if (s.empty()) return s;

    const size_t n = s.size();

    // Length of a line-break token at `pos`, or 0 if none. Recognizes \r\n
    // (as a single break), \r, \n, and the multi-byte UTF-8 sequences for
    // U+0085 (C2 85), U+2028 (E2 80 A8) and U+2029 (E2 80 A9).
    auto breakLen = [&s](size_t pos) -> size_t {
        const size_t n = s.size();
        if (pos >= n) return 0;
        const unsigned char c = static_cast<unsigned char>(s[pos]);
        if (c == '\r') return (pos + 1 < n && s[pos + 1] == '\n') ? 2 : 1;
        if (c == '\n') return 1;
        if (c == 0xC2 && pos + 1 < n &&
            static_cast<unsigned char>(s[pos + 1]) == 0x85)
            return 2;                                        // U+0085 NEL
        if (c == 0xE2 && pos + 2 < n &&
            static_cast<unsigned char>(s[pos + 1]) == 0x80 &&
            (static_cast<unsigned char>(s[pos + 2]) == 0xA8 ||
             static_cast<unsigned char>(s[pos + 2]) == 0xA9))
            return 3;                                        // U+2028 / U+2029
        return 0;
    };
    auto isHSpace = [](char c) { return c == ' ' || c == '\t'; };

    // Pass 1: replace every run of (break + hugging whitespace) with a single
    // '\n' sentinel. All real line breaks are consumed here, so a '\n' left in
    // `mark` is unambiguously a sentinel introduced by us.
    std::string mark;
    mark.reserve(n);
    size_t i = 0;
    while (i < n) {
        size_t bl = breakLen(i);
        if (bl > 0) {
            while (!mark.empty() && isHSpace(mark.back())) mark.pop_back();
            size_t j = i + bl;
            while (j < n && isHSpace(s[j])) ++j;
            while (j < n) {                                  // absorb further breaks
                size_t b2 = breakLen(j);
                if (b2 == 0) break;
                j += b2;
                while (j < n && isHSpace(s[j])) ++j;
            }
            mark.push_back('\n');
            i = j;
        } else {
            mark.push_back(s[i]);
            ++i;
        }
    }

    // Pass 2: sentinel -> one space; any run of 2+ spaces -> one space; tabs
    // (not adjacent to a break) and all other bytes are kept verbatim.
    std::string out;
    out.reserve(mark.size());
    for (size_t k = 0; k < mark.size(); ++k) {
        char c = mark[k];
        if (c == '\n') { out.push_back(' '); continue; }
        if (c == ' ') {
            size_t m = k;
            while (m < mark.size() && mark[m] == ' ') ++m;
            out.push_back(' ');                              // whole run -> one space
            k = m - 1;
            continue;
        }
        out.push_back(c);
    }

    // Trim leading / trailing spaces.
    size_t a = 0, b = out.size();
    while (a < b && out[a] == ' ') ++a;
    while (b > a && out[b - 1] == ' ') --b;
    return out.substr(a, b - a);
}

// ---------------------------------------------------------------------------
// Property value — supports string, integer, boolean, and double
// ---------------------------------------------------------------------------

struct RdbPropertyValue
{
    enum class Type { String, Integer, Boolean, Double };

    Type type = Type::String;

    std::string stringValue;
    int64_t     intValue   = 0;
    bool        boolValue  = false;
    double      doubleValue = 0.0;

    // Convenience constructors
    static RdbPropertyValue fromString(const std::string &v);
    static RdbPropertyValue fromInt(int64_t v);
    static RdbPropertyValue fromBool(bool v);
    static RdbPropertyValue fromDouble(double v);

    bool operator==(const RdbPropertyValue &other) const;
    bool operator!=(const RdbPropertyValue &other) const { return !(*this == other); }
};

// ---------------------------------------------------------------------------
// ROM identity
// ---------------------------------------------------------------------------

struct RdbRomIdentity
{
    std::string file;
    uint64_t    size    = 0;
    std::string sha256;

    bool operator==(const RdbRomIdentity &other) const;
    bool operator!=(const RdbRomIdentity &other) const { return !(*this == other); }

    // True if all fields are populated.
    bool isComplete() const;
};

// ---------------------------------------------------------------------------
// RDB Object
// ---------------------------------------------------------------------------

struct RdbObject
{
    uint16_t     address = 0;
    RdbObjectType type   = RdbObjectType::Unknown;
    std::string  name;
    uint32_t     size    = 0;     // 0 = unknown
    bool         hasSize = false;
    std::string  comment;

    // Additional properties (key → typed value)
    std::map<std::string, RdbPropertyValue> properties;

    // Links to other RDB objects (target addresses)
    std::vector<uint16_t> links;

    bool operator==(const RdbObject &other) const;
    bool operator!=(const RdbObject &other) const { return !(*this == other); }
};
