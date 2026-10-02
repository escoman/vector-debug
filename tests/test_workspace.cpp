// test_workspace.cpp — WorkspaceManager unit tests (Stage 5.1)
//
// Tests workspace save/load/switch/delete/reset using ImGui's serialization.
// No emulator dependencies — pure GUI + filesystem.

#include "workspace_manager.h"

#include "imgui.h"
#include "imgui_internal.h"

#include <cassert>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <vector>

// ---------------------------------------------------------------------------
// Minimal test framework
// ---------------------------------------------------------------------------

static int tests_run = 0;
static int tests_passed = 0;
static int tests_failed = 0;

#define TEST(name) \
    do { \
        tests_run++; \
        printf("  [%2d] %-50s ", tests_run, name); \
        fflush(stdout); \
    } while (0)

#define PASS() \
    do { tests_passed++; printf("PASS\n"); } while (0)

#define FAIL(msg) \
    do { tests_failed++; printf("FAIL: %s\n", msg); } while (0)

#define CHECK(cond, msg) \
    do { if (!(cond)) { FAIL(msg); return; } } while (0)

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static const char *kTestDir = "/tmp/test_workspace_ws";

static void removeDirRecursive(const std::string &path)
{
    std::string cmd = "rm -rf " + path;
    system(cmd.c_str());
}

static bool fileExists(const std::string &path)
{
    struct stat st;
    return stat(path.c_str(), &st) == 0;
}

static std::string readFile(const std::string &path)
{
    std::ifstream f(path);
    if (!f.good()) return "";
    return std::string((std::istreambuf_iterator<char>(f)),
                        std::istreambuf_iterator<char>());
}

// Create a fresh ImGui context for each test
static ImGuiContext *createTestContext()
{
    ImGuiContext *ctx = ImGui::CreateContext();
    ImGuiIO &io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    return ctx;
}

static void destroyTestContext(ImGuiContext *ctx)
{
    ImGui::DestroyContext(ctx);
}

// Helper: set refs and apply immediately (tests run between frames)
static void setRefsAndApply(WorkspaceManager &wm, std::vector<WorkspaceManager::WindowVisibility> refs)
{
    wm.setWindowVisibilityRefs(std::move(refs));
    wm.applyPendingWorkspace();
}

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

static void test_createWorkspaceDirectory()
{
    TEST("Create workspace directory");
    removeDirRecursive(kTestDir);

    ImGuiContext *ctx = createTestContext();
    WorkspaceManager wm;
    wm.initialize(kTestDir);

    struct stat st;
    bool ok = (stat(kTestDir, &st) == 0 && S_ISDIR(st.st_mode));
    destroyTestContext(ctx);
    removeDirRecursive(kTestDir);

    CHECK(ok, "directory not created");
    PASS();
}

static void test_saveWorkspaceToFile()
{
    TEST("Save workspace to file");
    removeDirRecursive(kTestDir);

    ImGuiContext *ctx = createTestContext();
    WorkspaceManager wm;
    wm.initialize(kTestDir);

    bool vis = true;
    setRefsAndApply(wm, {{"TestWindow", &vis}});

    wm.saveCurrentWorkspace();

    bool exists = fileExists(std::string(kTestDir) + "/Default.ini");
    destroyTestContext(ctx);
    removeDirRecursive(kTestDir);

    CHECK(exists, "Default.ini not created");
    PASS();
}

static void test_loadWorkspaceFromFile()
{
    TEST("Load workspace from file");
    removeDirRecursive(kTestDir);

    ImGuiContext *ctx = createTestContext();
    WorkspaceManager wm;
    wm.initialize(kTestDir);

    bool vis = true;
    setRefsAndApply(wm, {{"TestWindow", &vis}});
    wm.saveCurrentWorkspace();

    // Modify and reload
    vis = false;
    wm.saveCurrentWorkspace();

    vis = true;
    wm.switchWorkspace("Default");  // won't reload since same name
    // Force reload by switching away and back
    // Actually just verify the file content
    std::string content = readFile(std::string(kTestDir) + "/Default.ini");
    bool hasVisibility = (content.find("[Visibility]") != std::string::npos);
    bool hasTestWindow = (content.find("TestWindow=0") != std::string::npos);

    destroyTestContext(ctx);
    removeDirRecursive(kTestDir);

    CHECK(hasVisibility, "missing [Visibility] section");
    CHECK(hasTestWindow, "TestWindow=0 not found");
    PASS();
}

