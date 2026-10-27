#include "rdb_controller.h"
#include "nlohmann/json.hpp"

#include <algorithm>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>
#include <sys/stat.h>

using json = nlohmann::json;

// ---------------------------------------------------------------------------
// Internal state
// ---------------------------------------------------------------------------

struct RdbController::Impl
{
    std::string path;
    std::string platform = "vector06c";
    int version = 1;
    RdbRomIdentity romIdentity;
    bool loaded = false;
    bool dirty = false;
    bool fileExistsOnDisk = false;

    // Objects indexed by address
    std::map<uint16_t, RdbObject> objects;

    // -- Stage 6.28 knowledge sections -------------------------------------
    std::vector<RdbProtocolEntry> protocol;          // insertion order preserved
    std::map<uint8_t, RdbPortEntry> ports;
    std::map<std::string, RdbWatchEntry> watch;      // keyed by name (unique)
    int nextProtocolId = 1;                           // P-NNNN counter
};

// ---------------------------------------------------------------------------
// JSON helpers
// ---------------------------------------------------------------------------

static std::string addrToHex(uint16_t addr)
{
    char buf[8];
    snprintf(buf, sizeof(buf), "0x%04X", addr);
    return buf;
}

static bool hexToAddr(const std::string &s, uint16_t &addr)
{
    unsigned int a = 0;
    const char *str = s.c_str();
    // Skip "0x" or "0X" prefix if present
    if (str[0] == '0' && (str[1] == 'x' || str[1] == 'X')) {
        str += 2;
    }
    if (sscanf(str, "%x", &a) != 1) return false;
    if (a > 0xFFFF) return false;
    addr = static_cast<uint16_t>(a);
    return true;
}

// Strip line breaks from an object's free-form text: the comment and every
// string-typed property value. Names / addresses / sizes / links are untouched.
static void normalizeObjectText(RdbObject &obj)
{
    obj.comment = normalizeRdbText(obj.comment);
    for (auto &kv : obj.properties) {
        if (kv.second.type == RdbPropertyValue::Type::String)
            kv.second.stringValue = normalizeRdbText(kv.second.stringValue);
    }
}

// Serialize a single RdbObject to JSON
static json objectToJson(const RdbObject &obj)
{
    json j;
    j["address"] = addrToHex(obj.address);
    j["type"] = rdbObjectTypeToString(obj.type);

    if (!obj.name.empty()) {
        j["name"] = obj.name;
    }
    if (obj.hasSize) {
        j["size"] = obj.size;
    }
    // R4: normalize at the last gate before bytes hit disk, so the .rdb is
    // guaranteed break-free even if some path bypassed the in-memory setters.
    std::string comment = normalizeRdbText(obj.comment);
    if (!comment.empty()) {
        j["comment"] = comment;
    }
    if (!obj.properties.empty()) {
        json props = json::object();
        for (const auto &kv : obj.properties) {
            switch (kv.second.type) {
                case RdbPropertyValue::Type::String:
                    props[kv.first] = normalizeRdbText(kv.second.stringValue);
                    break;
                case RdbPropertyValue::Type::Integer:
                    props[kv.first] = kv.second.intValue;
                    break;
                case RdbPropertyValue::Type::Boolean:
                    props[kv.first] = kv.second.boolValue;
                    break;
                case RdbPropertyValue::Type::Double:
                    props[kv.first] = kv.second.doubleValue;
                    break;
            }
        }
        j["properties"] = props;
    }
    if (!obj.links.empty()) {
        json linksArr = json::array();
        for (uint16_t target : obj.links) {
            linksArr.push_back(addrToHex(target));
        }
        j["links"] = linksArr;
    }

    return j;
}

