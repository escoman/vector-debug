// test_gui_smoke.cpp — GUI launch smoke tests (Stage 5.1)
//
// Spawns the actual v06c-debugger binary and verifies it:
//   1. Starts without crashing (no SIGABRT/SIGSEGV)
//   2. Prints expected startup messages
//   3. Shuts down cleanly on SIGTERM
//
// Two scenarios:
//   - Launch without ROM (just boot screen)
//   - Launch with a test ROM loaded
//
// Requires a display (X11/Wayland). In headless CI, wrap with xvfb-run.

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <csignal>
#include <unistd.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <string>
#include <poll.h>

// ---------------------------------------------------------------------------
// Minimal test framework
// ---------------------------------------------------------------------------

static int tests_run = 0;
static int tests_passed = 0;
static int tests_failed = 0;

#define TEST(name) \
    do { \
        tests_run++; \
        printf("  [%d] %-50s ", tests_run, name); \
        fflush(stdout); \
    } while (0)

#define PASS() \
    do { tests_passed++; printf("PASS\n"); } while (0)

#define FAIL(msg) \
    do { tests_failed++; printf("FAIL: %s\n", msg); } while (0)

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

// Resolve path to v06c-debugger binary (same build directory as this test)
static std::string debuggerBinaryPath()
{
    // The test binary and v06c-debugger are in the same CMake build dir
    char buf[4096];
    ssize_t len = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (len <= 0) return "";
    buf[len] = '\0';

    // Strip executable name to get directory
    char *slash = strrchr(buf, '/');
    if (slash) *(slash + 1) = '\0';

    return std::string(buf) + "v06c-debugger";
}

static bool fileExists(const std::string &path)
{
    struct stat st;
    return stat(path.c_str(), &st) == 0;
}

// Read available output from a pipe fd into a string (non-blocking).
static std::string readAvailable(int fd, int timeout_ms = 500)
{
    std::string result;
    char buf[1024];
    struct pollfd pfd;
    pfd.fd = fd;
    pfd.events = POLLIN;

    while (poll(&pfd, 1, timeout_ms) > 0 && (pfd.revents & POLLIN)) {
        ssize_t n = read(fd, buf, sizeof(buf) - 1);
        if (n <= 0) break;
        buf[n] = '\0';
        result += buf;
        timeout_ms = 100;  // subsequent reads: shorter timeout
    }
    return result;
}

struct ProcessResult {
    bool started;         // process was spawned successfully
    bool crashed;         // process died from signal (SIGABRT, SIGSEGV, etc.)
    int  exitStatus;      // exit code (if !crashed)
    int  termSignal;      // signal that killed (if crashed)
    std::string output;   // captured stdout+stderr
};

// Spawn v06c-debugger with optional arguments, wait, then send SIGTERM.
// Returns collected output and exit status.
static ProcessResult spawnDebugger(const std::string &binary,
                                    const std::string &extraArg = "",
                                    int runSeconds = 3)
{
    ProcessResult res;
    res.started = false;
    res.crashed = false;
    res.exitStatus = -1;
    res.termSignal = 0;

    // Create pipe for capturing child stdout+stderr
    int pipefd[2];
    if (pipe(pipefd) != 0) {
        res.output = "pipe() failed";
        return res;
    }

    pid_t pid = fork();
    if (pid < 0) {
        close(pipefd[0]);
        close(pipefd[1]);
        res.output = "fork() failed";
        return res;
    }

    if (pid == 0) {
        // Child: redirect stdout and stderr to pipe
        close(pipefd[0]);
        dup2(pipefd[1], STDOUT_FILENO);
        dup2(pipefd[1], STDERR_FILENO);
        close(pipefd[1]);

        // Redirect workspace writes to a temp directory so that smoke
        // tests don't clobber the user's real workspace presets.
        setenv("V06C_WORKSPACE_DIR", "/tmp/v06c_test_workspaces", 1);
        // Redirect config.ini to temp directory as well
        setenv("V06C_CONFIG_DIR", "/tmp/v06c_test_workspaces", 1);

        if (extraArg.empty()) {
            execl(binary.c_str(), binary.c_str(), nullptr);
        } else {
            execl(binary.c_str(), binary.c_str(), extraArg.c_str(), nullptr);
        }
        // If execl returns, it failed
        perror("execl");
        _exit(127);
    }

    // Parent
    close(pipefd[1]);  // close write end
    res.started = true;

    // Wait for the specified duration, collecting output
    sleep(runSeconds);

    // Read any output produced so far
    res.output = readAvailable(pipefd[0], 200);

    // Send SIGTERM for clean shutdown
    kill(pid, SIGTERM);

    // Wait for child to exit (with timeout)
    int status = 0;
    int waitResult = 0;
    int waitAttempts = 0;
    do {
        waitResult = waitpid(pid, &status, WNOHANG);
        if (waitResult == 0) {
            usleep(100000);  // 100ms
            waitAttempts++;
            // Read more output while waiting
            std::string more = readAvailable(pipefd[0], 100);
            res.output += more;
        }
    } while (waitResult == 0 && waitAttempts < 50);  // max ~5 seconds

    // If still alive after timeout, force kill
    if (waitResult == 0) {
        kill(pid, SIGKILL);
        waitpid(pid, &status, 0);
    }

    // Read remaining output
    std::string tail = readAvailable(pipefd[0], 200);
    res.output += tail;
    close(pipefd[0]);

    // Analyze exit status
    if (WIFSIGNALED(status)) {
        res.crashed = true;
        res.termSignal = WTERMSIG(status);
        // SIGTERM is expected (we sent it), so don't count as crash
        if (res.termSignal == SIGTERM) {
            res.crashed = false;
        }
    } else if (WIFEXITED(status)) {
        res.exitStatus = WEXITSTATUS(status);
    }

    return res;
}

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

