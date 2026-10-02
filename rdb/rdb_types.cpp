#include "rdb_types.h"

#include <algorithm>
#include <cstring>

// ---------------------------------------------------------------------------
// RdbObjectType conversion
// ---------------------------------------------------------------------------

const char *rdbObjectTypeToString(RdbObjectType type)
{
    switch (type) {
        case RdbObjectType::Function: return "function";
        case RdbObjectType::Variable: return "variable";
        case RdbObjectType::Data:     return "data";
        case RdbObjectType::Table:    return "table";
        case RdbObjectType::String:   return "string";
        case RdbObjectType::Code:     return "code";
        case RdbObjectType::Label:    return "label";
        case RdbObjectType::Unknown:  return "unknown";
    }
    return "unknown";
}

RdbObjectType rdbObjectTypeFromString(const std::string &str)
{
    // Case-insensitive comparison.
    std::string lower = str;
    for (auto &c : lower) {
        if (c >= 'A' && c <= 'Z') c = c - 'A' + 'a';
    }
    if (lower == "function") return RdbObjectType::Function;
    if (lower == "variable") return RdbObjectType::Variable;
    if (lower == "data")     return RdbObjectType::Data;
    if (lower == "table")    return RdbObjectType::Table;
    if (lower == "string")   return RdbObjectType::String;
    if (lower == "code")     return RdbObjectType::Code;
    if (lower == "label")    return RdbObjectType::Label;
    return RdbObjectType::Unknown;
}

// ---------------------------------------------------------------------------
// RdbPropertyValue
// ---------------------------------------------------------------------------

RdbPropertyValue RdbPropertyValue::fromString(const std::string &v)
{
    RdbPropertyValue pv;
    pv.type = Type::String;
    pv.stringValue = v;
    return pv;
}

RdbPropertyValue RdbPropertyValue::fromInt(int64_t v)
{
    RdbPropertyValue pv;
    pv.type = Type::Integer;
    pv.intValue = v;
    return pv;
}

RdbPropertyValue RdbPropertyValue::fromBool(bool v)
{
    RdbPropertyValue pv;
    pv.type = Type::Boolean;
    pv.boolValue = v;
    return pv;
}

RdbPropertyValue RdbPropertyValue::fromDouble(double v)
{
    RdbPropertyValue pv;
    pv.type = Type::Double;
    pv.doubleValue = v;
    return pv;
}

bool RdbPropertyValue::operator==(const RdbPropertyValue &other) const
{
    if (type != other.type) return false;
    switch (type) {
        case Type::String:  return stringValue == other.stringValue;
        case Type::Integer: return intValue == other.intValue;
        case Type::Boolean: return boolValue == other.boolValue;
        case Type::Double:  return doubleValue == other.doubleValue;
    }
    return false;
}

// ---------------------------------------------------------------------------
// RdbRomIdentity
// ---------------------------------------------------------------------------

bool RdbRomIdentity::operator==(const RdbRomIdentity &other) const
{
    // Compare only content identifiers, not file path.
    // This allows moving ROM + RDB to different locations.
    return size == other.size
        && sha256 == other.sha256;
}

bool RdbRomIdentity::isComplete() const
{
    return !file.empty() && size > 0 && !sha256.empty();
}

// ---------------------------------------------------------------------------
// RdbObject
// ---------------------------------------------------------------------------

bool RdbObject::operator==(const RdbObject &other) const
{
    if (address != other.address) return false;
    if (type != other.type) return false;
    if (name != other.name) return false;
    if (size != other.size) return false;
    if (hasSize != other.hasSize) return false;
    if (comment != other.comment) return false;
    if (properties != other.properties) return false;

    // Links: compare as sorted sets (order is not semantically significant)
    if (links.size() != other.links.size()) return false;
    std::vector<uint16_t> a = links;
    std::vector<uint16_t> b = other.links;
    std::sort(a.begin(), a.end());
    std::sort(b.begin(), b.end());
    return a == b;
}