// Deserialize a single RdbObject from JSON
static RdbObject objectFromJson(const json &j)
{
    RdbObject obj;

    if (j.contains("address") && j["address"].is_string()) {
        hexToAddr(j["address"].get<std::string>(), obj.address);
    }

    if (j.contains("type") && j["type"].is_string()) {
        obj.type = rdbObjectTypeFromString(j["type"].get<std::string>());
    }

    if (j.contains("name") && j["name"].is_string()) {
        obj.name = j["name"].get<std::string>();
    }

    if (j.contains("size") && j["size"].is_number()) {
        obj.size = j["size"].get<uint32_t>();
        obj.hasSize = true;
    }

    if (j.contains("comment") && j["comment"].is_string()) {
        // R3: sanitize legacy .rdb files on ingest so an old multi-line
        // comment can never survive into memory (and a later save).
        obj.comment = normalizeRdbText(j["comment"].get<std::string>());
    }

    if (j.contains("properties") && j["properties"].is_object()) {
        for (auto it = j["properties"].begin(); it != j["properties"].end(); ++it) {
            RdbPropertyValue pv;
            const auto &val = it.value();
            if (val.is_string()) {
                pv.type = RdbPropertyValue::Type::String;
                pv.stringValue = normalizeRdbText(val.get<std::string>());
            } else if (val.is_boolean()) {
                pv.type = RdbPropertyValue::Type::Boolean;
                pv.boolValue = val.get<bool>();
            } else if (val.is_number_integer()) {
                pv.type = RdbPropertyValue::Type::Integer;
                pv.intValue = val.get<int64_t>();
            } else if (val.is_number_float()) {
                pv.type = RdbPropertyValue::Type::Double;
                pv.doubleValue = val.get<double>();
            }
            obj.properties[it.key()] = pv;
        }
    }

    if (j.contains("links") && j["links"].is_array()) {
        for (const auto &linkVal : j["links"]) {
            if (linkVal.is_string()) {
                uint16_t target = 0;
                if (hexToAddr(linkVal.get<std::string>(), target)) {
                    obj.links.push_back(target);
                }
            }
        }
    }

    return obj;
}

// ---------------------------------------------------------------------------
// Stage 6.28 — knowledge-section JSON + id/timestamp helpers
// ---------------------------------------------------------------------------

// Current UTC time as ISO-8601 "YYYY-MM-DDTHH:MM:SSZ".
static std::string nowIso8601()
{
    std::time_t t = std::time(nullptr);
    std::tm tmv{};
#if defined(_WIN32)
    gmtime_s(&tmv, &t);
#else
    gmtime_r(&t, &tmv);
#endif
    char buf[64];
    snprintf(buf, sizeof(buf), "%04d-%02d-%02dT%02d:%02d:%02dZ",
             tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday,
             tmv.tm_hour, tmv.tm_min, tmv.tm_sec);
    return buf;
}

static std::string protoId(int n)
{
    char buf[16];
    snprintf(buf, sizeof(buf), "P-%04d", n);
    return buf;
}

// Highest P-NNNN suffix already present, for counter resume after load.
static int scanMaxProtoId(const std::vector<RdbProtocolEntry> &entries)
{
    int maxn = 0;
    for (const auto &e : entries) {
        if (e.id.size() >= 6 && e.id.rfind("P-", 0) == 0) {
            int n = atoi(e.id.c_str() + 2);
            if (n > maxn) maxn = n;
        }
    }
    return maxn;
}

static json portBitToJson(const RdbPortBit &b)
{
    json j;
    j["index"]   = b.index;
    j["meaning"] = normalizeRdbText(b.meaning);
    return j;
}

static json protocolToJson(const RdbProtocolEntry &e)
{
    json j;
    j["id"]        = e.id;
    j["timestamp"] = e.timestamp;
    j["thesis"]    = normalizeRdbText(e.thesis);
    j["status"]    = protocolStatusToString(e.status);
    j["scope"]     = protocolScopeToString(e.scope);
    if (e.hasAddress) j["address"] = addrToHex(e.address);
    if (e.hasMethod)  j["method"]  = protocolMethodToString(e.method);
    std::string ev = normalizeRdbText(e.evidence);
    if (!ev.empty()) j["evidence"] = ev;
    if (!e.links.empty()) {
        j["links"] = e.links;
    }
    if (!e.supersedes.empty())   j["supersedes"]   = e.supersedes;
    if (!e.supersededBy.empty()) j["superseded_by"] = e.supersededBy;
    return j;
}

static RdbProtocolEntry protocolFromJson(const json &j)
{
    RdbProtocolEntry e;
    if (j.contains("id") && j["id"].is_string())        e.id = j["id"].get<std::string>();
    if (j.contains("timestamp") && j["timestamp"].is_string())
        e.timestamp = j["timestamp"].get<std::string>();
    if (j.contains("thesis") && j["thesis"].is_string())
        e.thesis = normalizeRdbText(j["thesis"].get<std::string>());
    if (j.contains("status") && j["status"].is_string())
        parseProtocolStatus(j["status"].get<std::string>(), e.status);
    if (j.contains("scope") && j["scope"].is_string())
        parseProtocolScope(j["scope"].get<std::string>(), e.scope);
    if (j.contains("address") && j["address"].is_string()) {
        uint16_t a = 0;
        if (hexToAddr(j["address"].get<std::string>(), a)) {
            e.address = a; e.hasAddress = true;
        }
    }
    if (j.contains("method") && j["method"].is_string()) {
        ProtocolMethod m;
        if (parseProtocolMethod(j["method"].get<std::string>(), m)) {
            e.method = m; e.hasMethod = true;
        }
    }
    if (j.contains("evidence") && j["evidence"].is_string())
        e.evidence = normalizeRdbText(j["evidence"].get<std::string>());
    if (j.contains("links") && j["links"].is_array())
        for (const auto &l : j["links"])
            if (l.is_string()) e.links.push_back(l.get<std::string>());
    if (j.contains("supersedes") && j["supersedes"].is_string())
        e.supersedes = j["supersedes"].get<std::string>();
    if (j.contains("superseded_by") && j["superseded_by"].is_string())
        e.supersededBy = j["superseded_by"].get<std::string>();
    return e;
}

