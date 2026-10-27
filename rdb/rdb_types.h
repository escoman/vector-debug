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

// ===========================================================================
// Stage 6.28 — Knowledge sections: Protocol / Ports / Watchlist
//
// Three new top-level RDB sections live alongside `objects`. They turn the RDB
// from "a database of ROM objects" into "a database of knowledge about a ROM":
//   - protocol  : append-only hypothesis journal (thesis + status + evidence)
//   - ports     : compiled hardware knowledge (port bit semantics / polarity)
//   - watchlist : named variables to print on checkpoint / breakpoint
//
// These types carry no GUI/Board/MCP/Agent dependency and are fully testable
// in isolation, exactly like RdbObject. All free-form text is normalized via
// normalizeRdbText at the Controller boundary and again at serialization.
// ===========================================================================

// ---------------------------------------------------------------------------
// Case-insensitive helpers used by the parse*() functions below.
// ---------------------------------------------------------------------------

namespace rdb_detail {
inline std::string toLower(std::string s) {
    for (auto &c : s) if (c >= 'A' && c <= 'Z') c = char(c - 'A' + 'a');
    return s;
}
inline std::string toUpper(std::string s) {
    for (auto &c : s) if (c >= 'a' && c <= 'z') c = char(c - 'a' + 'A');
    return s;
}
} // namespace rdb_detail

// ---------------------------------------------------------------------------
// Controller operation result (Stage 6.28).
// Existing object/link methods keep returning bool; the new knowledge sections
// need to distinguish *why* an operation failed, so they return this enum.
// The Agent API maps these onto ErrorCode.
// ---------------------------------------------------------------------------

enum class RdbOpResult
{
    None,             // success
    NotFound,         // entry / object does not exist
    Duplicate,        // already exists (watch entry name collision)
    InvalidStatus,    // protocol status string not in closed enum
    InvalidScope,     // protocol scope string not in closed enum
    InvalidMethod,    // protocol method string not in closed enum
    InvalidDirection, // port direction string not in closed enum
    InvalidPolarity,  // port polarity string not in closed enum
    MissingEvidence,  // VERIFIED*/CONFLICT/REFUTED without method+evidence
    Referenced,       // cannot remove: another active entry supersedes it
    InvalidThesis     // thesis empty after normalization
};

// ---------------------------------------------------------------------------
// Protocol status — closed enum, adapted from
// agent/knowledge/vector06c/verification.md classification.
// ---------------------------------------------------------------------------

enum class ProtocolStatus
{
    VerifiedByTrace,
    VerifiedBySource,
    Verified,
    PartiallyVerified,
    Conflict,
    Unverified,
    Refuted,
    Speculation
};

inline const char *protocolStatusToString(ProtocolStatus s) {
    switch (s) {
        case ProtocolStatus::VerifiedByTrace:    return "VERIFIED_BY_TRACE";
        case ProtocolStatus::VerifiedBySource:   return "VERIFIED_BY_SOURCE";
        case ProtocolStatus::Verified:           return "VERIFIED";
        case ProtocolStatus::PartiallyVerified:  return "PARTIALLY_VERIFIED";
        case ProtocolStatus::Conflict:           return "CONFLICT";
        case ProtocolStatus::Unverified:         return "UNVERIFIED";
        case ProtocolStatus::Refuted:            return "REFUTED";
        case ProtocolStatus::Speculation:        return "SPECULATION";
    }
    return "UNVERIFIED";
}

// Case-insensitive parse; returns false for unknown values.
inline bool parseProtocolStatus(const std::string &str, ProtocolStatus &out) {
    const std::string u = rdb_detail::toUpper(str);
    if (u == "VERIFIED_BY_TRACE")   { out = ProtocolStatus::VerifiedByTrace;   return true; }
    if (u == "VERIFIED_BY_SOURCE")  { out = ProtocolStatus::VerifiedBySource;  return true; }
    if (u == "VERIFIED")            { out = ProtocolStatus::Verified;          return true; }
    if (u == "PARTIALLY_VERIFIED")  { out = ProtocolStatus::PartiallyVerified; return true; }
    if (u == "CONFLICT")            { out = ProtocolStatus::Conflict;          return true; }
    if (u == "UNVERIFIED")          { out = ProtocolStatus::Unverified;        return true; }
    if (u == "REFUTED")             { out = ProtocolStatus::Refuted;           return true; }
    if (u == "SPECULATION")         { out = ProtocolStatus::Speculation;       return true; }
    return false;
}

// Statuses that must carry a method + evidence (Stage 6.28 §4.4).
inline bool protocolStatusRequiresEvidence(ProtocolStatus s) {
    switch (s) {
        case ProtocolStatus::VerifiedByTrace:
        case ProtocolStatus::VerifiedBySource:
        case ProtocolStatus::Verified:
        case ProtocolStatus::PartiallyVerified:
        case ProtocolStatus::Conflict:
        case ProtocolStatus::Refuted:
            return true;
        default:
            return false;
    }
}

// ---------------------------------------------------------------------------
// Protocol scope — what a claim is about (Stage 6.28 §4.5).
// ---------------------------------------------------------------------------

enum class ProtocolScope { Rom, Hardware, Tooling };

inline const char *protocolScopeToString(ProtocolScope s) {
    switch (s) {
        case ProtocolScope::Rom:      return "rom";
        case ProtocolScope::Hardware: return "hardware";
        case ProtocolScope::Tooling:  return "tooling";
    }
    return "rom";
}