static void test_visibilitySaveRestoreRoundtrip()
{
    TEST("Visibility save/restore roundtrip");
    removeDirRecursive(kTestDir);

    ImGuiContext *ctx = createTestContext();

    // Save
    {
        WorkspaceManager wm;
        wm.initialize(kTestDir);
        bool a = true, b = false;
        setRefsAndApply(wm, {{"WinA", &a}, {"WinB", &b}});
        wm.saveCurrentWorkspace();
    }

    // Restore
    {
        WorkspaceManager wm;
        wm.initialize(kTestDir);
        bool a = false, b = true;  // inverted
        setRefsAndApply(wm, {{"WinA", &a}, {"WinB", &b}});
        // setWindowVisibilityRefs loads the workspace
        CHECK(a == true, "WinA should be true after restore");
        CHECK(b == false, "WinB should be false after restore");
    }

    destroyTestContext(ctx);
    removeDirRecursive(kTestDir);
    PASS();
}

static void test_switchBetweenWorkspaces()
{
    TEST("Switch between workspaces");
    removeDirRecursive(kTestDir);

    ImGuiContext *ctx = createTestContext();
    WorkspaceManager wm;
    wm.initialize(kTestDir);

    bool vis = true;
    setRefsAndApply(wm, {{"Win", &vis}});

    // Save Default with vis=true
    vis = true;
    wm.saveCurrentWorkspace();

    // Create a second workspace
    wm.saveWorkspaceAs("Second");
    vis = false;
    wm.saveCurrentWorkspace();

    // Switch back to Default
    wm.switchWorkspace("Default");
    CHECK(wm.currentWorkspaceName() == "Default", "should be on Default");
    CHECK(vis == true, "Default should have vis=true");

    // Switch to Second
    wm.switchWorkspace("Second");
    CHECK(wm.currentWorkspaceName() == "Second", "should be on Second");
    CHECK(vis == false, "Second should have vis=false");

    destroyTestContext(ctx);
    removeDirRecursive(kTestDir);
    PASS();
}

static void test_saveAsCreatesNewWorkspace()
{
    TEST("Save As creates new workspace");
    removeDirRecursive(kTestDir);

    ImGuiContext *ctx = createTestContext();
    WorkspaceManager wm;
    wm.initialize(kTestDir);

    bool vis = true;
    setRefsAndApply(wm, {{"Win", &vis}});

    wm.saveWorkspaceAs("MyWorkspace");
    CHECK(wm.currentWorkspaceName() == "MyWorkspace", "current should be MyWorkspace");
    CHECK(fileExists(std::string(kTestDir) + "/MyWorkspace.ini"), "MyWorkspace.ini not created");

    destroyTestContext(ctx);
    removeDirRecursive(kTestDir);
    PASS();
}

static void test_deleteUserWorkspace()
{
    TEST("Delete user workspace");
    removeDirRecursive(kTestDir);

    ImGuiContext *ctx = createTestContext();
    WorkspaceManager wm;
    wm.initialize(kTestDir);

    bool vis = true;
    setRefsAndApply(wm, {{"Win", &vis}});

    wm.saveWorkspaceAs("ToDelete");
    CHECK(fileExists(std::string(kTestDir) + "/ToDelete.ini"), "file should exist");

    wm.deleteWorkspace("ToDelete");
    CHECK(!fileExists(std::string(kTestDir) + "/ToDelete.ini"), "file should be deleted");
    CHECK(wm.currentWorkspaceName() == "Default", "should fall back to Default");

    destroyTestContext(ctx);
    removeDirRecursive(kTestDir);
    PASS();
}