static json portToJson(const RdbPortEntry &p)
{
    json j;
    char buf[8];
    snprintf(buf, sizeof(buf), "0x%02X", p.port);
    j["port"] = buf;
    if (!p.name.empty()) j["name"] = normalizeRdbText(p.name);
    j["direction"] = portDirectionToString(p.direction);
    j["polarity"]  = portPolarityToString(p.polarity);
    if (!p.bits.empty()) {
        json bits = json::array();
        for (const auto &b : p.bits) bits.push_back(portBitToJson(b));
        j["bits"] = bits;
    }
    std::string c = normalizeRdbText(p.comment);
    if (!c.empty()) j["comment"] = c;
    return j;
}

static RdbPortEntry portFromJson(const json &j)
{
    RdbPortEntry p;
    if (j.contains("port") && j["port"].is_string()) {
        unsigned int v = 0;
        std::string s = j["port"].get<std::string>();
        if (s.size() && s[0] == '0' && (s.size() > 1 && (s[1] == 'x' || s[1] == 'X'))) s = s.substr(2);
        if (sscanf(s.c_str(), "%x", &v) == 1) p.port = static_cast<uint8_t>(v & 0xFF);
    }
    if (j.contains("name") && j["name"].is_string())
        p.name = normalizeRdbText(j["name"].get<std::string>());
    if (j.contains("direction") && j["direction"].is_string())
        parsePortDirection(j["direction"].get<std::string>(), p.direction);
    if (j.contains("polarity") && j["polarity"].is_string())
        parsePortPolarity(j["polarity"].get<std::string>(), p.polarity);
    if (j.contains("bits") && j["bits"].is_array()) {
        for (const auto &bj : j["bits"]) {
            if (!bj.is_object()) continue;
            RdbPortBit b;
            if (bj.contains("index") && bj["index"].is_number())
                b.index = bj["index"].get<int>();
            if (bj.contains("meaning") && bj["meaning"].is_string())
                b.meaning = normalizeRdbText(bj["meaning"].get<std::string>());
            p.bits.push_back(b);
        }
    }
    if (j.contains("comment") && j["comment"].is_string())
        p.comment = normalizeRdbText(j["comment"].get<std::string>());
    return p;
}

static json watchToJson(const RdbWatchEntry &w)
{
    json j;
    j["name"]    = normalizeRdbText(w.name);
    j["address"] = addrToHex(w.address);
    j["length"]  = w.length;
    j["format"]  = w.format;
    if (w.hasObjectAddress) j["object_address"] = addrToHex(w.objectAddress);
    return j;
}

static RdbWatchEntry watchFromJson(const json &j)
{
    RdbWatchEntry w;
    if (j.contains("name") && j["name"].is_string())
        w.name = normalizeRdbText(j["name"].get<std::string>());
    if (j.contains("address") && j["address"].is_string())
        hexToAddr(j["address"].get<std::string>(), w.address);
    if (j.contains("length") && j["length"].is_number())
        w.length = j["length"].get<uint32_t>();
    if (j.contains("format") && j["format"].is_string())
        w.format = j["format"].get<std::string>();
    if (j.contains("object_address") && j["object_address"].is_string()) {
        uint16_t a = 0;
        if (hexToAddr(j["object_address"].get<std::string>(), a)) {
            w.objectAddress = a; w.hasObjectAddress = true;
        }
    }
    return w;
}

// ---------------------------------------------------------------------------
// Full-document (de)serialization
// ---------------------------------------------------------------------------

