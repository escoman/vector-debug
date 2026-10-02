#include "rdb_controller.h"
#include "nlohmann/json.hpp"

#include <algorithm>
#include <cstdio>
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

// Serialize the full RDB to JSON
static json rdbToJson(const RdbController::Impl &impl)
{
    json root;
    root["format"] = "rdb";
    root["platform"] = impl.platform;
    root["version"] = impl.version;

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
            return false;
        }

        impl_->path = path;
        impl_->loaded = true;
        impl_->dirty = false;
        impl_->fileExistsOnDisk = true;
        return true;
    } catch (const std::exception &) {
        impl_->objects.clear();
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

        // Atomic rename
        if (::rename(tmpPath.c_str(), path.c_str()) != 0) {
            ::remove(tmpPath.c_str());
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
