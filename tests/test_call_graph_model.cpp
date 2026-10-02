#include "call_graph_model.h"
#include "call_graph_window.h"  // graphPathFromRdbPath

#include <cassert>
#include <cstdio>
#include <vector>
#include <string>
#include <algorithm>

// ---------------------------------------------------------------------------
// Minimal test framework
// ---------------------------------------------------------------------------

static int tests_run    = 0;
static int tests_passed = 0;

#define TEST_BEGIN(name) \
    do { \
        tests_run++; \
        const char *_test_name = (name);

#define TEST_END() \
        tests_passed++; \
        printf("  PASS: %s\n", _test_name); \
    } while (0);

#define ASSERT_EQ(a, b) \
    if ((a) != (b)) { \
        printf("  FAIL: %s — line %d: %s != %s\n", _test_name, __LINE__, #a, #b); \
        break; \
    }

#define ASSERT_TRUE(cond) \
    if (!(cond)) { \
        printf("  FAIL: %s — line %d: %s\n", _test_name, __LINE__, #cond); \
        break; \
    }

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static RdbObject makeObject(uint16_t addr, const std::string &name,
                             RdbObjectType type = RdbObjectType::Function)
{
    RdbObject obj;
    obj.address = addr;
    obj.name    = name;
    obj.type    = type;
    return obj;
}

static bool hasEdge(const CallGraphModel &model, uint16_t src, uint16_t tgt)
{
    for (const auto &e : model.edges)
        if (e.source == src && e.target == tgt)
            return true;
    return false;
}

static bool hasNode(const CallGraphModel &model, uint16_t addr)
{
    return model.addressToIndex.count(addr) > 0;
}

static const CallGraphNode &getNode(const CallGraphModel &model, uint16_t addr)
{
    return model.nodes[model.addressToIndex.at(addr)];
}

// ---------------------------------------------------------------------------
// Tests: Nodes
// ---------------------------------------------------------------------------

static void test_empty_rdb()
{
    TEST_BEGIN("empty RDB → empty model");
    std::vector<RdbObject> objects;
    auto model = buildCallGraphModel(objects);
    ASSERT_EQ(model.nodes.size(), 0u);
    ASSERT_EQ(model.edges.size(), 0u);
    ASSERT_EQ(model.unresolvedCount, 0u);
    TEST_END();
}

static void test_single_object_no_links()
{
    TEST_BEGIN("single object, no links → 1 node, 0 edges");
    std::vector<RdbObject> objects = { makeObject(0x4000, "main") };
    auto model = buildCallGraphModel(objects);
    ASSERT_EQ(model.nodes.size(), 1u);
    ASSERT_EQ(model.edges.size(), 0u);
    ASSERT_EQ(getNode(model, 0x4000).name, std::string("main"));
    ASSERT_TRUE(!getNode(model, 0x4000).unresolved);
    TEST_END();
}

static void test_multiple_objects()
{
    TEST_BEGIN("multiple objects → N nodes");
    std::vector<RdbObject> objects = {
        makeObject(0x4000, "main"),
        makeObject(0x4100, "init"),
        makeObject(0x4200, "loop"),
    };
    auto model = buildCallGraphModel(objects);
    ASSERT_EQ(model.nodes.size(), 3u);
    ASSERT_TRUE(hasNode(model, 0x4000));
    ASSERT_TRUE(hasNode(model, 0x4100));
    ASSERT_TRUE(hasNode(model, 0x4200));
    TEST_END();
}

static void test_missing_name()
{
    TEST_BEGIN("object with empty name");
    std::vector<RdbObject> objects = { makeObject(0x4000, "") };
    auto model = buildCallGraphModel(objects);
    ASSERT_EQ(model.nodes.size(), 1u);
    ASSERT_EQ(getNode(model, 0x4000).name, std::string(""));
    TEST_END();
}

static void test_unresolved_target()
{
    TEST_BEGIN("unresolved target → unresolved node");
    RdbObject obj = makeObject(0x4000, "main");
    obj.links = { 0x8123 };
    std::vector<RdbObject> objects = { obj };
    auto model = buildCallGraphModel(objects);
    ASSERT_EQ(model.nodes.size(), 2u);
    ASSERT_TRUE(hasNode(model, 0x8123));
    ASSERT_TRUE(getNode(model, 0x8123).unresolved);
    ASSERT_EQ(model.unresolvedCount, 1u);
    TEST_END();
}

static void test_object_type_preserved()
{
    TEST_BEGIN("object type preserved in node");
    std::vector<RdbObject> objects = {
        makeObject(0x4000, "main", RdbObjectType::Function),
        makeObject(0x5000, "data", RdbObjectType::Variable),
        makeObject(0x6000, "tbl",  RdbObjectType::Table),
    };
    auto model = buildCallGraphModel(objects);
    ASSERT_EQ(getNode(model, 0x4000).type, RdbObjectType::Function);
    ASSERT_EQ(getNode(model, 0x5000).type, RdbObjectType::Variable);
    ASSERT_EQ(getNode(model, 0x6000).type, RdbObjectType::Table);
    TEST_END();
}

// ---------------------------------------------------------------------------
// Tests: Links
// ---------------------------------------------------------------------------

static void test_single_link()
{
    TEST_BEGIN("single link → 1 edge");
    RdbObject a = makeObject(0x4000, "A");
    a.links = { 0x4100 };
    RdbObject b = makeObject(0x4100, "B");
    std::vector<RdbObject> objects = { a, b };
    auto model = buildCallGraphModel(objects);
    ASSERT_EQ(model.edges.size(), 1u);
    ASSERT_TRUE(hasEdge(model, 0x4000, 0x4100));
    TEST_END();
}

static void test_multiple_links()
{
    TEST_BEGIN("multiple links → multiple edges");
    RdbObject a = makeObject(0x4000, "A");
    a.links = { 0x4100, 0x4200 };
    RdbObject b = makeObject(0x4100, "B");
    RdbObject c = makeObject(0x4200, "C");
    std::vector<RdbObject> objects = { a, b, c };
    auto model = buildCallGraphModel(objects);
    ASSERT_EQ(model.edges.size(), 2u);
    ASSERT_TRUE(hasEdge(model, 0x4000, 0x4100));
    ASSERT_TRUE(hasEdge(model, 0x4000, 0x4200));
    TEST_END();
}

static void test_duplicate_links()
{
    TEST_BEGIN("duplicate links → 1 edge");
    RdbObject a = makeObject(0x4000, "A");
    a.links = { 0x4100, 0x4100, 0x4100 };
    RdbObject b = makeObject(0x4100, "B");
    std::vector<RdbObject> objects = { a, b };
    auto model = buildCallGraphModel(objects);
    ASSERT_EQ(model.edges.size(), 1u);
    ASSERT_TRUE(hasEdge(model, 0x4000, 0x4100));
    TEST_END();
}

static void test_self_link()
{
    TEST_BEGIN("self-link A → A");
    RdbObject a = makeObject(0x4000, "A");
    a.links = { 0x4000 };
    std::vector<RdbObject> objects = { a };
    auto model = buildCallGraphModel(objects);
    ASSERT_EQ(model.edges.size(), 1u);
    ASSERT_TRUE(hasEdge(model, 0x4000, 0x4000));
    TEST_END();
}

static void test_cyclic_links()
{
    TEST_BEGIN("cyclic links A → B → A");
    RdbObject a = makeObject(0x4000, "A");
    a.links = { 0x4100 };
    RdbObject b = makeObject(0x4100, "B");
    b.links = { 0x4000 };
    std::vector<RdbObject> objects = { a, b };
    auto model = buildCallGraphModel(objects);
    ASSERT_EQ(model.edges.size(), 2u);
    ASSERT_TRUE(hasEdge(model, 0x4000, 0x4100));
    ASSERT_TRUE(hasEdge(model, 0x4100, 0x4000));
    TEST_END();
}

// ---------------------------------------------------------------------------
// Tests: Build correctness
// ---------------------------------------------------------------------------

static void test_n_objects_n_nodes()
{
    TEST_BEGIN("N objects → N nodes (no unresolved)");
    std::vector<RdbObject> objects;
    for (uint16_t i = 0; i < 100; i++)
        objects.push_back(makeObject(0x1000 + i * 0x10, ""));
    auto model = buildCallGraphModel(objects);
    ASSERT_EQ(model.nodes.size(), 100u);
    ASSERT_EQ(model.unresolvedCount, 0u);
    TEST_END();
}

static void test_e_unique_links_e_edges()
{
    TEST_BEGIN("E unique links → E edges");
    RdbObject a = makeObject(0x4000, "A");
    a.links = { 0x4100, 0x4200, 0x4300, 0x4400 };
    std::vector<RdbObject> objects = {
        a,
        makeObject(0x4100, "B"),
        makeObject(0x4200, "C"),
        makeObject(0x4300, "D"),
        makeObject(0x4400, "E"),
    };
    auto model = buildCallGraphModel(objects);
    ASSERT_EQ(model.edges.size(), 4u);
    TEST_END();
}

static void test_complex_graph()
{
    TEST_BEGIN("complex graph: A→B, A→C, B→C, C→D");
    RdbObject a = makeObject(0x01, "A");
    a.links = { 0x02, 0x03 };
    RdbObject b = makeObject(0x02, "B");
    b.links = { 0x03 };
    RdbObject c = makeObject(0x03, "C");
    c.links = { 0x04 };
    RdbObject d = makeObject(0x04, "D");
    std::vector<RdbObject> objects = { a, b, c, d };
    auto model = buildCallGraphModel(objects);
    ASSERT_EQ(model.nodes.size(), 4u);
    ASSERT_EQ(model.edges.size(), 4u);
    ASSERT_TRUE(hasEdge(model, 0x01, 0x02));
    ASSERT_TRUE(hasEdge(model, 0x01, 0x03));
    ASSERT_TRUE(hasEdge(model, 0x02, 0x03));
    ASSERT_TRUE(hasEdge(model, 0x03, 0x04));
    TEST_END();
}

static void test_unresolved_in_complex()
{
    TEST_BEGIN("A → 0x9000 (unresolved)");
    RdbObject a = makeObject(0x01, "A");
    a.links = { 0x9000 };
    std::vector<RdbObject> objects = { a };
    auto model = buildCallGraphModel(objects);
    ASSERT_EQ(model.nodes.size(), 2u);
    ASSERT_EQ(model.edges.size(), 1u);
    ASSERT_TRUE(hasNode(model, 0x9000));
    ASSERT_TRUE(getNode(model, 0x9000).unresolved);
    ASSERT_EQ(model.unresolvedCount, 1u);
    ASSERT_TRUE(hasEdge(model, 0x01, 0x9000));
    TEST_END();
}

// ---------------------------------------------------------------------------
// Tests: RDB changes → rebuild
// ---------------------------------------------------------------------------

static void test_rebuild_after_change()
{
    TEST_BEGIN("Build → modify → rebuild reflects new data");
    // First build
    RdbObject a = makeObject(0x4000, "A");
    a.links = { 0x4100 };
    RdbObject b = makeObject(0x4100, "B");
    std::vector<RdbObject> objects1 = { a, b };
    auto model1 = buildCallGraphModel(objects1);
    ASSERT_EQ(model1.edges.size(), 1u);

    // Modify: add new object + link
    RdbObject c = makeObject(0x4200, "C");
    a.links = { 0x4100, 0x4200 };
    std::vector<RdbObject> objects2 = { a, b, c };
    auto model2 = buildCallGraphModel(objects2);
    ASSERT_EQ(model2.nodes.size(), 3u);
    ASSERT_EQ(model2.edges.size(), 2u);
    ASSERT_TRUE(hasEdge(model2, 0x4000, 0x4200));
    TEST_END();
}

// ---------------------------------------------------------------------------
// Tests: Comment and properties preserved
// ---------------------------------------------------------------------------

static void test_comment_preserved()
{
    TEST_BEGIN("comment preserved in node");
    RdbObject obj = makeObject(0x4000, "main");
    obj.comment = "Entry point";
    std::vector<RdbObject> objects = { obj };
    auto model = buildCallGraphModel(objects);
    ASSERT_EQ(getNode(model, 0x4000).comment, std::string("Entry point"));
    TEST_END();
}

static void test_properties_preserved()
{
    TEST_BEGIN("properties preserved in node");
    RdbObject obj = makeObject(0x4000, "main");
    obj.properties["calling_convention"] = RdbPropertyValue::fromString("cdecl");
    std::vector<RdbObject> objects = { obj };
    auto model = buildCallGraphModel(objects);
    auto it = getNode(model, 0x4000).properties.find("calling_convention");
    ASSERT_TRUE(it != getNode(model, 0x4000).properties.end());
    ASSERT_EQ(it->second.stringValue, std::string("cdecl"));
    TEST_END();
}

// ---------------------------------------------------------------------------
// Tests: Duplicate address handling
// ---------------------------------------------------------------------------

static void test_duplicate_addresses()
{
    TEST_BEGIN("duplicate addresses → first wins");
    RdbObject a1 = makeObject(0x4000, "first");
    RdbObject a2 = makeObject(0x4000, "second");
    std::vector<RdbObject> objects = { a1, a2 };
    auto model = buildCallGraphModel(objects);
    ASSERT_EQ(model.nodes.size(), 1u);
    ASSERT_EQ(getNode(model, 0x4000).name, std::string("first"));
    TEST_END();
}

// ---------------------------------------------------------------------------
// Tests: graphPathFromRdbPath
// ---------------------------------------------------------------------------

static void test_graph_path_from_rdb()
{
    TEST_BEGIN("graphPathFromRdbPath: game.rdb → game.rdb.graph");
    ASSERT_EQ(graphPathFromRdbPath("game.rdb"), std::string("game.rdb.graph"));
    TEST_END();
}

static void test_graph_path_from_rdb_with_dir()
{
    TEST_BEGIN("graphPathFromRdbPath: /path/to/game.rdb");
    ASSERT_EQ(graphPathFromRdbPath("/path/to/game.rdb"),
              std::string("/path/to/game.rdb.graph"));
    TEST_END();
}

static void test_graph_path_empty()
{
    TEST_BEGIN("graphPathFromRdbPath: empty → empty");
    ASSERT_EQ(graphPathFromRdbPath(""), std::string(""));
    TEST_END();
}

// ---------------------------------------------------------------------------
// Main
// ---------------------------------------------------------------------------

int main()
{
    printf("=== Call Graph Model Tests (Stage 6.12) ===\n\n");

    // Nodes
    test_empty_rdb();
    test_single_object_no_links();
    test_multiple_objects();
    test_missing_name();
    test_unresolved_target();
    test_object_type_preserved();

    // Links
    test_single_link();
    test_multiple_links();
    test_duplicate_links();
    test_self_link();
    test_cyclic_links();

    // Build
    test_n_objects_n_nodes();
    test_e_unique_links_e_edges();
    test_complex_graph();
    test_unresolved_in_complex();

    // RDB changes
    test_rebuild_after_change();

    // Properties
    test_comment_preserved();
    test_properties_preserved();

    // Edge cases
    test_duplicate_addresses();

    // Graph path derivation
    test_graph_path_from_rdb();
    test_graph_path_from_rdb_with_dir();
    test_graph_path_empty();

    printf("\n=== Results: %d/%d passed ===\n", tests_passed, tests_run);
    return (tests_passed == tests_run) ? 0 : 1;
}