// Serialize the full RDB to JSON
static json rdbToJson(const RdbController::Impl &impl)
{
    json root;
    root["format"] = "rdb";
    root["platform"] = impl.platform;

    // Effective version: bump to 2 only when a knowledge section is present,
    // otherwise preserve the loaded version so untouched v1 files stay v1.
    bool hasKnowledge = !impl.protocol.empty() || !impl.ports.empty() || !impl.watch.empty();
    int outVersion = hasKnowledge ? std::max(impl.version, 2) : impl.version;
    root["version"] = outVersion;

    if (impl.romIdentity.isComplete()) {
        json rom;
        rom["file"] = impl.romIdentity.file;
        rom["size"] = impl.romIdentity.size;
        rom["sha256"] = impl.romIdentity.sha256;
        root["rom"] = rom;
    }

    json objectsArr = json::array();
    for (const auto &kv : impl.objects) {
        objectsArr.push_back(objectToJson(kv.second));
    }
    root["objects"] = objectsArr;

    // -- Stage 6.28 knowledge sections (only written when non-empty) --------
    if (!impl.protocol.empty()) {
        json arr = json::array();
        for (const auto &e : impl.protocol) arr.push_back(protocolToJson(e));
        root["protocol"] = arr;
    }
    if (!impl.ports.empty()) {
        json arr = json::array();
        for (const auto &kv : impl.ports) arr.push_back(portToJson(kv.second));
        root["ports"] = arr;
    }
    if (!impl.watch.empty()) {
        json arr = json::array();
        // Deterministic order: sort by address, then name.
        std::vector<RdbWatchEntry> ws;
        ws.reserve(impl.watch.size());
        for (const auto &kv : impl.watch) ws.push_back(kv.second);
        std::sort(ws.begin(), ws.end(), [](const RdbWatchEntry &a, const RdbWatchEntry &b) {
            if (a.address != b.address) return a.address < b.address;
            return a.name < b.name;
        });
        for (const auto &w : ws) arr.push_back(watchToJson(w));
        root["watchlist"] = arr;
    }

    return root;
}

// Deserialize the full RDB from JSON. Returns false on validation failure.
static bool rdbFromJson(const json &root, RdbController::Impl &impl)
{
    // Validate format
    if (!root.contains("format") || root["format"] != "rdb") {
        return false;
    }

    if (root.contains("platform") && root["platform"].is_string()) {
        impl.platform = root["platform"].get<std::string>();
    }

    if (root.contains("version") && root["version"].is_number()) {
        impl.version = root["version"].get<int>();
    }

    // Stage 6.28: accept v1 (no knowledge sections) and v2; reject unknown
    // future versions rather than silently mis-parsing them.
    if (impl.version > 2) {
        return false;
    }

    // ROM identity
    if (root.contains("rom") && root["rom"].is_object()) {
        const auto &rom = root["rom"];
        if (rom.contains("file") && rom["file"].is_string()) {
            impl.romIdentity.file = rom["file"].get<std::string>();
        }
        if (rom.contains("size") && rom["size"].is_number()) {
            impl.romIdentity.size = rom["size"].get<uint64_t>();
        }
        if (rom.contains("sha256") && rom["sha256"].is_string()) {
            impl.romIdentity.sha256 = rom["sha256"].get<std::string>();
        }
    }

    // Objects
    impl.objects.clear();
    if (root.contains("objects") && root["objects"].is_array()) {
        for (const auto &objJson : root["objects"]) {
            RdbObject obj = objectFromJson(objJson);
            impl.objects[obj.address] = obj;
        }
    }

    // -- Stage 6.28 knowledge sections (v1 files simply omit them) ---------
    impl.protocol.clear();
    if (root.contains("protocol") && root["protocol"].is_array()) {
        for (const auto &ej : root["protocol"]) {
            if (!ej.is_object()) continue;
            impl.protocol.push_back(protocolFromJson(ej));
        }
    }
    impl.nextProtocolId = scanMaxProtoId(impl.protocol) + 1;

    impl.ports.clear();
    if (root.contains("ports") && root["ports"].is_array()) {
        for (const auto &pj : root["ports"]) {
            if (!pj.is_object()) continue;
            RdbPortEntry p = portFromJson(pj);
            impl.ports[p.port] = p;
        }
    }

    impl.watch.clear();
    if (root.contains("watchlist") && root["watchlist"].is_array()) {
        for (const auto &wj : root["watchlist"]) {
            if (!wj.is_object()) continue;
            RdbWatchEntry w = watchFromJson(wj);
            if (!w.name.empty()) impl.watch[w.name] = w;
        }
    }

    return true;
}

// ---------------------------------------------------------------------------
// Constructor / Destructor
// ---------------------------------------------------------------------------

RdbController::RdbController()
    : impl_(new Impl())
{
}

RdbController::~RdbController()
{
    delete impl_;
}

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

