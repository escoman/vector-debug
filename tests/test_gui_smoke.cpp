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
// On Windows the debugger is spawned via CreateProcess; the graceful-shutdown
// step posts WM_CLOSE to its window (SDL maps it to SDL_QUIT ≡ SIGTERM here).

#include "test_tmp_dir.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#else
#include <csignal>
#include <unistd.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <poll.h>
#endif

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
#ifdef _WIN32
    char buf[4096];
    DWORD len = GetModuleFileNameA(nullptr, buf, sizeof(buf) - 1);
    if (len == 0) return "";
    buf[len] = '\0';

    // Strip executable name to get directory (accept either separator)
    char *slash = strrchr(buf, '\\');
    char *fwd  = strrchr(buf, '/');
    if (fwd && (!slash || fwd > slash)) slash = fwd;
    if (slash) *(slash + 1) = '\0';

    return std::string(buf) + "v06c-debugger.exe";
#else
    char buf[4096];
    ssize_t len = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (len <= 0) return "";
    buf[len] = '\0';

    // Strip executable name to get directory
    char *slash = strrchr(buf, '/');
    if (slash) *(slash + 1) = '\0';

    return std::string(buf) + "v06c-debugger";
#endif
}

static bool fileExists(const std::string &path)
{
#ifdef _WIN32
    return GetFileAttributesA(path.c_str()) != INVALID_FILE_ATTRIBUTES;
#else
    struct stat st;
    return stat(path.c_str(), &st) == 0;
#endif
}

// Read available output from the capture pipe into a string (non-blocking).
#ifdef _WIN32
static std::string readAvailable(HANDLE hPipe, int timeout_ms = 500)
{
    std::string result;
    char buf[1024];
    DWORD avail = 0;

    // PeekNamedPipe fails once the write end is closed (child exited) — stop.
    while (PeekNamedPipe(hPipe, nullptr, 0, nullptr, &avail, nullptr) && avail > 0) {
        if (timeout_ms <= 0) break;
        DWORD n = 0;
        if (!ReadFile(hPipe, buf, sizeof(buf) - 1, &n, nullptr) || n == 0) break;
        buf[n] = '\0';
        result += buf;
        timeout_ms = 100;  // subsequent reads: shorter wait
        Sleep(10);
    }
    return result;
}
#else
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
#endif

struct ProcessResult {
    bool started;         // process was spawned successfully
    bool crashed;         // process died from signal (SIGABRT, SIGSEGV, etc.)
    int  exitStatus;      // exit code (if !crashed)
    int  termSignal;      // signal that killed (if crashed)
    std::string output;   // captured stdout+stderr
};

// Spawn v06c-debugger with optional arguments, wait, then send SIGTERM.
// Returns collected output and exit status.
#ifdef _WIN32
// EnumWindows callback: post WM_CLOSE to every visible window of the child
// process. SDL translates the window close into SDL_QUIT, which the GUI main
// loop handles as a graceful shutdown — the Windows equivalent of SIGTERM.
static BOOL CALLBACK closeWindowProc(HWND hwnd, LPARAM lParam)
{
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid == static_cast<DWORD>(lParam) && IsWindowVisible(hwnd))
        PostMessageA(hwnd, WM_CLOSE, 0, 0);
    return TRUE;
}