inline bool parseProtocolScope(const std::string &str, ProtocolScope &out) {
    const std::string l = rdb_detail::toLower(str);
    if (l == "rom")      { out = ProtocolScope::Rom;      return true; }
    if (l == "hardware") { out = ProtocolScope::Hardware; return true; }
    if (l == "tooling")  { out = ProtocolScope::Tooling;  return true; }
    return false;
}

// ---------------------------------------------------------------------------
// Protocol method — how the claim was / will be checked.
// ---------------------------------------------------------------------------

enum class ProtocolMethod { Trace, Diff, PrimarySource, GoldenTest, Static };

inline const char *protocolMethodToString(ProtocolMethod m) {
    switch (m) {
        case ProtocolMethod::Trace:        return "trace";
        case ProtocolMethod::Diff:         return "diff";
        case ProtocolMethod::PrimarySource:return "primary-source";
        case ProtocolMethod::GoldenTest:   return "golden-test";
        case ProtocolMethod::Static:       return "static";
    }
    return "trace";
}

inline bool parseProtocolMethod(const std::string &str, ProtocolMethod &out) {
    const std::string l = rdb_detail::toLower(str);
    if (l == "trace")           { out = ProtocolMethod::Trace;        return true; }
    if (l == "diff")            { out = ProtocolMethod::Diff;         return true; }
    if (l == "primary-source")  { out = ProtocolMethod::PrimarySource;return true; }
    if (l == "golden-test")     { out = ProtocolMethod::GoldenTest;   return true; }
    if (l == "static")          { out = ProtocolMethod::Static;       return true; }
    return false;
}

// ---------------------------------------------------------------------------
// Protocol entry — one hypothesis-journal record.
// ---------------------------------------------------------------------------

struct RdbProtocolEntry
{
    std::string   id;            // "P-NNNN", assigned by the controller
    std::string   timestamp;     // ISO-8601 UTC, assigned by the controller
    std::string   thesis;        // one-line claim (normalized)
    ProtocolStatus status = ProtocolStatus::Unverified;
    ProtocolScope  scope  = ProtocolScope::Rom;

    bool      hasAddress = false;
    uint16_t  address    = 0;

    bool          hasMethod = false;
    ProtocolMethod method   = ProtocolMethod::Trace;

    std::string evidence;        // the "улика" (normalized)

    std::vector<std::string> links;   // ids of related protocol entries
    std::string supersedes;           // id this entry replaces (or empty)
    std::string supersededBy;         // id that replaces this one (controller-set)

    bool isSuperseded() const { return !supersededBy.empty(); }
};

// ---------------------------------------------------------------------------
// Port direction / polarity enums.
// ---------------------------------------------------------------------------

enum class PortDirection { In, Out, InOut };

inline const char *portDirectionToString(PortDirection d) {
    switch (d) {
        case PortDirection::In:    return "input";
        case PortDirection::Out:   return "output";
        case PortDirection::InOut: return "in-out";
    }
    return "input";
}

inline bool parsePortDirection(const std::string &str, PortDirection &out) {
    const std::string l = rdb_detail::toLower(str);
    if (l == "input")   { out = PortDirection::In;    return true; }
    if (l == "output")  { out = PortDirection::Out;   return true; }
    if (l == "in-out")  { out = PortDirection::InOut; return true; }
    return false;
}

enum class PortPolarity { ActiveHigh, ActiveLow, NA };

inline const char *portPolarityToString(PortPolarity p) {
    switch (p) {
        case PortPolarity::ActiveHigh: return "active-high";
        case PortPolarity::ActiveLow:  return "active-low";
        case PortPolarity::NA:         return "n/a";
    }
    return "n/a";
}

inline bool parsePortPolarity(const std::string &str, PortPolarity &out) {
    const std::string l = rdb_detail::toLower(str);
    if (l == "active-high") { out = PortPolarity::ActiveHigh; return true; }
    if (l == "active-low")  { out = PortPolarity::ActiveLow;  return true; }
    if (l == "n/a")         { out = PortPolarity::NA;         return true; }
    return false;
}

// ---------------------------------------------------------------------------
// Port bit semantics + port entry.
// ---------------------------------------------------------------------------

struct RdbPortBit
{
    int         index = 0;
    std::string meaning;
    bool operator==(const RdbPortBit &o) const {
        return index == o.index && meaning == o.meaning;
    }
};

struct RdbPortEntry
{
    uint8_t      port      = 0;
    std::string  name;
    PortDirection direction = PortDirection::In;
    PortPolarity  polarity  = PortPolarity::NA;
    std::vector<RdbPortBit> bits;
    std::string  comment;
};

// ---------------------------------------------------------------------------
// Watchlist entry — a named variable to print on stop.
// ---------------------------------------------------------------------------

struct RdbWatchEntry
{
    std::string name;
    uint16_t    address = 0;
    uint32_t    length  = 1;
    std::string format  = "uint8";   // uint8/int8/uint16-le/bcd/bits/...
    bool        hasObjectAddress = false;
    uint16_t    objectAddress    = 0;
};

// ---------------------------------------------------------------------------
// Protocol query filter (Stage 6.28 §4.7). Empty/`false` flags = no constraint.
// ---------------------------------------------------------------------------

struct ProtocolFilter
{
    bool        hasStatus = false;
    ProtocolStatus status = ProtocolStatus::Unverified;

    bool        hasScope = false;
    ProtocolScope scope  = ProtocolScope::Rom;

    bool        hasAddress = false;
    uint16_t    address    = 0;

    bool        activeOnly = false;   // only entries not superseded
    std::string substring;            // case-insensitive match on thesis
    size_t      maxEntries = 0;       // 0 = unlimited
};