bool RdbController::load(const std::string &path)
{
    std::ifstream file(path);
    if (!file.is_open()) {
        return false;
    }

    try {
        json root;
        file >> root;

        impl_->objects.clear();
        if (!rdbFromJson(root, *impl_)) {
            impl_->objects.clear();
            impl_->protocol.clear();
            impl_->ports.clear();
            impl_->watch.clear();
            impl_->nextProtocolId = 1;
            return false;
        }

        impl_->path = path;
        impl_->loaded = true;
        impl_->dirty = false;
        impl_->fileExistsOnDisk = true;
        return true;
    } catch (const std::exception &) {
        impl_->objects.clear();
        impl_->protocol.clear();
        impl_->ports.clear();
        impl_->watch.clear();
        impl_->nextProtocolId = 1;
        return false;
    }
}

bool RdbController::reload()
{
    if (impl_->path.empty()) {
        return false;
    }
    return load(impl_->path);
}

bool RdbController::save()
{
    // Don't write if not dirty
    if (!impl_->dirty) {
        return true;
    }

    // Don't create a file if we have no path and no data worth saving
    if (impl_->path.empty()) {
        return true;
    }

    return saveAs(impl_->path);
}

bool RdbController::saveAs(const std::string &path)
{
    if (!impl_->dirty && impl_->fileExistsOnDisk && path == impl_->path) {
        return true;
    }

    // Atomic save: write to .tmp, then rename
    std::string tmpPath = path + ".tmp";

    try {
        json root = rdbToJson(*impl_);

        std::ofstream file(tmpPath);
        if (!file.is_open()) {
            return false;
        }

        file << root.dump(2) << std::endl;
        file.close();

        if (file.fail()) {
            ::remove(tmpPath.c_str());
            return false;
        }

        // Atomic rename. std::filesystem::rename overwrites an existing
        // destination on Windows too; POSIX ::rename does too, but the CRT
        // ::rename on Windows fails when the target already exists — which
        // broke every re-save of the same .rdb path.
        std::error_code rec;
        std::filesystem::rename(tmpPath, path, rec);
        if (rec) {
            std::error_code rim;
            std::filesystem::remove(tmpPath, rim);
            return false;
        }

        impl_->path = path;
        impl_->dirty = false;
        impl_->fileExistsOnDisk = true;
        return true;
    } catch (const std::exception &) {
        ::remove(tmpPath.c_str());
        return false;
    }
}

void RdbController::close()
{
    impl_->path.clear();
    impl_->platform = "vector06c";
    impl_->version = 1;
    impl_->romIdentity = RdbRomIdentity();
    impl_->loaded = false;
    impl_->dirty = false;
    impl_->fileExistsOnDisk = false;
    impl_->objects.clear();
    impl_->protocol.clear();
    impl_->ports.clear();
    impl_->watch.clear();
    impl_->nextProtocolId = 1;
}

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------

bool RdbController::isLoaded() const { return impl_->loaded; }
bool RdbController::isDirty() const { return impl_->dirty; }
std::string RdbController::getPath() const { return impl_->path; }
std::string RdbController::getPlatform() const { return impl_->platform; }
int RdbController::getVersion() const { return impl_->version; }
RdbRomIdentity RdbController::getRomIdentity() const { return impl_->romIdentity; }

bool RdbController::matchesRom(const RdbRomIdentity &rom) const
{
    if (!impl_->loaded) return false;
    if (!impl_->romIdentity.isComplete()) return false;
    if (!rom.isComplete()) return false;
    return impl_->romIdentity == rom;
}

bool RdbController::existsOnDisk() const { return impl_->fileExistsOnDisk; }

// ---------------------------------------------------------------------------
// Objects
// ---------------------------------------------------------------------------

const RdbObject *RdbController::getObject(uint16_t address) const
{
    auto it = impl_->objects.find(address);
    if (it == impl_->objects.end()) return nullptr;
    return &it->second;
}

const RdbObject *RdbController::findObject(const std::string &name) const
{
    for (const auto &kv : impl_->objects) {
        if (kv.second.name == name) {
            return &kv.second;
        }
    }
    return nullptr;
}

std::vector<RdbObject> RdbController::listObjects() const
{
    std::vector<RdbObject> result;
    result.reserve(impl_->objects.size());
    for (const auto &kv : impl_->objects) {
        result.push_back(kv.second);
    }
    return result;
}

size_t RdbController::objectCount() const
{
    return impl_->objects.size();
}

bool RdbController::addObject(const RdbObject &object)
{
    if (impl_->objects.count(object.address)) {
        return false;  // already exists
    }
    RdbObject stored = object;
    normalizeObjectText(stored);              // R1: sanitize whole-object write
    impl_->objects[object.address] = stored;
    impl_->dirty = true;
    return true;
}