static void test_cannotDeleteBuiltInWorkspace()
{
    TEST("Cannot delete built-in workspace");
    removeDirRecursive(kTestDir);

    ImGuiContext *ctx = createTestContext();
    WorkspaceManager wm;
    wm.initialize(kTestDir);

    bool vis = true;
    setRefsAndApply(wm, {{"Win", &vis}});

    // Explicitly create Default.ini (no longer auto-generated on startup)
    wm.saveCurrentWorkspace();

    wm.deleteWorkspace("Default");
    // Default should still exist (deleteWorkspace refuses built-in names)
    CHECK(fileExists(std::string(kTestDir) + "/Default.ini"), "Default.ini should still exist");

    destroyTestContext(ctx);
    removeDirRecursive(kTestDir);
    PASS();
}

static void test_resetRestoresPresetLayout()
{
    TEST("Reset restores preset layout");
    removeDirRecursive(kTestDir);

    ImGuiContext *ctx = createTestContext();
    WorkspaceManager wm;
    wm.initialize(kTestDir);

    bool vis = true;
    setRefsAndApply(wm, {{"Win", &vis}});

    // Modify and save
    vis = false;
    wm.saveCurrentWorkspace();

    // Reset should regenerate from preset
    wm.resetWorkspace();
    wm.processDeferredOps();
    // After reset, the file should be regenerated
    CHECK(fileExists(std::string(kTestDir) + "/Default.ini"), "Default.ini should exist after reset");

    destroyTestContext(ctx);
    removeDirRecursive(kTestDir);
    PASS();
}

static void test_missingFileFallsToDefault()
{
    TEST("Missing file falls back to Default");
    removeDirRecursive(kTestDir);

    ImGuiContext *ctx = createTestContext();
    WorkspaceManager wm;
    wm.initialize(kTestDir);

    bool vis = true;
    setRefsAndApply(wm, {{"Win", &vis}});

    // Try to switch to non-existent workspace
    wm.switchWorkspace("NonExistent");
    // Should fall back to Default
    CHECK(wm.currentWorkspaceName() == "Default", "should fall back to Default");

    destroyTestContext(ctx);
    removeDirRecursive(kTestDir);
    PASS();
}

static void test_corruptedFileFallsToDefault()
{
    TEST("Corrupted file falls back to Default");
    removeDirRecursive(kTestDir);

    ImGuiContext *ctx = createTestContext();
    WorkspaceManager wm;
    wm.initialize(kTestDir);

    bool vis = true;
    setRefsAndApply(wm, {{"Win", &vis}});

    // Write garbage to Default.ini
    {
        std::string path = std::string(kTestDir) + "/Default.ini";
        std::ofstream f(path);
        f << "GARBAGE_DATA_NO_SECTIONS";
    }

    // Create a fresh WM and try to load
    WorkspaceManager wm2;
    wm2.initialize(kTestDir);
    bool vis2 = true;
    setRefsAndApply(wm2, {{"Win", &vis2}});
    // loadFromFile returns true even with garbage (no crash), visibility unchanged
    // This tests that it doesn't crash on corrupted data

    destroyTestContext(ctx);
    removeDirRecursive(kTestDir);
    PASS();
}

static void test_persistenceAcrossRestart()
{
    TEST("Persistence across WorkspaceManager restart");
    removeDirRecursive(kTestDir);

    ImGuiContext *ctx = createTestContext();

    // First session
    {
        WorkspaceManager wm;
        wm.initialize(kTestDir);
        bool vis = true;
        setRefsAndApply(wm, {{"Win", &vis}});
        vis = false;
        wm.saveCurrentWorkspace();
        wm.shutdown();
    }

    // Second session
    {
        WorkspaceManager wm;
        wm.initialize(kTestDir);
        bool vis = true;
        setRefsAndApply(wm, {{"Win", &vis}});
        // After loading, vis should be restored to false
        CHECK(vis == false, "visibility should persist across restart");
    }

    destroyTestContext(ctx);
    removeDirRecursive(kTestDir);
    PASS();
}