static void test_launchWithoutRom()
{
    TEST("Launch without ROM (no crash)");

    std::string binary = debuggerBinaryPath();
    if (!fileExists(binary)) {
        FAIL("v06c-debugger binary not found");
        return;
    }

    ProcessResult res = spawnDebugger(binary, "", 3);

    if (!res.started) {
        FAIL("failed to spawn process");
        return;
    }

    if (res.crashed) {
        char msg[128];
        snprintf(msg, sizeof(msg), "crashed with signal %d", res.termSignal);
        FAIL(msg);
        return;
    }

    // Check for expected startup output
    if (res.output.find("starting up") == std::string::npos) {
        FAIL("missing 'starting up' in output");
        return;
    }

    PASS();
}

static void test_launchWithTestRom()
{
    TEST("Launch with test ROM (no crash)");

    std::string binary = debuggerBinaryPath();
    if (!fileExists(binary)) {
        FAIL("v06c-debugger binary not found");
        return;
    }

    // Find a test ROM — look relative to the build directory
    // Build dir is same as binary location; testroms/ is at project root
    char buf[4096];
    ssize_t len = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (len <= 0) {
        FAIL("cannot resolve own path");
        return;
    }
    buf[len] = '\0';
    char *slash = strrchr(buf, '/');
    if (slash) *(slash + 1) = '\0';
    std::string buildDir = buf;

    // Navigate from build/ to project root: build/ is at debugger/build/
    // so project root is ../../ from build dir
    std::string romPath = buildDir + "../../testroms/clrs.rom";

    // Verify ROM exists
    if (!fileExists(romPath)) {
        // Try alternative: maybe build dir is at root
        romPath = buildDir + "testroms/clrs.rom";
    }
    if (!fileExists(romPath)) {
        // Try from debugger/build/ directly
        romPath = buildDir + "../testroms/clrs.rom";
    }
    if (!fileExists(romPath)) {
        FAIL("test ROM not found (tried multiple paths)");
        return;
    }

    ProcessResult res = spawnDebugger(binary, romPath, 3);

    if (!res.started) {
        FAIL("failed to spawn process");
        return;
    }

    if (res.crashed) {
        char msg[128];
        snprintf(msg, sizeof(msg), "crashed with signal %d", res.termSignal);
        FAIL(msg);
        return;
    }

    // Check for expected startup output
    if (res.output.find("starting up") == std::string::npos) {
        FAIL("missing 'starting up' in output");
        return;
    }

    PASS();
}

static void test_cleanShutdown()
{
    TEST("Clean shutdown message present");

    std::string binary = debuggerBinaryPath();
    if (!fileExists(binary)) {
        FAIL("v06c-debugger binary not found");
        return;
    }

    ProcessResult res = spawnDebugger(binary, "", 2);

    if (!res.started) {
        FAIL("failed to spawn process");
        return;
    }

    if (res.crashed) {
        FAIL("process crashed");
        return;
    }

    // After SIGTERM, the app should print shutdown message
    if (res.output.find("shutdown complete") == std::string::npos) {
        FAIL("missing 'shutdown complete' — unclean shutdown");
        return;
    }

    PASS();
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------

int main()
{
    printf("=== GUI Smoke Tests ===\n\n");
    printf("  Binary: %s\n\n", debuggerBinaryPath().c_str());

    test_launchWithoutRom();
    test_launchWithTestRom();
    test_cleanShutdown();

    printf("\n=== Results: %d/%d passed", tests_passed, tests_run);
    if (tests_failed > 0) {
        printf(", %d FAILED", tests_failed);
    }
    printf(" ===\n");

    return tests_failed > 0 ? 1 : 0;
}