bool RdbController::updateObject(const RdbObject &object)
{
    auto it = impl_->objects.find(object.address);
    if (it == impl_->objects.end()) {
        return false;
    }

    RdbObject stored = object;
    normalizeObjectText(stored);              // R1: sanitize whole-object write

    // Only set dirty if data actually changed
    if (it->second != stored) {
        it->second = stored;
        impl_->dirty = true;
    }
    return true;
}

bool RdbController::removeObject(uint16_t address)
{
    auto it = impl_->objects.find(address);
    if (it == impl_->objects.end()) {
        return false;
    }
    impl_->objects.erase(it);
    impl_->dirty = true;
    return true;
}

// ---------------------------------------------------------------------------
// Comments
// ---------------------------------------------------------------------------

bool RdbController::setComment(uint16_t address, const std::string &comment)
{
    auto it = impl_->objects.find(address);
    if (it == impl_->objects.end()) {
        return false;
    }

    const std::string normalized = normalizeRdbText(comment);   // R1
    if (it->second.comment != normalized) {
        it->second.comment = normalized;
        impl_->dirty = true;
    }
    return true;
}

std::string RdbController::getComment(uint16_t address) const
{
    auto it = impl_->objects.find(address);
    if (it == impl_->objects.end()) return "";
    return normalizeRdbText(it->second.comment);   // R2: belt-and-braces on read
}

// ---------------------------------------------------------------------------
// Properties
// ---------------------------------------------------------------------------

bool RdbController::setProperty(uint16_t address, const std::string &name,
                                const RdbPropertyValue &value)
{
    auto it = impl_->objects.find(address);
    if (it == impl_->objects.end()) {
        return false;
    }

    RdbPropertyValue storedValue = value;                       // R1
    if (storedValue.type == RdbPropertyValue::Type::String)
        storedValue.stringValue = normalizeRdbText(storedValue.stringValue);

    auto propIt = it->second.properties.find(name);
    if (propIt != it->second.properties.end() && propIt->second == storedValue) {
        return true;  // no change
    }

    it->second.properties[name] = storedValue;
    impl_->dirty = true;
    return true;
}

const RdbPropertyValue *RdbController::getProperty(uint16_t address,
                                                    const std::string &name) const
{
    auto objIt = impl_->objects.find(address);
    if (objIt == impl_->objects.end()) return nullptr;

    auto propIt = objIt->second.properties.find(name);
    if (propIt == objIt->second.properties.end()) return nullptr;
    return &propIt->second;
}

bool RdbController::removeProperty(uint16_t address, const std::string &name)
{
    auto objIt = impl_->objects.find(address);
    if (objIt == impl_->objects.end()) {
        return false;
    }

    auto propIt = objIt->second.properties.find(name);
    if (propIt == objIt->second.properties.end()) {
        return false;
    }

    objIt->second.properties.erase(propIt);
    impl_->dirty = true;
    return true;
}

// ---------------------------------------------------------------------------
// Links
// ---------------------------------------------------------------------------

std::vector<uint16_t> RdbController::getLinks(uint16_t address) const
{
    auto it = impl_->objects.find(address);
    if (it == impl_->objects.end()) return {};
    return it->second.links;
}

bool RdbController::addLink(uint16_t address, uint16_t targetAddress)
{
    auto it = impl_->objects.find(address);
    if (it == impl_->objects.end()) {
        return false;
    }

    // Check for duplicate
    auto &links = it->second.links;
    if (std::find(links.begin(), links.end(), targetAddress) != links.end()) {
        return true;  // already exists, not a change
    }

    links.push_back(targetAddress);
    impl_->dirty = true;
    return true;
}

bool RdbController::removeLink(uint16_t address, uint16_t targetAddress)
{
    auto it = impl_->objects.find(address);
    if (it == impl_->objects.end()) {
        return false;
    }

    auto &links = it->second.links;
    auto linkIt = std::find(links.begin(), links.end(), targetAddress);
    if (linkIt == links.end()) {
        return false;
    }

    links.erase(linkIt);
    impl_->dirty = true;
    return true;
}

bool RdbController::setLinks(uint16_t address, const std::vector<uint16_t> &links)
{
    auto it = impl_->objects.find(address);
    if (it == impl_->objects.end()) {
        return false;
    }

    // Deduplicate input
    std::vector<uint16_t> deduped = links;
    std::sort(deduped.begin(), deduped.end());
    deduped.erase(std::unique(deduped.begin(), deduped.end()), deduped.end());

    // Compare sorted
    std::vector<uint16_t> current = it->second.links;
    std::sort(current.begin(), current.end());

    if (current == deduped) {
        return true;  // no change
    }

    it->second.links = deduped;
    impl_->dirty = true;
    return true;
}