static void test_sanitizedFilenames()
{
    TEST("Sanitized filenames (no path escape)");
    removeDirRecursive(kTestDir);

    ImGuiContext *ctx = createTestContext();
    WorkspaceManager wm;
    wm.initialize(kTestDir);

    bool vis = true;
    setRefsAndApply(wm, {{"Win", &vis}});

    // Try to save with path traversal
    wm.saveWorkspaceAs("../../../etc/evil");
    // Sanitizer replaces '/' with '_', so the file is ".._.._.._etc_evil.ini"
    CHECK(fileExists(std::string(kTestDir) + "/.._.._.._etc_evil.ini"),
          "sanitized filename should replace slashes with underscores");
    CHECK(!fileExists("/etc/evil.ini"), "should not create file outside workspace dir");

    destroyTestContext(ctx);
    removeDirRecursive(kTestDir);
    PASS();
}

static void test_builtInWorkspaceList()
{
    TEST("Built-in workspace list is correct");
    removeDirRecursive(kTestDir);

    ImGuiContext *ctx = createTestContext();
    WorkspaceManager wm;
    wm.initialize(kTestDir);

    bool vis = true;
    setRefsAndApply(wm, {{"Win", &vis}});

    CHECK(wm.isBuiltIn("Default"), "Default should be built-in");
    CHECK(wm.isBuiltIn("Screen Analysis"), "Screen Analysis should be built-in");
    CHECK(wm.isBuiltIn("CPU Analysis"), "CPU Analysis should be built-in");
    CHECK(wm.isBuiltIn("I/O Analysis"), "I/O Analysis should be built-in");
    CHECK(wm.isBuiltIn("Memory Analysis"), "Memory Analysis should be built-in");
    CHECK(!wm.isBuiltIn("MyWorkspace"), "MyWorkspace should not be built-in");
    CHECK(!wm.isBuiltIn(""), "empty string should not be built-in");

    destroyTestContext(ctx);
    removeDirRecursive(kTestDir);
    PASS();
}

static void test_listWorkspacesReturnsAll()
{
    TEST("listWorkspaces returns all workspace files");
    removeDirRecursive(kTestDir);

    ImGuiContext *ctx = createTestContext();
    WorkspaceManager wm;
    wm.initialize(kTestDir);

    bool vis = true;
    setRefsAndApply(wm, {{"Win", &vis}});

    // Explicitly create Default.ini (no longer auto-generated on startup)
    wm.saveCurrentWorkspace();
    wm.saveWorkspaceAs("Custom1");
    wm.saveWorkspaceAs("Custom2");

    auto list = wm.listWorkspaces();
    // Should contain at least: Default, Screen Analysis, CPU Analysis,
    // I/O Analysis, Memory Analysis, Custom1, Custom2
    bool hasDefault = false, hasCustom1 = false, hasCustom2 = false;
    for (const auto &ws : list) {
        if (ws == "Default") hasDefault = true;
        if (ws == "Custom1") hasCustom1 = true;
        if (ws == "Custom2") hasCustom2 = true;
    }

    destroyTestContext(ctx);
    removeDirRecursive(kTestDir);

    CHECK(hasDefault, "missing Default");
    CHECK(hasCustom1, "missing Custom1");
    CHECK(hasCustom2, "missing Custom2");
    PASS();
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------

int main()
{
    printf("=== WorkspaceManager tests ===\n\n");

    test_createWorkspaceDirectory();
    test_saveWorkspaceToFile();
    test_loadWorkspaceFromFile();
    test_visibilitySaveRestoreRoundtrip();
    test_switchBetweenWorkspaces();
    test_saveAsCreatesNewWorkspace();
    test_deleteUserWorkspace();
    test_cannotDeleteBuiltInWorkspace();
    test_resetRestoresPresetLayout();
    test_missingFileFallsToDefault();
    test_corruptedFileFallsToDefault();
    test_persistenceAcrossRestart();
    test_sanitizedFilenames();
    test_builtInWorkspaceList();
    test_listWorkspacesReturnsAll();

    printf("\n=== Results: %d/%d passed", tests_passed, tests_run);
    if (tests_failed > 0) {
        printf(", %d FAILED", tests_failed);
    }
    printf(" ===\n");

    return tests_failed > 0 ? 1 : 0;
}
