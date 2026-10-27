#include "rdb_controller.h"

#include <cassert>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <string>

#include "test_tmp_dir.h"

// ---------------------------------------------------------------------------
// Test helpers
// ---------------------------------------------------------------------------

static int testsPassed = 0;
static int testsFailed = 0;

#define TEST(name) \
    static void test_##name(); \
    struct Register_##name { \
        Register_##name() { \
            std::cout << "  " #name "..."; \
            try { \
                test_##name(); \
                std::cout << " PASS" << std::endl; \
                testsPassed++; \
            } catch (const std::exception &e) { \
                std::cout << " FAIL: " << e.what() << std::endl; \
                testsFailed++; \
            } catch (...) { \
                std::cout << " FAIL (unknown exception)" << std::endl; \
                testsFailed++; \
            } \
        } \
    } register_##name; \
    static void test_##name()

#define ASSERT(cond) \
    do { if (!(cond)) throw std::runtime_error("Assertion failed: " #cond); } while(0)

#define ASSERT_EQ(a, b) \
    do { if ((a) != (b)) throw std::runtime_error("Assertion failed: " #a " != " #b); } while(0)

// Portable temp paths (Linux: /tmp, Windows: %TEMP%). The const char* aliases
// keep existing ::remove()/std::string() call sites unchanged.
static const std::string TEST_RDB_PATH_STR = testTmpPath("test_rdb_controller.rdb");
static const std::string TEST_RDB_TMP_STR  = testTmpPath("test_rdb_controller_tmp.rdb");
static const char *TEST_RDB_PATH = TEST_RDB_PATH_STR.c_str();
static const char *TEST_RDB_TMP  = TEST_RDB_TMP_STR.c_str();

static void cleanupTestFiles()
{
    ::remove(TEST_RDB_PATH);
    ::remove((std::string(TEST_RDB_PATH) + ".tmp").c_str());
    ::remove(TEST_RDB_TMP);
}

static RdbObject makeTestObject(uint16_t addr, const std::string &name,
                                 RdbObjectType type = RdbObjectType::Function)
{
    RdbObject obj;
    obj.address = addr;
    obj.type = type;
    obj.name = name;
    return obj;
}

// ---------------------------------------------------------------------------
// Basic lifecycle tests
// ---------------------------------------------------------------------------

TEST(create_empty)
{
    RdbController ctrl;
    ASSERT(!ctrl.isLoaded());
    ASSERT(!ctrl.isDirty());
    ASSERT(ctrl.getPath().empty());
    ASSERT_EQ(ctrl.objectCount(), (size_t)0);
    ASSERT_EQ(ctrl.getPlatform(), std::string("vector06c"));
    ASSERT_EQ(ctrl.getVersion(), 1);
}

TEST(initialize)
{
    RdbController ctrl;
    RdbRomIdentity rom;
    rom.file = "test.rom";
    rom.size = 32768;
    rom.sha256 = "abc123";
    ctrl.initialize("vector06c", rom);

    ASSERT(ctrl.isLoaded());
    ASSERT(!ctrl.isDirty());
    ASSERT_EQ(ctrl.getPlatform(), std::string("vector06c"));
    ASSERT(ctrl.getRomIdentity() == rom);
    ASSERT(!ctrl.existsOnDisk());
}

TEST(close_resets_state)
{
    RdbController ctrl;
    ctrl.addObject(makeTestObject(0x1000, "test"));
    ASSERT(ctrl.isDirty());

    ctrl.close();
    ASSERT(!ctrl.isLoaded());
    ASSERT(!ctrl.isDirty());
    ASSERT_EQ(ctrl.objectCount(), (size_t)0);
}

// ---------------------------------------------------------------------------
// Object CRUD tests
// ---------------------------------------------------------------------------

TEST(add_object)
{
    RdbController ctrl;
    ctrl.initialize("vector06c", RdbRomIdentity());

    RdbObject obj = makeTestObject(0x1234, "draw_sprite");
    obj.size = 47;
    obj.hasSize = true;
    obj.comment = "Draws a sprite";

    ASSERT(ctrl.addObject(obj));
    ASSERT(ctrl.isDirty());
    ASSERT_EQ(ctrl.objectCount(), (size_t)1);

    const RdbObject *got = ctrl.getObject(0x1234);
    ASSERT(got != nullptr);
    ASSERT_EQ(got->address, (uint16_t)0x1234);
    ASSERT_EQ(got->name, std::string("draw_sprite"));
    ASSERT_EQ(got->type, RdbObjectType::Function);
    ASSERT_EQ(got->size, (uint32_t)47);
    ASSERT(got->hasSize);
    ASSERT_EQ(got->comment, std::string("Draws a sprite"));
}

TEST(add_duplicate_fails)
{
    RdbController ctrl;
    ctrl.initialize("vector06c", RdbRomIdentity());

    ASSERT(ctrl.addObject(makeTestObject(0x1000, "foo")));
    ASSERT(!ctrl.addObject(makeTestObject(0x1000, "bar")));
    ASSERT_EQ(ctrl.objectCount(), (size_t)1);
}

TEST(update_object)
{
    RdbController ctrl;
    ctrl.initialize("vector06c", RdbRomIdentity());

    ctrl.addObject(makeTestObject(0x1000, "foo"));
    ctrl.save();  // reset dirty by saving (will fail silently, no path)

    // Actually test update changes
    RdbObject updated = makeTestObject(0x1000, "bar");
    ASSERT(ctrl.updateObject(updated));
    ASSERT_EQ(ctrl.getObject(0x1000)->name, std::string("bar"));
}

TEST(update_nonexistent_fails)
{
    RdbController ctrl;
    ctrl.initialize("vector06c", RdbRomIdentity());

    ASSERT(!ctrl.updateObject(makeTestObject(0x9999, "nope")));
}

TEST(update_same_data_no_dirty)
{
    RdbController ctrl;
    ctrl.initialize("vector06c", RdbRomIdentity());

    RdbObject obj = makeTestObject(0x1000, "foo");
    ctrl.addObject(obj);
    // Force dirty to false via reload-like mechanism
    // We can't easily do this without save/load, so test via saveAs
    ctrl.saveAs(TEST_RDB_PATH);
    ASSERT(!ctrl.isDirty());

    // Update with identical data
    ASSERT(ctrl.updateObject(obj));
    ASSERT(!ctrl.isDirty());  // no change → no dirty
}

TEST(remove_object)
{
    RdbController ctrl;
    ctrl.initialize("vector06c", RdbRomIdentity());

    ctrl.addObject(makeTestObject(0x1000, "foo"));
    ASSERT(ctrl.removeObject(0x1000));
    ASSERT(ctrl.isDirty());
    ASSERT_EQ(ctrl.objectCount(), (size_t)0);
    ASSERT(ctrl.getObject(0x1000) == nullptr);
}

TEST(remove_nonexistent_fails)
{
    RdbController ctrl;
    ctrl.initialize("vector06c", RdbRomIdentity());

    ASSERT(!ctrl.removeObject(0x9999));
}

TEST(find_by_name)
{
    RdbController ctrl;
    ctrl.initialize("vector06c", RdbRomIdentity());

    ctrl.addObject(makeTestObject(0x1000, "alpha"));
    ctrl.addObject(makeTestObject(0x2000, "beta"));

    const RdbObject *found = ctrl.findObject("beta");
    ASSERT(found != nullptr);
    ASSERT_EQ(found->address, (uint16_t)0x2000);

    ASSERT(ctrl.findObject("gamma") == nullptr);
}

TEST(list_objects_sorted)
{
    RdbController ctrl;
    ctrl.initialize("vector06c", RdbRomIdentity());

    ctrl.addObject(makeTestObject(0x3000, "c"));
    ctrl.addObject(makeTestObject(0x1000, "a"));
    ctrl.addObject(makeTestObject(0x2000, "b"));

    auto list = ctrl.listObjects();
    ASSERT_EQ(list.size(), (size_t)3);
    ASSERT_EQ(list[0].address, (uint16_t)0x1000);
    ASSERT_EQ(list[1].address, (uint16_t)0x2000);
    ASSERT_EQ(list[2].address, (uint16_t)0x3000);
}

// ---------------------------------------------------------------------------
// Comment tests
// ---------------------------------------------------------------------------

TEST(set_get_comment)
{
    RdbController ctrl;
    ctrl.initialize("vector06c", RdbRomIdentity());

    ctrl.addObject(makeTestObject(0x1000, "foo"));
    ASSERT(ctrl.setComment(0x1000, "Hello world"));
    ASSERT(ctrl.isDirty());
    ASSERT_EQ(ctrl.getComment(0x1000), std::string("Hello world"));
}

TEST(comment_no_change_no_dirty)
{
    RdbController ctrl;
    ctrl.initialize("vector06c", RdbRomIdentity());

    ctrl.addObject(makeTestObject(0x1000, "foo"));
    ctrl.setComment(0x1000, "test");
    ctrl.saveAs(TEST_RDB_PATH);
    ASSERT(!ctrl.isDirty());

    // Set same comment
    ASSERT(ctrl.setComment(0x1000, "test"));
    ASSERT(!ctrl.isDirty());
}

TEST(comment_nonexistent_fails)
{
    RdbController ctrl;
    ctrl.initialize("vector06c", RdbRomIdentity());

    ASSERT(!ctrl.setComment(0x9999, "nope"));
    ASSERT_EQ(ctrl.getComment(0x9999), std::string(""));
}

// ---------------------------------------------------------------------------
// Property tests
// ---------------------------------------------------------------------------

TEST(set_get_property)
{
    RdbController ctrl;
    ctrl.initialize("vector06c", RdbRomIdentity());

    ctrl.addObject(makeTestObject(0x1000, "foo"));
    ASSERT(ctrl.setProperty(0x1000, "color", RdbPropertyValue::fromString("red")));
    ASSERT(ctrl.isDirty());

    const RdbPropertyValue *val = ctrl.getProperty(0x1000, "color");
    ASSERT(val != nullptr);
    ASSERT_EQ(val->stringValue, std::string("red"));
}

TEST(remove_property)
{
    RdbController ctrl;
    ctrl.initialize("vector06c", RdbRomIdentity());

    ctrl.addObject(makeTestObject(0x1000, "foo"));
    ctrl.setProperty(0x1000, "key", RdbPropertyValue::fromInt(42));
    ASSERT(ctrl.removeProperty(0x1000, "key"));
    ASSERT(ctrl.getProperty(0x1000, "key") == nullptr);
}

TEST(property_nonexistent_object_fails)
{
    RdbController ctrl;
    ctrl.initialize("vector06c", RdbRomIdentity());

    ASSERT(!ctrl.setProperty(0x9999, "key", RdbPropertyValue::fromInt(1)));
    ASSERT(ctrl.getProperty(0x9999, "key") == nullptr);
}

// ---------------------------------------------------------------------------
// Line-break normalization tests (RDB comment/property round-trip contract)
// ---------------------------------------------------------------------------

TEST(normalize_text_idempotent_and_basic)
{
    // §6.1 / §6.3
    ASSERT_EQ(normalizeRdbText("a\nb"), std::string("a b"));
    ASSERT_EQ(normalizeRdbText(std::string("\xd0\xb5\xd0\xb7 \xd0\xbf\xd0\xb5\xd1\x80\xd0\xb5\xd0\xbd\xd0\xbe\xd1\x81\xd0\xb0")),
              std::string("\xd0\xb5\xd0\xb7 \xd0\xbf\xd0\xb5\xd1\x80\xd0\xb5\xd0\xbd\xd0\xbe\xd1\x81\xd0\xb0"));
    std::string once = normalizeRdbText("a\n\n  b\r\n   c");
    ASSERT_EQ(normalizeRdbText(once), once);          // idempotent
    ASSERT(once.find('\n') == std::string::npos);
    ASSERT(once.find('\r') == std::string::npos);
    ASSERT_EQ(normalizeRdbText(std::string()), std::string());
}

TEST(normalize_text_crlf_and_indent)
{
    // §6.2: newline + hugging whitespace collapses to a single space
    ASSERT_EQ(normalizeRdbText("a\r\n   b"), std::string("a b"));
    ASSERT_EQ(normalizeRdbText("a \t\n\t b"), std::string("a b"));
    ASSERT_EQ(normalizeRdbText("line1\nline2\nline3"), std::string("line1 line2 line3"));
}

TEST(normalize_text_unicode_breaks)
{
    // §6.4: U+2028, U+2029 and U+0085 are treated as line breaks
    std::string in = std::string("a") + "\xE2\x80\xA8" + "b" + "\xE2\x80\xA9" + "c";
    ASSERT_EQ(normalizeRdbText(in), std::string("a b c"));
    std::string nel = std::string("x") + "\xC2\x85" + "y";
    ASSERT_EQ(normalizeRdbText(nel), std::string("x y"));
}

TEST(normalize_text_preserves_meaningful_chars)
{
    // Cyrillic, ';', U+2192 arrow, U+00D7 times, U+2116 numero all survive verbatim
    std::string in = std::string("\xD0\x9F\xD1\x80\xD0\xB8\xD0\xBC\xD0\xB5\xD1\x80; \xE2\x86\x92 \xC3\x97 \xE2\x84\x96 42");
    ASSERT_EQ(normalizeRdbText(in), in);
}

TEST(set_rdb_comment_normalizes)
{
    // §6.1 end-to-end through the controller store
    RdbController ctrl;
    ctrl.initialize("vector06c", RdbRomIdentity());
    ctrl.addObject(makeTestObject(0x1000, "foo"));
    ctrl.setComment(0x1000, "a\nb");
    ASSERT_EQ(ctrl.getComment(0x1000), std::string("a b"));
}

TEST(set_rdb_property_normalizes)
{
    // §6.5: string property values are normalized too
    RdbController ctrl;
    ctrl.initialize("vector06c", RdbRomIdentity());
    ctrl.addObject(makeTestObject(0x1000, "foo"));
    ctrl.setProperty(0x1000, "note", RdbPropertyValue::fromString("v1\nv2"));
    const RdbPropertyValue *v = ctrl.getProperty(0x1000, "note");
    ASSERT(v != nullptr);
    ASSERT_EQ(v->stringValue, std::string("v1 v2"));
}

TEST(add_update_object_normalizes)
{
    RdbController ctrl;
    ctrl.initialize("vector06c", RdbRomIdentity());

    RdbObject obj = makeTestObject(0x1000, "foo");
    obj.comment = "x\ny";
    obj.properties["p"] = RdbPropertyValue::fromString("q\r\nz");
    ctrl.addObject(obj);
    ASSERT_EQ(ctrl.getObject(0x1000)->comment, std::string("x y"));
    ASSERT_EQ(ctrl.getObject(0x1000)->properties.at("p").stringValue, std::string("q z"));

    RdbObject upd = *ctrl.getObject(0x1000);
    upd.comment = "1\n2\n3";
    ctrl.updateObject(upd);
    ASSERT_EQ(ctrl.getComment(0x1000), std::string("1 2 3"));
}

TEST(legacy_rdb_load_and_save_have_no_breaks)
{
    // §6.6: a legacy .rdb with a literal \n inside a comment/property must load
    // normalized and must never write that break back to disk.
    cleanupTestFiles();
    {
        std::ofstream f(TEST_RDB_PATH);
        f << "{\n"
          << "  \"format\": \"rdb\",\n"
          << "  \"platform\": \"vector06c\",\n"
          << "  \"version\": 1,\n"
          << "  \"objects\": [\n"
          << "    { \"address\": \"0x0100\", \"type\": \"function\", \"name\": \"foo\",\n"
          << "      \"comment\": \"line1\\nline2\",\n"
          << "      \"properties\": { \"note\": \"p1\\np2\" } }\n"
          << "  ]\n"
          << "}\n";
    }

    RdbController ctrl;
    ASSERT(ctrl.load(TEST_RDB_PATH));
    ASSERT_EQ(ctrl.getComment(0x0100), std::string("line1 line2"));
    const RdbObject *o = ctrl.getObject(0x0100);
    ASSERT(o != nullptr);
    ASSERT(o->comment.find('\n') == std::string::npos);
    ASSERT(o->properties.at("note").stringValue.find('\n') == std::string::npos);

    // Force a re-save (mark dirty) and inspect the raw bytes on disk.
    ctrl.setComment(0x0100, "trigger");
    ASSERT(ctrl.saveAs(TEST_RDB_PATH));

    std::ifstream rf(TEST_RDB_PATH);
    std::string disk((std::istreambuf_iterator<char>(rf)), std::istreambuf_iterator<char>());
    ASSERT(disk.find("line1\\nline2") == std::string::npos);
    ASSERT(disk.find("p1\\np2") == std::string::npos);

    cleanupTestFiles();
}


// ---------------------------------------------------------------------------
// Link tests
// ---------------------------------------------------------------------------

TEST(add_link)
{
    RdbController ctrl;
    ctrl.initialize("vector06c", RdbRomIdentity());

    ctrl.addObject(makeTestObject(0x1000, "A"));
    ctrl.addObject(makeTestObject(0x2000, "B"));

    ASSERT(ctrl.addLink(0x1000, 0x2000));
    ASSERT(ctrl.isDirty());

    auto links = ctrl.getLinks(0x1000);
    ASSERT_EQ(links.size(), (size_t)1);
    ASSERT_EQ(links[0], (uint16_t)0x2000);
}

TEST(duplicate_link_no_dirty)
{
    RdbController ctrl;
    ctrl.initialize("vector06c", RdbRomIdentity());

    ctrl.addObject(makeTestObject(0x1000, "A"));
    ctrl.addLink(0x1000, 0x2000);
    ctrl.saveAs(TEST_RDB_PATH);
    ASSERT(!ctrl.isDirty());

    // Adding same link again
    ASSERT(ctrl.addLink(0x1000, 0x2000));
    ASSERT(!ctrl.isDirty());
}

TEST(remove_link)
{
    RdbController ctrl;
    ctrl.initialize("vector06c", RdbRomIdentity());

    ctrl.addObject(makeTestObject(0x1000, "A"));
    ctrl.addLink(0x1000, 0x2000);
    ASSERT(ctrl.removeLink(0x1000, 0x2000));
    ASSERT(ctrl.isDirty());
    ASSERT_EQ(ctrl.getLinks(0x1000).size(), (size_t)0);
}

TEST(remove_nonexistent_link_fails)
{
    RdbController ctrl;
    ctrl.initialize("vector06c", RdbRomIdentity());

    ctrl.addObject(makeTestObject(0x1000, "A"));
    ASSERT(!ctrl.removeLink(0x1000, 0x9999));
}

TEST(set_links)
{
    RdbController ctrl;
    ctrl.initialize("vector06c", RdbRomIdentity());

    ctrl.addObject(makeTestObject(0x1000, "A"));
    ASSERT(ctrl.setLinks(0x1000, {0x2000, 0x3000, 0x4000}));

    auto links = ctrl.getLinks(0x1000);
    ASSERT_EQ(links.size(), (size_t)3);
}

TEST(set_links_deduplicates)
{
    RdbController ctrl;
    ctrl.initialize("vector06c", RdbRomIdentity());

    ctrl.addObject(makeTestObject(0x1000, "A"));
    ASSERT(ctrl.setLinks(0x1000, {0x2000, 0x2000, 0x3000}));

    auto links = ctrl.getLinks(0x1000);
    ASSERT_EQ(links.size(), (size_t)2);
}

TEST(has_link)
{
    RdbController ctrl;
    ctrl.initialize("vector06c", RdbRomIdentity());

    ctrl.addObject(makeTestObject(0x1000, "A"));
    ctrl.addLink(0x1000, 0x2000);

    ASSERT(ctrl.hasLink(0x1000, 0x2000));
    ASSERT(!ctrl.hasLink(0x1000, 0x3000));
    ASSERT(!ctrl.hasLink(0x9999, 0x2000));
}

TEST(links_are_directed)
{
    RdbController ctrl;
    ctrl.initialize("vector06c", RdbRomIdentity());

    ctrl.addObject(makeTestObject(0x1000, "A"));
    ctrl.addObject(makeTestObject(0x2000, "B"));

    ctrl.addLink(0x1000, 0x2000);

    // A → B does NOT imply B → A
    ASSERT(ctrl.hasLink(0x1000, 0x2000));
    ASSERT(!ctrl.hasLink(0x2000, 0x1000));
}

TEST(unresolved_link_allowed)
{
    RdbController ctrl;
    ctrl.initialize("vector06c", RdbRomIdentity());

    ctrl.addObject(makeTestObject(0x1000, "A"));
    // Link to 0x5000 which doesn't exist yet
    ASSERT(ctrl.addLink(0x1000, 0x5000));
    ASSERT(ctrl.hasLink(0x1000, 0x5000));
}

TEST(link_nonexistent_source_fails)
{
    RdbController ctrl;
    ctrl.initialize("vector06c", RdbRomIdentity());

    ASSERT(!ctrl.addLink(0x9999, 0x1000));
    ASSERT(ctrl.getLinks(0x9999).empty());
}

// ---------------------------------------------------------------------------
// Dirty state tests
// ---------------------------------------------------------------------------

TEST(dirty_after_add)
{
    RdbController ctrl;
    ctrl.initialize("vector06c", RdbRomIdentity());

    ASSERT(!ctrl.isDirty());
    ctrl.addObject(makeTestObject(0x1000, "foo"));
    ASSERT(ctrl.isDirty());
}

TEST(dirty_after_remove)
{
    RdbController ctrl;
    ctrl.initialize("vector06c", RdbRomIdentity());

    ctrl.addObject(makeTestObject(0x1000, "foo"));
    ctrl.saveAs(TEST_RDB_PATH);
    ASSERT(!ctrl.isDirty());

    ctrl.removeObject(0x1000);
    ASSERT(ctrl.isDirty());
}

TEST(read_operations_no_dirty)
{
    RdbController ctrl;
    ctrl.initialize("vector06c", RdbRomIdentity());
    ctrl.addObject(makeTestObject(0x1000, "foo"));
    ctrl.saveAs(TEST_RDB_PATH);
    ASSERT(!ctrl.isDirty());

    // All these should NOT set dirty
    ctrl.getObject(0x1000);
    ctrl.findObject("foo");
    ctrl.listObjects();
    ctrl.getComment(0x1000);
    ctrl.getProperty(0x1000, "nonexistent");
    ctrl.getRomIdentity();
    ctrl.getPlatform();
    ctrl.getVersion();
    ctrl.isDirty();
    ctrl.getLinks(0x1000);
    ctrl.hasLink(0x1000, 0x2000);
    ctrl.matchesRom(RdbRomIdentity());
    ctrl.existsOnDisk();
    ctrl.objectCount();

    ASSERT(!ctrl.isDirty());
}

// ---------------------------------------------------------------------------
// Save / Load persistence tests
// ---------------------------------------------------------------------------

TEST(save_and_reload)
{
    cleanupTestFiles();

    RdbRomIdentity rom;
    rom.file = "test.rom";
    rom.size = 16384;
    rom.sha256 = "deadbeef";

    {
        RdbController ctrl;
        ctrl.initialize("vector06c", rom);

        RdbObject obj;
        obj.address = 0x0100;
        obj.type = RdbObjectType::Function;
        obj.name = "start";
        obj.size = 42;
        obj.hasSize = true;
        obj.comment = "Entry point";
        obj.properties["calling_convention"] = RdbPropertyValue::fromString("custom");
        obj.links.push_back(0x0200);
        obj.links.push_back(0x0300);
        ctrl.addObject(obj);

        ASSERT(ctrl.saveAs(TEST_RDB_PATH));
        ASSERT(!ctrl.isDirty());
        ASSERT(ctrl.existsOnDisk());
    }

    // Reload
    {
        RdbController ctrl;
        ASSERT(ctrl.load(TEST_RDB_PATH));
        ASSERT(ctrl.isLoaded());
        ASSERT(!ctrl.isDirty());
        ASSERT(ctrl.existsOnDisk());

        ASSERT_EQ(ctrl.getPlatform(), std::string("vector06c"));
        ASSERT(ctrl.getRomIdentity() == rom);
        ASSERT_EQ(ctrl.objectCount(), (size_t)1);

        const RdbObject *obj = ctrl.getObject(0x0100);
        ASSERT(obj != nullptr);
        ASSERT_EQ(obj->name, std::string("start"));
        ASSERT_EQ(obj->type, RdbObjectType::Function);
        ASSERT_EQ(obj->size, (uint32_t)42);
        ASSERT(obj->hasSize);
        ASSERT_EQ(obj->comment, std::string("Entry point"));

        auto propIt = obj->properties.find("calling_convention");
        ASSERT(propIt != obj->properties.end());
        ASSERT_EQ(propIt->second.stringValue, std::string("custom"));

        ASSERT_EQ(obj->links.size(), (size_t)2);
        ASSERT(ctrl.hasLink(0x0100, 0x0200));
        ASSERT(ctrl.hasLink(0x0100, 0x0300));
    }

    cleanupTestFiles();
}

TEST(save_no_dirty_no_write)
{
    cleanupTestFiles();

    RdbController ctrl;
    ctrl.initialize("vector06c", RdbRomIdentity());
    // No objects, no dirty → save should succeed without creating file
    ASSERT(ctrl.saveAs(TEST_RDB_PATH));  // first save creates file
    // Now it exists. Reset dirty by reloading
    ctrl.reload();
    ASSERT(!ctrl.isDirty());

    // save() should not rewrite
    ASSERT(ctrl.save());
    ASSERT(!ctrl.isDirty());

    cleanupTestFiles();
}

TEST(save_without_dirty_returns_true)
{
    RdbController ctrl;
    // Never loaded, no path, no dirty
    ASSERT(ctrl.save());  // should return true (nothing to do)
}

TEST(load_nonexistent_fails)
{
    RdbController ctrl;
    ASSERT(!ctrl.load(testTmpPath("nonexistent_rdb_test_file.rdb")));
    ASSERT(!ctrl.isLoaded());
}

TEST(reload_without_load_fails)
{
    RdbController ctrl;
    ASSERT(!ctrl.reload());
}

// ---------------------------------------------------------------------------
// ROM identity tests
// ---------------------------------------------------------------------------

TEST(rom_identity_match)
{
    RdbController ctrl;
    RdbRomIdentity rom;
    rom.file = "game.rom";
    rom.size = 32768;
    rom.sha256 = "abcdef";
    ctrl.initialize("vector06c", rom);
    ctrl.saveAs(TEST_RDB_PATH);

    ctrl.reload();
    ASSERT(ctrl.matchesRom(rom));

    RdbRomIdentity other;
    other.file = "other.rom";
    other.size = 32768;
    other.sha256 = "123456";
    ASSERT(!ctrl.matchesRom(other));

    cleanupTestFiles();
}

// ---------------------------------------------------------------------------
// Object type conversion tests
// ---------------------------------------------------------------------------

TEST(object_type_roundtrip)
{
    ASSERT_EQ(std::string(rdbObjectTypeToString(RdbObjectType::Function)), std::string("function"));
    ASSERT_EQ(std::string(rdbObjectTypeToString(RdbObjectType::Variable)), std::string("variable"));
    ASSERT_EQ(std::string(rdbObjectTypeToString(RdbObjectType::Data)), std::string("data"));
    ASSERT_EQ(std::string(rdbObjectTypeToString(RdbObjectType::Unknown)), std::string("unknown"));

    ASSERT(rdbObjectTypeFromString("function") == RdbObjectType::Function);
    ASSERT(rdbObjectTypeFromString("variable") == RdbObjectType::Variable);
    ASSERT(rdbObjectTypeFromString("garbage") == RdbObjectType::Unknown);
}

// ---------------------------------------------------------------------------
// Property value tests
// ---------------------------------------------------------------------------

TEST(property_value_equality)
{
    ASSERT(RdbPropertyValue::fromString("a") == RdbPropertyValue::fromString("a"));
    ASSERT(RdbPropertyValue::fromString("a") != RdbPropertyValue::fromString("b"));
    ASSERT(RdbPropertyValue::fromInt(42) == RdbPropertyValue::fromInt(42));
    ASSERT(RdbPropertyValue::fromInt(42) != RdbPropertyValue::fromInt(43));
    ASSERT(RdbPropertyValue::fromBool(true) == RdbPropertyValue::fromBool(true));
    ASSERT(RdbPropertyValue::fromBool(true) != RdbPropertyValue::fromBool(false));
    // Different types are not equal
    ASSERT(RdbPropertyValue::fromString("1") != RdbPropertyValue::fromInt(1));
}

// ---------------------------------------------------------------------------
// Stage 6.28 — Protocol journal
// ---------------------------------------------------------------------------

static RdbProtocolEntry makeVerifiedEntry(const std::string &thesis) {
    RdbProtocolEntry e;
    e.thesis = thesis;
    e.status = ProtocolStatus::VerifiedByTrace;
    e.scope  = ProtocolScope::Hardware;
    e.hasMethod = true;
    e.method    = ProtocolMethod::Trace;
    e.evidence  = "breakpoint @0x1234: reader saw bit 2 = 0";
    return e;
}

static std::string readFileToString(const std::string &p) {
    std::ifstream f(p);
    return std::string((std::istreambuf_iterator<char>(f)),
                        std::istreambuf_iterator<char>());
}

TEST(protocol_add_assigns_id_and_timestamp)
{
    RdbController ctrl;
    ctrl.initialize("vector06c", RdbRomIdentity());
    RdbProtocolEntry e = makeVerifiedEntry("Keyboard port is active-low");
    ASSERT(ctrl.addProtocolEntry(e) == RdbOpResult::None);
    ASSERT(e.id == "P-0001");
    ASSERT(!e.timestamp.empty());
    ASSERT_EQ(ctrl.protocolCount(), (size_t)1);
    ASSERT(ctrl.isDirty());

    RdbProtocolEntry e2 = makeVerifiedEntry("Second claim");
    ASSERT(ctrl.addProtocolEntry(e2) == RdbOpResult::None);
    ASSERT(e2.id == "P-0002");
}

TEST(protocol_requires_evidence)
{
    RdbController ctrl;
    ctrl.initialize("vector06c", RdbRomIdentity());
    RdbProtocolEntry e;
    e.thesis = "Score at $2010";
    e.status = ProtocolStatus::Verified;   // requires method + evidence
    ASSERT(ctrl.addProtocolEntry(e) == RdbOpResult::MissingEvidence);
    ASSERT_EQ(ctrl.protocolCount(), (size_t)0);

    e.hasMethod = true; e.method = ProtocolMethod::Diff;
    ASSERT(ctrl.addProtocolEntry(e) == RdbOpResult::MissingEvidence);  // still no evidence
    e.evidence = "diff score=0 vs score=420 -> $2010 changed";
    ASSERT(ctrl.addProtocolEntry(e) == RdbOpResult::None);
}

TEST(protocol_speculation_no_evidence)
{
    RdbController ctrl;
    ctrl.initialize("vector06c", RdbRomIdentity());
    RdbProtocolEntry e;
    e.thesis = "Maybe overlays";
    e.status = ProtocolStatus::Speculation;
    e.scope  = ProtocolScope::Rom;
    ASSERT(ctrl.addProtocolEntry(e) == RdbOpResult::None);
}

TEST(protocol_empty_thesis_rejected)
{
    RdbController ctrl;
    ctrl.initialize("vector06c", RdbRomIdentity());
    RdbProtocolEntry e = makeVerifiedEntry("   ");
    ASSERT(ctrl.addProtocolEntry(e) == RdbOpResult::InvalidThesis);
}

TEST(protocol_supersede_history_preserved)
{
    RdbController ctrl;
    ctrl.initialize("vector06c", RdbRomIdentity());
    RdbProtocolEntry old = makeVerifiedEntry("$D000+ is packed graphics");
    old.scope = ProtocolScope::Rom;
    ASSERT(ctrl.addProtocolEntry(old) == RdbOpResult::None);

    RdbProtocolEntry nu;
    nu.thesis = "$D000+ is actually executable code (PC=$E2D7 observed)";
    nu.status = ProtocolStatus::Refuted;   // requires evidence
    nu.scope  = ProtocolScope::Rom;
    nu.hasMethod = true; nu.method = ProtocolMethod::Trace;
    nu.evidence = "checkpoint PC=$E2D7";
    std::string newId;
    ASSERT(ctrl.supersedeProtocolEntry(old.id, nu, newId) == RdbOpResult::None);
    ASSERT(newId == "P-0002");

    const RdbProtocolEntry *o = ctrl.findProtocolEntry("P-0001");
    ASSERT(o != nullptr);
    ASSERT(o->supersededBy == "P-0002");
    ASSERT(o->isSuperseded());
    ASSERT(ctrl.findProtocolEntry("P-0002")->supersedes == "P-0001");

    // activeOnly must hide the superseded entry.
    ProtocolFilter f; f.activeOnly = true;
    auto active = ctrl.getProtocol(f);
    ASSERT_EQ(active.size(), (size_t)1);
    ASSERT(active[0].id == "P-0002");

    // Cannot delete an entry another active entry supersedes.
    ASSERT(ctrl.removeProtocolEntry("P-0001") == RdbOpResult::Referenced);
}

TEST(protocol_get_filter_substring_and_status)
{
    RdbController ctrl;
    ctrl.initialize("vector06c", RdbRomIdentity());
    RdbProtocolEntry a = makeVerifiedEntry("Keyboard strobe polarity");
    RdbProtocolEntry b = makeVerifiedEntry("Score variable location");
    b.status = ProtocolStatus::VerifiedBySource; b.method = ProtocolMethod::PrimarySource;
    ASSERT(ctrl.addProtocolEntry(a) == RdbOpResult::None);
    ASSERT(ctrl.addProtocolEntry(b) == RdbOpResult::None);

    ProtocolFilter f; f.substring = "keyboard";
    ASSERT_EQ(ctrl.getProtocol(f).size(), (size_t)1);

    ProtocolFilter g; g.hasStatus = true; g.status = ProtocolStatus::VerifiedBySource;
    ASSERT_EQ(ctrl.getProtocol(g).size(), (size_t)1);
    ASSERT(ctrl.getProtocol(g)[0].id == "P-0002");
}

TEST(protocol_update_status_validates)
{
    RdbController ctrl;
    ctrl.initialize("vector06c", RdbRomIdentity());
    RdbProtocolEntry e;
    e.thesis = "Guess"; e.status = ProtocolStatus::Speculation; e.scope = ProtocolScope::Tooling;
    ASSERT(ctrl.addProtocolEntry(e) == RdbOpResult::None);
    // Promoting to VERIFIED without evidence must fail.
    ASSERT(ctrl.updateProtocolStatus(e.id, ProtocolStatus::Verified, false,
                                    ProtocolMethod::Trace, "") == RdbOpResult::MissingEvidence);
    ASSERT(ctrl.updateProtocolStatus(e.id, ProtocolStatus::Verified, true,
                                    ProtocolMethod::GoldenTest, "golden passes") == RdbOpResult::None);
    ASSERT(ctrl.findProtocolEntry(e.id)->status == ProtocolStatus::Verified);
}

// ---------------------------------------------------------------------------
// Stage 6.28 — Ports table
// ---------------------------------------------------------------------------

TEST(ports_upsert_and_list_sorted)
{
    RdbController ctrl;
    ctrl.initialize("vector06c", RdbRomIdentity());
    RdbPortEntry hi;
    hi.port = 0x0F; hi.name = "palette"; hi.direction = PortDirection::Out;
    hi.polarity = PortPolarity::ActiveHigh;
    hi.bits.push_back({0, "intensity"});
    ASSERT(ctrl.upsertPort(hi) == RdbOpResult::None);

    RdbPortEntry lo;
    lo.port = 0x01; lo.name = "keyboard"; lo.direction = PortDirection::In;
    lo.polarity = PortPolarity::ActiveLow;
    lo.bits.push_back({7, "shift"});
    ASSERT(ctrl.upsertPort(lo) == RdbOpResult::None);

    auto all = ctrl.listPorts();
    ASSERT_EQ(all.size(), (size_t)2);
    ASSERT_EQ(all[0].port, (uint8_t)0x01);   // ascending
    ASSERT(all[0].polarity == PortPolarity::ActiveLow);

    const RdbPortEntry *k = ctrl.getPort(0x01);
    ASSERT(k != nullptr);
    ASSERT(k->name == "keyboard");
    ASSERT_EQ(k->bits.size(), (size_t)1);
    ASSERT(k->bits[0].meaning == "shift");
}

TEST(ports_upsert_idempotent_no_dirty)
{
    RdbController ctrl;
    ctrl.initialize("vector06c", RdbRomIdentity());
    RdbPortEntry p; p.port = 0x0C; p.name = "color"; p.direction = PortDirection::Out;
    p.polarity = PortPolarity::NA;
    ASSERT(ctrl.upsertPort(p) == RdbOpResult::None);
    ctrl.saveAs(TEST_RDB_TMP);          // reset dirty
    ASSERT(!ctrl.isDirty());
    ASSERT(ctrl.upsertPort(p) == RdbOpResult::None);   // identical
    ASSERT(!ctrl.isDirty());
}

// ---------------------------------------------------------------------------
// Stage 6.28 — Watchlist
// ---------------------------------------------------------------------------

TEST(watch_add_update_remove)
{
    RdbController ctrl;
    ctrl.initialize("vector06c", RdbRomIdentity());
    RdbWatchEntry score; score.name = "score"; score.address = 0x2010; score.length = 2; score.format = "uint16-le";
    ASSERT(ctrl.addWatchEntry(score) == RdbOpResult::None);
    RdbWatchEntry demo; demo.name = "demo_flag"; demo.address = 0x0020; demo.format = "bits";
    ASSERT(ctrl.addWatchEntry(demo) == RdbOpResult::None);

    auto list = ctrl.listWatch();
    ASSERT_EQ(list.size(), (size_t)2);
    ASSERT_EQ(list[0].address, (uint16_t)0x0020);   // ascending by address

    // Same name + same content -> not dirty change.
    ctrl.saveAs(TEST_RDB_TMP);
    ASSERT(!ctrl.isDirty());
    ASSERT(ctrl.addWatchEntry(score) == RdbOpResult::None);
    ASSERT(!ctrl.isDirty());

    // Same name + changed address -> updates and marks dirty.
    RdbWatchEntry score2 = score; score2.address = 0x2040;
    ASSERT(ctrl.addWatchEntry(score2) == RdbOpResult::None);
    ASSERT(ctrl.isDirty());
    ASSERT_EQ(ctrl.watchCount(), (size_t)2);   // still keyed by name, no dup

    ASSERT(ctrl.removeWatchEntry("score") == RdbOpResult::None);
    ASSERT(ctrl.removeWatchEntry("score") == RdbOpResult::NotFound);
}

// ---------------------------------------------------------------------------
// Stage 6.28 — Versioning + persistence
// ---------------------------------------------------------------------------

TEST(version_bumps_to_2_with_knowledge)
{
    RdbController ctrl;
    RdbRomIdentity rom; rom.file = "g.rom"; rom.size = 32768; rom.sha256 = "deadbeef";
    ctrl.initialize("vector06c", rom, TEST_RDB_TMP);
    RdbPortEntry p; p.port = 0x01; p.name = "kb"; p.polarity = PortPolarity::ActiveLow;
    ctrl.upsertPort(p);
    ASSERT(ctrl.save() == true);
    std::string text = readFileToString(TEST_RDB_TMP);
    ASSERT(text.find("\"version\": 2") != std::string::npos);
    ::remove((std::string(TEST_RDB_TMP) + ".tmp").c_str());
}

TEST(version_stays_1_without_knowledge)
{
    RdbController ctrl;
    RdbRomIdentity rom; rom.file = "g.rom"; rom.size = 32768; rom.sha256 = "cafe";
    ctrl.initialize("vector06c", rom, TEST_RDB_TMP);
    ctrl.addObject(makeTestObject(0x100, "start"));   // objects only
    ASSERT(ctrl.save() == true);
    std::string text = readFileToString(TEST_RDB_TMP);
    ASSERT(text.find("\"version\": 1") != std::string::npos);
    ::remove((std::string(TEST_RDB_TMP) + ".tmp").c_str());
}

TEST(knowledge_roundtrip_persistence)
{
    RdbController ctrl;
    RdbRomIdentity rom; rom.file = "g.rom"; rom.size = 32768; rom.sha256 = "sha";
    ctrl.initialize("vector06c", rom, TEST_RDB_TMP);

    RdbProtocolEntry e = makeVerifiedEntry("Keyboard active-low");
    ASSERT(ctrl.addProtocolEntry(e) == RdbOpResult::None);
    RdbProtocolEntry nu;
    nu.thesis = "superseding claim"; nu.status = ProtocolStatus::Refuted; nu.scope = ProtocolScope::Rom;
    nu.hasMethod = true; nu.method = ProtocolMethod::Trace; nu.evidence = "e";
    std::string nid;
    ctrl.supersedeProtocolEntry(e.id, nu, nid);

    RdbPortEntry p; p.port = 0x01; p.name = "kb"; p.polarity = PortPolarity::ActiveLow; p.direction = PortDirection::In;
    p.bits.push_back({7, "shift"});
    ctrl.upsertPort(p);

    RdbWatchEntry w; w.name = "score"; w.address = 0x2010; w.length = 2; w.format = "uint16-le";
    ctrl.addWatchEntry(w);

    ASSERT(ctrl.save() == true);

    RdbController c2;
    ASSERT(c2.load(TEST_RDB_TMP));
    ASSERT_EQ(c2.protocolCount(), (size_t)2);
    ASSERT_EQ(c2.portCount(), (size_t)1);
    ASSERT_EQ(c2.watchCount(), (size_t)1);
    ASSERT(c2.findProtocolEntry("P-0001")->supersededBy == "P-0002");
    const RdbPortEntry *rp = c2.getPort(0x01);
    ASSERT(rp && rp->polarity == PortPolarity::ActiveLow && rp->bits[0].meaning == "shift");
    ASSERT(c2.listWatch()[0].name == "score");
    ASSERT(!c2.isDirty());   // load must not dirty
    // Counter resumed after load so the next id is P-0003.
    RdbProtocolEntry nx = makeVerifiedEntry("new after reload");
    ASSERT(c2.addProtocolEntry(nx) == RdbOpResult::None);
    ASSERT(nx.id == "P-0003");

    ::remove((std::string(TEST_RDB_TMP) + ".tmp").c_str());
}

TEST(promotion_protocol_to_port)
{
    // §9: a confirmed protocol entry is promoted into a port row. Promotion does
    // not delete the protocol entry — it links to the promoted target so the
    // "when / by what evidence" provenance survives. The link round-trips.
    RdbController ctrl;
    RdbRomIdentity rom; rom.file = "g.rom"; rom.size = 32768; rom.sha256 = "sha";
    ctrl.initialize("vector06c", rom, TEST_RDB_TMP);

    RdbProtocolEntry e = makeVerifiedEntry("Keyboard port 0x01 is active-low");
    ASSERT(ctrl.addProtocolEntry(e) == RdbOpResult::None);

    RdbPortEntry p; p.port = 0x01; p.name = "kb";
    p.polarity = PortPolarity::ActiveLow; p.direction = PortDirection::In;
    ASSERT(ctrl.upsertPort(p) == RdbOpResult::None);

    // Mark the entry as promoted by linking it to the port row (append a link).
    RdbProtocolEntry promoted = e;
    promoted.id.clear(); promoted.timestamp.clear();
    promoted.links.push_back("port:0x01");
    std::string nid;
    ASSERT(ctrl.supersedeProtocolEntry(e.id, promoted, nid) == RdbOpResult::None);
    ASSERT(ctrl.findProtocolEntry(nid)->links.size() == 1);

    ASSERT(ctrl.save());
    RdbController c2;
    ASSERT(c2.load(TEST_RDB_TMP));
    // Promotion history preserved: original superseded, promoted entry active
    // with its provenance link to the compiled port row.
    ASSERT(c2.findProtocolEntry(e.id)->isSuperseded());
    const RdbProtocolEntry *act = c2.findProtocolEntry(nid);
    ASSERT(act && !act->isSuperseded());
    ASSERT(act && act->links.size() == 1 && act->links[0] == "port:0x01");
    ASSERT(act && act->status == ProtocolStatus::VerifiedByTrace);
    ASSERT(c2.getPort(0x01) != nullptr);

    ::remove((std::string(TEST_RDB_TMP) + ".tmp").c_str());
}

TEST(load_rejects_version_3)
{
    std::string path = TEST_RDB_TMP;
    {
        std::ofstream f(path);
        f << "{\n  \"format\": \"rdb\",\n  \"platform\": \"vector06c\",\n"
             "  \"version\": 3,\n  \"objects\": []\n}\n";
    }
    RdbController ctrl;
    ASSERT(!ctrl.load(path));
}

TEST(parse_helpers_reject_unknown)
{
    ProtocolStatus st;
    ASSERT(parseProtocolStatus("verified_by_trace", st) && st == ProtocolStatus::VerifiedByTrace);
    ASSERT(parseProtocolStatus("VERIFIED", st) && st == ProtocolStatus::Verified);
    ASSERT(!parseProtocolStatus("MAYBE", st));

    ProtocolScope sc;
    ASSERT(parseProtocolScope("HARDWARE", sc) && sc == ProtocolScope::Hardware);
    ASSERT(!parseProtocolScope("network", sc));

    PortPolarity po;
    ASSERT(parsePortPolarity("active-low", po) && po == PortPolarity::ActiveLow);
    ASSERT(!parsePortPolarity("active-maybe", po));

    ASSERT(protocolStatusRequiresEvidence(ProtocolStatus::Refuted));
    ASSERT(!protocolStatusRequiresEvidence(ProtocolStatus::Speculation));
}

// ---------------------------------------------------------------------------
// Main
// ---------------------------------------------------------------------------

int main()
{
    std::cout << "RDB Controller Tests (Stage 6.11):" << std::endl;

    // Tests are auto-registered by static constructors above

    std::cout << std::endl;
    std::cout << "Results: " << testsPassed << " passed, "
              << testsFailed << " failed" << std::endl;

    cleanupTestFiles();

    return testsFailed > 0 ? 1 : 0;
}