static ProcessResult spawnDebugger(const std::string &binary,
                                    const std::string &extraArg,
                                    int runSeconds)
{
    ProcessResult res;
    res.started = false;
    res.crashed = false;
    res.exitStatus = -1;
    res.termSignal = 0;

    // Inheritable pipe capturing the child's stdout+stderr
    SECURITY_ATTRIBUTES sa = {};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    HANDLE hPipeR = nullptr, hPipeW = nullptr;
    if (!CreatePipe(&hPipeR, &hPipeW, &sa, 0)) {
        res.output = "CreatePipe() failed";
        return res;
    }
    SetHandleInformation(hPipeR, HANDLE_FLAG_INHERIT, 0);  // read end: parent-only

    // Redirect workspace/config writes to a temp directory so that smoke
    // tests don't clobber the user's real workspace presets.
    const std::string tmpDir = testTmpPath("v06c_test_workspaces");
    testMakeDir(tmpDir);

    // Build a copy of the current environment with the overrides appended
    // (CreateProcessA takes an explicit ANSI environment block).
    std::vector<std::string> env;
    for (char *p = GetEnvironmentStringsA(); p && *p; p += strlen(p) + 1)
        env.emplace_back(p);
    auto setVar = [&env](const char *name, const std::string &value) {
        std::string kv = std::string(name) + "=" + value;
        for (auto &e : env)
            if (_stricmp(e.c_str(), kv.c_str()) == 0) { e = kv; return; }
        env.push_back(kv);
    };
    setVar("V06C_WORKSPACE_DIR", tmpDir);
    setVar("V06C_CONFIG_DIR", tmpDir);
    std::string envBlock;
    for (const auto &e : env) envBlock += e;
    envBlock += '\0';

    std::string cmd = '"' + binary + '"';
    if (!extraArg.empty()) cmd += " \"" + extraArg + '"';
    std::vector<char> cmdBuf(cmd.begin(), cmd.end());
    cmdBuf.push_back('\0');

    STARTUPINFOA si = {};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = hPipeW;
    si.hStdError = hPipeW;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);

    PROCESS_INFORMATION pi = {};
    if (!CreateProcessA(nullptr, cmdBuf.data(), nullptr, nullptr, TRUE,
                        CREATE_NO_WINDOW,
                        envBlock.data(), nullptr, &si, &pi)) {
        char msg[128];
        snprintf(msg, sizeof(msg), "CreateProcess() failed (err=%lu)", GetLastError());
        res.output = msg;
        CloseHandle(hPipeR);
        CloseHandle(hPipeW);
        return res;
    }
    CloseHandle(hPipeW);  // parent's copy — child holds its own
    CloseHandle(pi.hThread);
    res.started = true;

    // Run for the specified duration, draining the pipe so a chatty child
    // never blocks on a full pipe buffer
    for (int i = 0; i < runSeconds * 20; ++i) {
        Sleep(50);
        res.output += readAvailable(hPipeR, 0);
    }

    // Graceful shutdown: WM_CLOSE → SDL_QUIT (≡ SIGTERM on POSIX)
    EnumWindows(closeWindowProc, static_cast<LPARAM>(pi.dwProcessId));

    // Wait up to ~5s for exit, collecting output while waiting
    bool exited = false;
    for (int i = 0; i < 50 && !exited; ++i) {
        exited = (WaitForSingleObject(pi.hProcess, 100) == WAIT_OBJECT_0);
        res.output += readAvailable(hPipeR, 0);
    }
    if (!exited)
        TerminateProcess(pi.hProcess, 1);
    WaitForSingleObject(pi.hProcess, 5000);

    // Drain remaining buffered output
    res.output += readAvailable(hPipeR, 200);

    DWORD exitCode = 0;
    GetExitCodeProcess(pi.hProcess, &exitCode);
    res.exitStatus = static_cast<int>(exitCode);
    // NT statuses (0xC0000000 and above) mean abnormal termination
    // (heap corruption, AV, abort…) — the Windows counterpart of a fatal signal
    if ((exitCode & 0xC0000000) != 0)
        res.crashed = true;

    CloseHandle(hPipeR);
    CloseHandle(pi.hProcess);
    return res;
}
#else
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

        const std::string tmpDir = testTmpPath("v06c_test_workspaces");
        testMakeDir(tmpDir);
        setenv("V06C_WORKSPACE_DIR", tmpDir.c_str(), 1);
        // Redirect config.ini to temp directory as well
        setenv("V06C_CONFIG_DIR", tmpDir.c_str(), 1);

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
#endif

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
#ifdef _WIN32
        snprintf(msg, sizeof(msg), "crashed: exit status 0x%08X", (unsigned)res.exitStatus);
#else
        snprintf(msg, sizeof(msg), "crashed with signal %d", res.termSignal);
#endif
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
    // Build dir is same as binary location; testroms/ now lives in the
    // emulator submodule at vendor/v06c-emu/testroms
    // Derive the build directory from the debugger binary path
    std::string buildDir;
#ifdef _WIN32
    buildDir = binary.substr(0, binary.find_last_of("\\/") + 1);
#else
    char buf[4096];
    ssize_t len = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (len <= 0) {
        FAIL("cannot resolve own path");
        return;
    }
    buf[len] = '\0';
    char *slash = strrchr(buf, '/');
    if (slash) *(slash + 1) = '\0';
    buildDir = buf;
#endif

    // Navigate from build/ to the repo root, then into the emulator submodule:
    // build/ is at <repo>/build/, so testroms is ../vendor/v06c-emu/testroms
    std::string romPath = buildDir + "../vendor/v06c-emu/testroms/clrs.rom";

    // Verify ROM exists
    if (!fileExists(romPath)) {
        // Try alternative: maybe build dir is at repo root
        romPath = buildDir + "vendor/v06c-emu/testroms/clrs.rom";
    }
    if (!fileExists(romPath)) {
        // Try one level deeper (e.g. nested build dir)
        romPath = buildDir + "../../vendor/v06c-emu/testroms/clrs.rom";
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
#ifdef _WIN32
        snprintf(msg, sizeof(msg), "crashed: exit status 0x%08X", (unsigned)res.exitStatus);
#else
        snprintf(msg, sizeof(msg), "crashed with signal %d", res.termSignal);
#endif
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