bool RdbController::hasLink(uint16_t address, uint16_t targetAddress) const
{
    auto it = impl_->objects.find(address);
    if (it == impl_->objects.end()) return false;

    const auto &links = it->second.links;
    return std::find(links.begin(), links.end(), targetAddress) != links.end();
}

// ---------------------------------------------------------------------------
// ROM identity
// ---------------------------------------------------------------------------

void RdbController::setRomIdentity(const RdbRomIdentity &identity)
{
    impl_->romIdentity = identity;
}

// ---------------------------------------------------------------------------
// Initialization
// ---------------------------------------------------------------------------

void RdbController::initialize(const std::string &platform,
                                const RdbRomIdentity &rom,
                                const std::string &path)
{
    close();
    impl_->platform = platform;
    impl_->romIdentity = rom;
    impl_->path = path;
    impl_->loaded = true;
    // dirty remains false — in-memory only until modified
}

// ---------------------------------------------------------------------------
// Protocol journal (Stage 6.28)
// ---------------------------------------------------------------------------

// Evidence rule (§4.4): a VERIFIED*/CONFLICT/REFUTED status must carry a valid
// method and a non-empty evidence string. Returns MissingEvidence otherwise.
static RdbOpResult validateProtocolEvidence(const RdbProtocolEntry &e)
{
    if (e.thesis.empty()) return RdbOpResult::InvalidThesis;
    if (protocolStatusRequiresEvidence(e.status)) {
        if (!e.hasMethod || e.evidence.empty())
            return RdbOpResult::MissingEvidence;
    }
    return RdbOpResult::None;
}

RdbOpResult RdbController::addProtocolEntry(RdbProtocolEntry &entry)
{
    entry.thesis   = normalizeRdbText(entry.thesis);
    entry.evidence = normalizeRdbText(entry.evidence);

    RdbOpResult v = validateProtocolEvidence(entry);
    if (v != RdbOpResult::None) return v;

    entry.id        = protoId(impl_->nextProtocolId++);
    entry.timestamp = nowIso8601();
    entry.supersededBy.clear();   // a fresh entry is never already superseded

    impl_->protocol.push_back(entry);
    impl_->dirty = true;
    return RdbOpResult::None;
}

RdbOpResult RdbController::supersedeProtocolEntry(const std::string &supersededId,
                                                  RdbProtocolEntry &newEntry,
                                                  std::string &outNewId)
{
    // Locate the target by index (a pointer would be invalidated by the
    // push_back inside addProtocolEntry below).
    size_t targetIdx = impl_->protocol.size();
    for (size_t i = 0; i < impl_->protocol.size(); ++i) {
        if (impl_->protocol[i].id == supersededId) { targetIdx = i; break; }
    }
    if (targetIdx == impl_->protocol.size()) return RdbOpResult::NotFound;

    RdbOpResult r = addProtocolEntry(newEntry);
    if (r != RdbOpResult::None) return r;

    // addProtocolEntry assigned id/timestamp; wire the back-references.
    newEntry.supersedes = supersededId;
    impl_->protocol.back().supersedes = supersededId;
    impl_->protocol[targetIdx].supersededBy = newEntry.id;

    outNewId = newEntry.id;
    return RdbOpResult::None;
}

RdbOpResult RdbController::updateProtocolStatus(const std::string &id,
                                                ProtocolStatus status,
                                                bool hasMethod, ProtocolMethod method,
                                                const std::string &evidence)
{
    for (auto &e : impl_->protocol) {
        if (e.id != id) continue;

        RdbProtocolEntry candidate = e;
        candidate.status    = status;
        candidate.hasMethod = hasMethod;
        candidate.method    = method;
        candidate.evidence  = normalizeRdbText(evidence);

        RdbOpResult v = validateProtocolEvidence(candidate);
        if (v != RdbOpResult::None) return v;

        if (candidate.status    == e.status    &&
            candidate.hasMethod == e.hasMethod &&
            candidate.method    == e.method    &&
            candidate.evidence  == e.evidence) {
            return RdbOpResult::None;   // no change
        }

        e.status    = candidate.status;
        e.hasMethod = candidate.hasMethod;
        e.method    = candidate.method;
        e.evidence  = candidate.evidence;
        impl_->dirty = true;
        return RdbOpResult::None;
    }
    return RdbOpResult::NotFound;
}

RdbOpResult RdbController::removeProtocolEntry(const std::string &id)
{
    auto it = std::find_if(impl_->protocol.begin(), impl_->protocol.end(),
                           [&](const RdbProtocolEntry &e) { return e.id == id; });
    if (it == impl_->protocol.end()) return RdbOpResult::NotFound;

    // Refuse to delete an entry another active entry supersedes.
    for (const auto &e : impl_->protocol) {
        if (e.supersedes == id) return RdbOpResult::Referenced;
    }

    impl_->protocol.erase(it);
    impl_->dirty = true;
    return RdbOpResult::None;
}

const RdbProtocolEntry *RdbController::findProtocolEntry(const std::string &id) const
{
    for (const auto &e : impl_->protocol) {
        if (e.id == id) return &e;
    }
    return nullptr;
}

std::vector<RdbProtocolEntry> RdbController::getProtocol(const ProtocolFilter &filter) const
{
    const std::string needle = rdb_detail::toLower(filter.substring);

    std::vector<RdbProtocolEntry> result;
    for (const auto &e : impl_->protocol) {
        if (filter.activeOnly && e.isSuperseded()) continue;
        if (filter.hasStatus && e.status != filter.status) continue;
        if (filter.hasScope  && e.scope  != filter.scope)  continue;
        if (filter.hasAddress && (!e.hasAddress || e.address != filter.address)) continue;
        if (!needle.empty()) {
            if (rdb_detail::toLower(e.thesis).find(needle) == std::string::npos)
                continue;
        }
        result.push_back(e);
        if (filter.maxEntries > 0 && result.size() >= filter.maxEntries) break;
    }
    return result;
}

size_t RdbController::protocolCount() const { return impl_->protocol.size(); }

// ---------------------------------------------------------------------------
// Ports table (Stage 6.28)
// ---------------------------------------------------------------------------

RdbOpResult RdbController::upsertPort(const RdbPortEntry &port)
{
    RdbPortEntry stored = port;
    stored.name    = normalizeRdbText(port.name);
    stored.comment = normalizeRdbText(port.comment);
    for (auto &b : stored.bits) b.meaning = normalizeRdbText(b.meaning);

    auto it = impl_->ports.find(stored.port);
    if (it != impl_->ports.end()) {
        // Compare; only mark dirty when something actually changed.
        json a = portToJson(it->second);
        json b = portToJson(stored);
        if (a == b) return RdbOpResult::None;
        it->second = stored;
    } else {
        impl_->ports[stored.port] = stored;
    }
    impl_->dirty = true;
    return RdbOpResult::None;
}

const RdbPortEntry *RdbController::getPort(uint8_t port) const
{
    auto it = impl_->ports.find(port);
    if (it == impl_->ports.end()) return nullptr;
    return &it->second;
}

std::vector<RdbPortEntry> RdbController::listPorts() const
{
    std::vector<RdbPortEntry> result;   // std::map iterates ascending by key
    result.reserve(impl_->ports.size());
    for (const auto &kv : impl_->ports) result.push_back(kv.second);
    return result;
}

size_t RdbController::portCount() const { return impl_->ports.size(); }

// ---------------------------------------------------------------------------
// Watchlist (Stage 6.28)
// ---------------------------------------------------------------------------

RdbOpResult RdbController::addWatchEntry(const RdbWatchEntry &watch)
{
    RdbWatchEntry stored = watch;
    stored.name = normalizeRdbText(watch.name);
    if (stored.name.empty()) return RdbOpResult::InvalidThesis;  // name required

    auto it = impl_->watch.find(stored.name);
    if (it != impl_->watch.end()) {
        // Idempotent replace: same content -> not dirty.
        if (it->second.address == stored.address &&
            it->second.length  == stored.length  &&
            it->second.format  == stored.format  &&
            it->second.hasObjectAddress == stored.hasObjectAddress &&
            it->second.objectAddress    == stored.objectAddress) {
            return RdbOpResult::None;
        }
        it->second = stored;   // update existing name
        impl_->dirty = true;
        return RdbOpResult::None;
    }

    impl_->watch[stored.name] = stored;
    impl_->dirty = true;
    return RdbOpResult::None;
}

RdbOpResult RdbController::removeWatchEntry(const std::string &name)
{
    auto it = impl_->watch.find(name);
    if (it == impl_->watch.end()) return RdbOpResult::NotFound;
    impl_->watch.erase(it);
    impl_->dirty = true;
    return RdbOpResult::None;
}

std::vector<RdbWatchEntry> RdbController::listWatch() const
{
    std::vector<RdbWatchEntry> result;
    result.reserve(impl_->watch.size());
    for (const auto &kv : impl_->watch) result.push_back(kv.second);
    std::sort(result.begin(), result.end(),
              [](const RdbWatchEntry &a, const RdbWatchEntry &b) {
        if (a.address != b.address) return a.address < b.address;
        return a.name < b.name;
    });
    return result;
}

size_t RdbController::watchCount() const { return impl_->watch.size(); }
