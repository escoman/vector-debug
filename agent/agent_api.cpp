#include "agent_api.h"
#include "disassembler.h"
#include "opcode_info.h"
#include "rom_load_address.h"
#include "rdb_controller.h"
#include "png_encoder.h"

#include <algorithm>
#include <cctype>
#include <map>
#include <set>
#include <sstream>
#include <iomanip>
#include <thread>

// ---------------------------------------------------------------------------
// AgentApi implementation — Stage 6.3
//
// All operations delegate to IDebugBackend.  No direct access to Board,
// Memory, CPU, IO, TV, or any emulator internals.
//
// All public operations that can fail return AgentApiResult<T>.
// State-changing operations go through the Backend command protocol.
// ---------------------------------------------------------------------------

AgentApi::AgentApi(IDebugBackend &backend)
    : backend_(backend)
{
}

// ---------------------------------------------------------------------------
// Timer helper
// ---------------------------------------------------------------------------

double AgentApi::elapsedMs(std::chrono::steady_clock::time_point start)
{
    auto now = std::chrono::steady_clock::now();
    return std::chrono::duration<double, std::milli>(now - start).count();
}

// ---------------------------------------------------------------------------
// Execution control (Stage 6.3: AgentApiResult<void>)
// ---------------------------------------------------------------------------

AgentApiResult<void> AgentApi::run()
{
    auto t0 = std::chrono::steady_clock::now();
    backend_.requestRun();
    bool running = !backend_.isPaused();
    log_.record("run", "", running ? "running" : "still paused", elapsedMs(t0));
    return AgentApiResult<void>::ok();
}

AgentApiResult<void> AgentApi::pause()
{
    auto t0 = std::chrono::steady_clock::now();
    backend_.requestPause();
    bool paused = backend_.isPaused();
    log_.record("pause", "", paused ? "paused" : "still running", elapsedMs(t0));
    return AgentApiResult<void>::ok();
}

AgentApiResult<void> AgentApi::step()
{
    auto t0 = std::chrono::steady_clock::now();

    if (!backend_.isPaused()) {
        log_.record("step", "", "not paused", elapsedMs(t0), false, "emulation is not paused");
        return AgentApiResult<void>::fail(
            ErrorCode::NotPaused, "Cannot step: emulation is not paused");
    }

    auto cpuBefore = backend_.getCpuState();
    backend_.stepInstruction();
    auto cpuAfter = backend_.getCpuState();

    std::ostringstream oss;
    oss << "PC: " << std::hex << cpuBefore.pc << " -> " << cpuAfter.pc;
    log_.record("step", "", oss.str(), elapsedMs(t0));
    return AgentApiResult<void>::ok();
}

AgentApiResult<void> AgentApi::reset()
{
    auto t0 = std::chrono::steady_clock::now();
    backend_.requestReset();
    log_.record("reset", "", "done", elapsedMs(t0));
    return AgentApiResult<void>::ok();
}

bool AgentApi::isRunning() const
{
    return !backend_.isPaused();
}

// ---------------------------------------------------------------------------
// CPU state (Stage 6.3: AgentApiResult<CpuState>)
// ---------------------------------------------------------------------------

AgentApiResult<CpuState> AgentApi::getCpuState()
{
    auto t0 = std::chrono::steady_clock::now();
    CpuState cpu = backend_.getCpuState();

    std::ostringstream oss;
    oss << "PC=" << std::hex << cpu.pc << " SP=" << cpu.sp
        << " A=" << (int)cpu.a;
    log_.record("getCpuState", "", oss.str(), elapsedMs(t0));
    return AgentApiResult<CpuState>::ok(std::move(cpu));
}

// ---------------------------------------------------------------------------
// Memory access (Stage 6.3: AgentApiResult + overflow checks)
// ---------------------------------------------------------------------------

AgentApiResult<std::vector<uint8_t>> AgentApi::readMemory(uint16_t address, size_t size)
{
    auto t0 = std::chrono::steady_clock::now();

    // Stage 6.3: overflow check
    uint32_t endAddr = static_cast<uint32_t>(address) + size;
    if (endAddr > 0x10000) {
        log_.record("readMemory", "addr=" + std::to_string(address) + " size=" + std::to_string(size),
                    "range overflow", elapsedMs(t0), false, "address + size exceeds 64K");
        return AgentApiResult<std::vector<uint8_t>>::fail(
            ErrorCode::InvalidRange, "address + size exceeds 64K address space");
    }

    auto snap = backend_.readMemorySnapshot(address, size);

    std::ostringstream oss;
    oss << "addr=" << std::hex << address << " size=" << std::dec << size;
    log_.record("readMemory", oss.str(),
                std::to_string(snap.data.size()) + " bytes",
                elapsedMs(t0));
    return AgentApiResult<std::vector<uint8_t>>::ok(std::move(snap.data));
}

AgentApiResult<void> AgentApi::writeMemory(uint16_t address, const std::vector<uint8_t> &data)
{
    auto t0 = std::chrono::steady_clock::now();

    // Stage 6.3: overflow check
    uint32_t endAddr = static_cast<uint32_t>(address) + data.size();
    if (endAddr > 0x10000) {
        log_.record("writeMemory", "addr=" + std::to_string(address) + " size=" + std::to_string(data.size()),
                    "range overflow", elapsedMs(t0), false, "address + size exceeds 64K");
        return AgentApiResult<void>::fail(
            ErrorCode::InvalidRange, "address + size exceeds 64K address space");
    }

    bool ok = backend_.writeMemory(address, data.data(), data.size());

    std::ostringstream oss;
    oss << "addr=" << std::hex << address << " size=" << std::dec << data.size();
    log_.record("writeMemory", oss.str(), ok ? "ok" : "failed", elapsedMs(t0), ok, ok ? "" : "write failed");

    if (!ok) {
        return AgentApiResult<void>::fail(
            ErrorCode::OperationFailed, "Memory write failed");
    }
    return AgentApiResult<void>::ok();
}

// ---------------------------------------------------------------------------
// I/O ports (Stage 6.1 Iteration 3)
// ---------------------------------------------------------------------------

AgentApiResult<uint8_t> AgentApi::readIo(uint8_t port)
{
    auto t0 = std::chrono::steady_clock::now();
    uint8_t value = backend_.readIoPort(port);

    std::ostringstream oss;
    oss << "port=" << std::hex << static_cast<int>(port);
    log_.record("readIo", oss.str(),
                "value=" + std::to_string(value),
                elapsedMs(t0));
    return AgentApiResult<uint8_t>::ok(value);
}

AgentApiResult<void> AgentApi::writeIo(uint8_t port, uint8_t value)
{
    auto t0 = std::chrono::steady_clock::now();
    auto result = backend_.writeIoPort(port, value);

    std::ostringstream oss;
    oss << "port=" << std::hex << static_cast<int>(port)
        << " value=" << std::hex << static_cast<int>(value);
    log_.record("writeIo", oss.str(),
                result.success ? "ok" : result.error, elapsedMs(t0),
                result.success, result.error);

    if (!result.success) {
        return AgentApiResult<void>::fail(
            ErrorCode::OperationFailed, result.error);
    }
    return AgentApiResult<void>::ok();
}

// ---------------------------------------------------------------------------
// Virtual keyboard injection
//
// Key names map to raw SDL scancodes (the value IDebugBackend::pressKey /
// releaseKey forward to Keyboard::apply_key).  The table mirrors the emulator
// keyboard matrix in src/keyboard.h; scancode numbers are the SDL2 standard
// values, kept literal here so the agent layer stays free of an SDL dependency.
// ---------------------------------------------------------------------------

namespace {

struct KeyTableEntry {
    const char *name;
    int         scancode;
    const char *description;
    bool        modifier;
};

// SDL2 scancodes (standard enum values).
enum {
    SCAN_A = 4,  SCAN_B,  SCAN_C,  SCAN_D,  SCAN_E,  SCAN_F,  SCAN_G,
    SCAN_H,  SCAN_I,  SCAN_J,  SCAN_K,  SCAN_L,  SCAN_M,  SCAN_N,
    SCAN_O,  SCAN_P,  SCAN_Q,  SCAN_R,  SCAN_S,  SCAN_T,  SCAN_U,
    SCAN_V,  SCAN_W,  SCAN_X,  SCAN_Y,  SCAN_Z,
    SCAN_1 = 30, SCAN_2, SCAN_3, SCAN_4, SCAN_5, SCAN_6, SCAN_7,
    SCAN_8, SCAN_9, SCAN_0,
    SCAN_RETURN = 40, SCAN_ESCAPE, SCAN_BACKSPACE, SCAN_TAB, SCAN_SPACE,
    SCAN_MINUS = 45, SCAN_EQUALS, SCAN_LBRACKET, SCAN_RBRACKET,
    SCAN_BACKSLASH, SCAN_SEMICOLON = 51, SCAN_APOSTROPHE, SCAN_GRAVE,
    SCAN_COMMA, SCAN_PERIOD, SCAN_SLASH,
    SCAN_F1 = 58, SCAN_F2, SCAN_F3, SCAN_F4, SCAN_F5, SCAN_F6, SCAN_F7, SCAN_F8,
    SCAN_RIGHT = 79, SCAN_LEFT, SCAN_DOWN, SCAN_UP,
    SCAN_LCTRL = 224, SCAN_LSHIFT, SCAN_LALT,
    SCAN_RCTRL = 228, SCAN_RSHIFT, SCAN_RALT,
};

const KeyTableEntry kKeyTable[] = {
    // Letters (English face / Russian face)
    {"A", SCAN_A, "A", false},
    {"B", SCAN_B, "B", false},
    {"C", SCAN_C, "C", false},
    {"D", SCAN_D, "D", false},
    {"E", SCAN_E, "E", false},
    {"F", SCAN_F, "F", false},
    {"G", SCAN_G, "G", false},
    {"H", SCAN_H, "H", false},
    {"I", SCAN_I, "I", false},
    {"J", SCAN_J, "J", false},
    {"K", SCAN_K, "K", false},
    {"L", SCAN_L, "L", false},
    {"M", SCAN_M, "M", false},
    {"N", SCAN_N, "N", false},
    {"O", SCAN_O, "O", false},
    {"P", SCAN_P, "P", false},
    {"Q", SCAN_Q, "Q", false},
    {"R", SCAN_R, "R", false},
    {"S", SCAN_S, "S", false},
    {"T", SCAN_T, "T", false},
    {"U", SCAN_U, "U", false},
    {"V", SCAN_V, "V", false},
    {"W", SCAN_W, "W", false},
    {"X", SCAN_X, "X", false},
    {"Y", SCAN_Y, "Y", false},
    {"Z", SCAN_Z, "Z", false},
    // Digits
    {"0", SCAN_0, "digit 0", false},
    {"1", SCAN_1, "digit 1", false},
    {"2", SCAN_2, "digit 2", false},
    {"3", SCAN_3, "digit 3", false},
    {"4", SCAN_4, "digit 4", false},
    {"5", SCAN_5, "digit 5", false},
    {"6", SCAN_6, "digit 6", false},
    {"7", SCAN_7, "digit 7", false},
    {"8", SCAN_8, "digit 8", false},
    {"9", SCAN_9, "digit 9", false},
    // Punctuation
    {"MINUS",      SCAN_MINUS,      "- / @",   false},
    {"EQUALS",     SCAN_EQUALS,     "=",       false},
    {"LBRACKET",   SCAN_LBRACKET,   "[",       false},
    {"RBRACKET",   SCAN_RBRACKET,   "]",       false},
    {"BACKSLASH",  SCAN_BACKSLASH,  "\\",      false},
    {"SEMICOLON",  SCAN_SEMICOLON,  ";",       false},
    {"APOSTROPHE", SCAN_APOSTROPHE, "'",       false},
    {"COMMA",      SCAN_COMMA,      ",",       false},
    {"PERIOD",     SCAN_PERIOD,     ".",       false},
    {"SLASH",      SCAN_SLASH,      "/",       false},
    {"GRAVE",      SCAN_GRAVE,      "^",       false},
    // Control keys
    {"SPACE",     SCAN_SPACE,     "space bar", false},
    {"TAB",       SCAN_TAB,       "tab",       false},
    {"ENTER",     SCAN_RETURN,    "enter (VK input)", false},
    {"BACKSPACE", SCAN_BACKSPACE, "backspace / delete", false},
    {"ESCAPE",    SCAN_ESCAPE,    "escape",    false},
    {"UP",        SCAN_UP,        "up arrow",   false},
    {"DOWN",      SCAN_DOWN,      "down arrow", false},
    {"LEFT",      SCAN_LEFT,      "left arrow", false},
    {"RIGHT",     SCAN_RIGHT,     "right arrow", false},
    // Function keys F1-F8 (F6 is the RU/LAT switch pulse)
    {"F1", SCAN_F1, "F1", false},
    {"F2", SCAN_F2, "F2", false},
    {"F3", SCAN_F3, "F3", false},
    {"F4", SCAN_F4, "F4", false},
    {"F5", SCAN_F5, "F5", false},
    {"F6", SCAN_F6, "F6 (RU/LAT switch pulse)", false},
    {"F7", SCAN_F7, "F7", false},
    {"F8", SCAN_F8, "F8", false},
    // Modifier keys (latch until released)
    {"SS",    SCAN_LSHIFT, "shift (SS)",  true},
    {"SHIFT", SCAN_LSHIFT, "alias for SS", true},
    {"US",    SCAN_LCTRL,  "ctrl (US)",   true},
    {"CTRL",  SCAN_LCTRL,  "alias for US", true},
    {"RUS",   SCAN_F6,     "RU/LAT toggle (F6)", true},
    {"PS",    SCAN_RALT,   "matrix key (right-alt slot)", false},
};

std::string upperKey(const std::string &s)
{
    std::string r = s;
    std::transform(r.begin(), r.end(), r.begin(),
                   [](unsigned char c) { return std::toupper(c); });
    return r;
}

// Resolve a key name to its table entry (nullptr when unknown).
const KeyTableEntry *findKey(const std::string &keyName)
{
    const std::string want = upperKey(keyName);
    for (const auto &e : kKeyTable) {
        if (want == e.name) return &e;
    }
    return nullptr;
}

} // namespace

AgentApiResult<void> AgentApi::pressKey(const std::string &keyName)
{
    auto t0 = std::chrono::steady_clock::now();
    const KeyTableEntry *e = findKey(keyName);
    if (!e) {
        log_.record("pressKey", keyName, "unknown key", elapsedMs(t0),
                    false, "unknown key");
        return AgentApiResult<void>::fail(
            ErrorCode::InvalidArgument, "unknown key: " + keyName);
    }
    backend_.pressKey(e->scancode);
    log_.record("pressKey", keyName, "ok", elapsedMs(t0));
    return AgentApiResult<void>::ok();
}

AgentApiResult<void> AgentApi::releaseKey(const std::string &keyName)
{
    auto t0 = std::chrono::steady_clock::now();
    const KeyTableEntry *e = findKey(keyName);
    if (!e) {
        log_.record("releaseKey", keyName, "unknown key", elapsedMs(t0),
                    false, "unknown key");
        return AgentApiResult<void>::fail(
            ErrorCode::InvalidArgument, "unknown key: " + keyName);
    }
    backend_.releaseKey(e->scancode);
    log_.record("releaseKey", keyName, "ok", elapsedMs(t0));
    return AgentApiResult<void>::ok();
}

AgentApiResult<void> AgentApi::typeKey(const std::string &keyName)
{
    auto t0 = std::chrono::steady_clock::now();
    const KeyTableEntry *e = findKey(keyName);
    if (!e) {
        log_.record("typeKey", keyName, "unknown key", elapsedMs(t0),
                    false, "unknown key");
        return AgentApiResult<void>::fail(
            ErrorCode::InvalidArgument, "unknown key: " + keyName);
    }

    // The ROM only samples the matrix while the CPU runs and polls the
    // keyboard port; a press is invisible when paused.  Warn but still latch
    // the key so it registers once execution resumes.
    bool running = !backend_.isPaused();

    backend_.pressKey(e->scancode);
    // Hold long enough for the ROM's periodic keyboard scan to observe it
    // (~6 frames at 50 Hz).  Real-time sleep; the emulation thread advances
    // independently and reads the port during that window.
    std::this_thread::sleep_for(std::chrono::milliseconds(120));
    backend_.releaseKey(e->scancode);

    std::string note = running ? "ok" : "ok (was paused: ROM did not poll)";
    log_.record("typeKey", keyName, note, elapsedMs(t0));
    return AgentApiResult<void>::ok();
}

AgentApiResult<std::vector<KeyboardKeyInfo>> AgentApi::listKeys()
{
    auto t0 = std::chrono::steady_clock::now();
    std::vector<KeyboardKeyInfo> keys;
    keys.reserve(sizeof(kKeyTable) / sizeof(kKeyTable[0]));
    for (const auto &e : kKeyTable) {
        KeyboardKeyInfo ki;
        ki.name        = e.name;
        ki.scancode    = e.scancode;
        ki.description = e.description;
        ki.modifier    = e.modifier;
        keys.push_back(ki);
    }
    log_.record("listKeys", "", std::to_string(keys.size()) + " keys", elapsedMs(t0));
    return AgentApiResult<std::vector<KeyboardKeyInfo>>::ok(std::move(keys));
}

// ---------------------------------------------------------------------------
// Breakpoints (through command protocol)
// ---------------------------------------------------------------------------

AgentApiResult<void> AgentApi::setBreakpoint(uint16_t address)
{
    auto t0 = std::chrono::steady_clock::now();
    auto result = backend_.requestAddBreakpoint(address);

    std::ostringstream oss;
    oss << "addr=" << std::hex << address;
    log_.record("setBreakpoint", oss.str(),
                result.success ? "ok" : result.error, elapsedMs(t0),
                result.success, result.error);

    if (!result.success) {
        return AgentApiResult<void>::fail(
            ErrorCode::OperationFailed, result.error);
    }
    return AgentApiResult<void>::ok();
}

AgentApiResult<void> AgentApi::clearBreakpoint(uint16_t address)
{
    auto t0 = std::chrono::steady_clock::now();
    auto result = backend_.requestRemoveBreakpoint(address);

    std::ostringstream oss;
    oss << "addr=" << std::hex << address;
    log_.record("clearBreakpoint", oss.str(),
                result.success ? "removed" : result.error, elapsedMs(t0),
                result.success, result.error);

    if (!result.success) {
        return AgentApiResult<void>::fail(
            ErrorCode::NotFound, result.error);
    }
    return AgentApiResult<void>::ok();
}

AgentApiResult<void> AgentApi::setBreakpointEnabled(uint16_t address, bool enabled)
{
    auto t0 = std::chrono::steady_clock::now();
    auto result = backend_.requestSetBreakpointEnabled(address, enabled);

    std::ostringstream oss;
    oss << "addr=" << std::hex << address << " enabled=" << enabled;
    log_.record("setBreakpointEnabled", oss.str(),
                result.success ? "ok" : result.error, elapsedMs(t0),
                result.success, result.error);

    if (!result.success) {
        return AgentApiResult<void>::fail(
            ErrorCode::NotFound, result.error);
    }
    return AgentApiResult<void>::ok();
}

AgentApiResult<std::vector<DebuggerBreakpoint>> AgentApi::listBreakpoints()
{
    auto t0 = std::chrono::steady_clock::now();
    auto bps = backend_.getBreakpoints();
    log_.record("listBreakpoints", "",
                std::to_string(bps.size()) + " breakpoints",
                elapsedMs(t0));
    return AgentApiResult<std::vector<DebuggerBreakpoint>>::ok(std::move(bps));
}

AgentApiResult<void> AgentApi::clearAllBreakpoints()
{
    auto t0 = std::chrono::steady_clock::now();
    auto result = backend_.requestClearBreakpoints();

    log_.record("clearAllBreakpoints", "",
                result.success ? "cleared" : result.error,
                elapsedMs(t0), result.success, result.error);

    if (!result.success) {
        return AgentApiResult<void>::fail(
            ErrorCode::OperationFailed, result.error);
    }
    return AgentApiResult<void>::ok();
}

// ---------------------------------------------------------------------------
// Registers (Stage 6.1 §8)
// ---------------------------------------------------------------------------

AgentApiResult<void> AgentApi::setRegister(const std::string &name, uint16_t value)
{
    auto t0 = std::chrono::steady_clock::now();

    // Map register name → (RegisterId, high/low/whole)
    // Pairs AF, BC, DE, HL: high byte = first reg, low byte = second reg.
    struct RegMapping {
        IDebugBackend::RegisterId pair;
        enum { High, Low, Whole } half;
    };

    static const std::map<std::string, RegMapping> regMap = {
        {"A",  {IDebugBackend::RegisterId::AF, RegMapping::High}},
        {"F",  {IDebugBackend::RegisterId::AF, RegMapping::Low}},
        {"B",  {IDebugBackend::RegisterId::BC, RegMapping::High}},
        {"C",  {IDebugBackend::RegisterId::BC, RegMapping::Low}},
        {"D",  {IDebugBackend::RegisterId::DE, RegMapping::High}},
        {"E",  {IDebugBackend::RegisterId::DE, RegMapping::Low}},
        {"H",  {IDebugBackend::RegisterId::HL, RegMapping::High}},
        {"L",  {IDebugBackend::RegisterId::HL, RegMapping::Low}},
        {"PC", {IDebugBackend::RegisterId::PC, RegMapping::Whole}},
        {"SP", {IDebugBackend::RegisterId::SP, RegMapping::Whole}},
    };

    auto it = regMap.find(name);
    if (it == regMap.end()) {
        log_.record("setRegister", "name=" + name,
                     "unknown register", elapsedMs(t0), false, "unknown register");
        return AgentApiResult<void>::fail(
            ErrorCode::InvalidArgument,
            "Unknown register: " + name);
    }

    const auto &m = it->second;
    uint16_t writeValue = value;

    if (m.half != RegMapping::Whole) {
        // Read current pair, modify one half, write back
        CpuState cpu = backend_.getCpuState();
        uint16_t current = 0;
        switch (m.pair) {
            case IDebugBackend::RegisterId::AF: current = (static_cast<uint16_t>(cpu.a) << 8) | cpu.flags; break;
            case IDebugBackend::RegisterId::BC: current = (static_cast<uint16_t>(cpu.b) << 8) | cpu.c; break;
            case IDebugBackend::RegisterId::DE: current = (static_cast<uint16_t>(cpu.d) << 8) | cpu.e; break;
            case IDebugBackend::RegisterId::HL: current = (static_cast<uint16_t>(cpu.h) << 8) | cpu.l; break;
            default: break;
        }
        if (m.half == RegMapping::High) {
            writeValue = (value << 8) | (current & 0xFF);
        } else {
            writeValue = (current & 0xFF00) | (value & 0xFF);
        }
    }

    bool ok = backend_.writeRegister(m.pair, writeValue);

    std::ostringstream oss;
    oss << name << "=" << std::hex << value;
    log_.record("setRegister", oss.str(), ok ? "ok" : "failed",
                elapsedMs(t0), ok, ok ? "" : "write failed");

    if (!ok) {
        return AgentApiResult<void>::fail(
            ErrorCode::OperationFailed, "Register write failed");
    }
    return AgentApiResult<void>::ok();
}

// ---------------------------------------------------------------------------
// Disassembly (Stage 6.1 §10)
// ---------------------------------------------------------------------------

AgentApiResult<std::vector<DisassembledInstructionResult>>
AgentApi::disassemble(uint16_t address, size_t count)
{
    auto t0 = std::chrono::steady_clock::now();

    if (count == 0) {
        log_.record("disassemble", "count=0", "invalid count",
                    elapsedMs(t0), false, "count must be > 0");
        return AgentApiResult<std::vector<DisassembledInstructionResult>>::fail(
            ErrorCode::InvalidArgument, "count must be > 0");
    }

    // Stage 6.3: safe maximum
    if (count > AgentLimits::MAX_DISASSEMBLY_COUNT) {
        count = AgentLimits::MAX_DISASSEMBLY_COUNT;
    }

    auto readByte = [this](uint16_t addr) -> uint8_t {
        return backend_.readMemory(addr);
    };

    std::vector<DisassembledInstructionResult> result;
    result.reserve(count);

    uint16_t pc = address;
    for (size_t i = 0; i < count; ++i) {
        DisassembledInstruction di = ::disassemble(pc, readByte);

        DisassembledInstructionResult entry;
        entry.address = di.address;
        entry.next_address = pc + di.length;
        entry.bytes.assign(di.bytes.begin(), di.bytes.begin() + di.length);
        entry.mnemonic = di.mnemonic;
        entry.operands = di.operands;
        entry.text = di.text;
        result.push_back(std::move(entry));

        uint16_t nextPc = pc + di.length;
        if (nextPc <= pc) break;  // overflow guard
        pc = nextPc;
    }

    std::ostringstream oss;
    oss << "addr=" << std::hex << address << " count=" << std::dec << count;
    log_.record("disassemble", oss.str(),
                std::to_string(result.size()) + " instructions",
                elapsedMs(t0));

    return AgentApiResult<std::vector<DisassembledInstructionResult>>::ok(
        std::move(result));
}

// ---------------------------------------------------------------------------
// Code Analysis (Stage 6.16 — control-flow analysis via existing disassembler)
// ---------------------------------------------------------------------------

AgentApiResult<CodeAnalysisResult>
AgentApi::analyzeCode(uint16_t startAddress, size_t maxInstructions)
{
    auto t0 = std::chrono::steady_clock::now();

    if (maxInstructions == 0) {
        log_.record("analyzeCode", "max=0", "invalid",
                    elapsedMs(t0), false, "maxInstructions must be > 0");
        return AgentApiResult<CodeAnalysisResult>::fail(
            ErrorCode::InvalidArgument, "maxInstructions must be > 0");
    }

    if (maxInstructions > AgentLimits::MAX_CODE_ANALYSIS_INSTRUCTIONS) {
        maxInstructions = AgentLimits::MAX_CODE_ANALYSIS_INSTRUCTIONS;
    }

    auto readByte = [this](uint16_t addr) -> uint8_t {
        return backend_.readMemory(addr);
    };

    CodeAnalysisResult analysis = ::analyzeCode(startAddress, readByte, maxInstructions);

    std::ostringstream oss;
    oss << "entry=" << std::hex << startAddress
        << " instructions=" << std::dec << analysis.instructionCount
        << " ranges=" << analysis.ranges.size()
        << " refs=" << analysis.references.size()
        << " conflicts=" << analysis.conflicts.size();
    if (analysis.truncated) oss << " TRUNCATED";
    log_.record("analyzeCode", oss.str(),
                std::to_string(analysis.instructionCount) + " instructions",
                elapsedMs(t0));

    return AgentApiResult<CodeAnalysisResult>::ok(std::move(analysis));
}

// ---------------------------------------------------------------------------
// Code Analysis — Multi-Entry (Stage 6.19)
// ---------------------------------------------------------------------------

AgentApiResult<CodeAnalysisResult>
AgentApi::analyzeCode(const std::vector<uint16_t>& entryPoints, size_t maxInstructions)
{
    auto t0 = std::chrono::steady_clock::now();

    if (entryPoints.empty()) {
        log_.record("analyzeCodeMulti", "empty", "invalid",
                    elapsedMs(t0), false, "entryPoints must not be empty");
        return AgentApiResult<CodeAnalysisResult>::fail(
            ErrorCode::InvalidArgument, "entryPoints must not be empty");
    }

    if (maxInstructions == 0) {
        log_.record("analyzeCodeMulti", "max=0", "invalid",
                    elapsedMs(t0), false, "maxInstructions must be > 0");
        return AgentApiResult<CodeAnalysisResult>::fail(
            ErrorCode::InvalidArgument, "maxInstructions must be > 0");
    }

    if (maxInstructions > AgentLimits::MAX_CODE_ANALYSIS_INSTRUCTIONS) {
        maxInstructions = AgentLimits::MAX_CODE_ANALYSIS_INSTRUCTIONS;
    }

    if (entryPoints.size() > AgentLimits::MAX_ANALYSIS_ENTRY_POINTS) {
        log_.record("analyzeCodeMulti", "too many entries",
                    "invalid", elapsedMs(t0), false,
                    "entryPoints count " + std::to_string(entryPoints.size()) +
                    " exceeds limit " +
                    std::to_string(AgentLimits::MAX_ANALYSIS_ENTRY_POINTS));
        return AgentApiResult<CodeAnalysisResult>::fail(
            ErrorCode::InvalidArgument,
            "entryPoints count exceeds limit (" +
            std::to_string(AgentLimits::MAX_ANALYSIS_ENTRY_POINTS) + ")");
    }

    auto readByte = [this](uint16_t addr) -> uint8_t {
        return backend_.readMemory(addr);
    };

    CodeAnalysisResult analysis = ::analyzeCodeMulti(entryPoints, readByte, maxInstructions);

    std::ostringstream oss;
    oss << "entries=" << std::dec << analysis.entryPoints.size()
        << " instructions=" << analysis.instructionCount
        << " ranges=" << analysis.ranges.size()
        << " codeBytes=" << analysis.codeBytes
        << " refs=" << analysis.references.size()
        << " conflicts=" << analysis.conflicts.size();
    if (analysis.truncated) oss << " TRUNCATED";
    log_.record("analyzeCodeMulti", oss.str(),
                std::to_string(analysis.instructionCount) + " instructions",
                elapsedMs(t0));

    return AgentApiResult<CodeAnalysisResult>::ok(std::move(analysis));
}

// ---------------------------------------------------------------------------
// Range Disassembly (Stage 6.18)
// ---------------------------------------------------------------------------

AgentApiResult<DisassembleRangeResult>
AgentApi::disassembleRange(uint16_t address, uint16_t size)
{
    auto t0 = std::chrono::steady_clock::now();

    // Validate size
    if (size == 0) {
        log_.record("disassembleRange", "size=0", "invalid",
                    elapsedMs(t0), false, "size must be > 0");
        return AgentApiResult<DisassembleRangeResult>::fail(
            ErrorCode::InvalidArgument, "size must be > 0");
    }

    // Check maximum range
    if (size > AgentLimits::MAX_MEMORY_READ_RANGE) {
        log_.record("disassembleRange", 
                    "size=" + std::to_string(size), "invalid",
                    elapsedMs(t0), false, "size exceeds maximum");
        return AgentApiResult<DisassembleRangeResult>::fail(
            ErrorCode::InvalidRange, 
            "size exceeds maximum (" + std::to_string(AgentLimits::MAX_MEMORY_READ_RANGE) + ")");
    }

    // Check address wrap-around
    uint32_t endAddr = static_cast<uint32_t>(address) + size;
    if (endAddr > 0x10000) {
        log_.record("disassembleRange",
                    "address+size > 64K", "invalid",
                    elapsedMs(t0), false, "address wrap-around");
        return AgentApiResult<DisassembleRangeResult>::fail(
            ErrorCode::InvalidRange,
            "address + size exceeds 64K address space");
    }

    DisassembleRangeResult result = linearDisassemble(address, size);
    result.startAddress = address;
    result.size = static_cast<uint16_t>(size);

    std::ostringstream oss;
    oss << "address=" << std::hex << address
        << " size=" << std::hex << size
        << " instructions=" << std::dec << result.instructions.size();
    if (result.incomplete_instruction) oss << " INCOMPLETE";
    log_.record("disassembleRange", oss.str(),
                std::to_string(result.instructions.size()) + " instructions",
                elapsedMs(t0));

    return AgentApiResult<DisassembleRangeResult>::ok(std::move(result));
}

// ---------------------------------------------------------------------------
// Shared linear sweep (Stage 6.18 / Stage 6.26)
//
// Used verbatim by disassembleRange() and disassembleImage() so that the
// batch result is bit-identical to the single-range result over the same
// range.  No validation here — callers guarantee size fits the space.
// ---------------------------------------------------------------------------

DisassembleRangeResult
AgentApi::linearDisassemble(uint16_t address, uint32_t size)
{
    DisassembleRangeResult result;
    result.startAddress = address;
    result.size = static_cast<uint16_t>(size <= 0xFFFF ? size : 0xFFFF);

    auto readByte = [this](uint16_t addr) -> uint8_t {
        return backend_.readMemory(addr);
    };

    uint32_t currentAddr = address;
    uint32_t rangeEnd = static_cast<uint32_t>(address) + size;

    // Sequential disassembly through the range
    while (currentAddr < rangeEnd) {
        DisassembledInstruction di = ::disassemble(static_cast<uint16_t>(currentAddr), readByte);

        // Check if instruction fits completely in range
        uint32_t instrEnd = currentAddr + di.length;
        if (instrEnd > rangeEnd) {
            // Instruction extends beyond range
            result.incomplete_instruction = true;
            break;
        }

        // Convert to result format
        DisassembledRangeInstruction ri;
        ri.address = di.address;
        ri.bytes.assign(di.bytes.begin(), di.bytes.begin() + di.length);
        ri.mnemonic = di.mnemonic;
        ri.operands = di.operands;
        ri.size = di.length;
        // Determine branch type and target
        ControlFlowType cft = classifyControlFlow(di.opcode);
        switch (cft) {
        case ControlFlowType::UnconditionalJmp:
            ri.branch_type = "JMP";
            ri.branch_target = di.target;
            break;
        case ControlFlowType::ConditionalJmp:
            ri.branch_type = "JCC";
            ri.branch_target = di.target;
            break;
        case ControlFlowType::UnconditionalCall:
            ri.branch_type = "CALL";
            ri.branch_target = di.target;
            break;
        case ControlFlowType::ConditionalCall:
            // Stage 6.22 §7: не путать с безусловным CALL
            ri.branch_type = "CALLCC";
            ri.branch_target = di.target;
            break;
        case ControlFlowType::UnconditionalRet:
            ri.branch_type = "RET";
            ri.branch_target = std::nullopt;  // RET has no static target
            break;
        case ControlFlowType::ConditionalRet:
            ri.branch_type = "RET";
            ri.branch_target = std::nullopt;
            break;
        case ControlFlowType::Restart:
            ri.branch_type = "RST";
            ri.branch_target = di.target;
            break;
        default:
            // Sequential instruction — no branch
            break;
        }

        result.instructions.push_back(std::move(ri));
        currentAddr = instrEnd;
    }

    return result;
}

// ---------------------------------------------------------------------------
// Batch analysis (Stage 6.26)
// ---------------------------------------------------------------------------

AgentApiResult<DisassembleRangeResult>
AgentApi::disassembleImage(uint16_t address, uint32_t length)
{
    auto t0 = std::chrono::steady_clock::now();

    if (length == 0) {
        log_.record("disassembleImage", "length=0", "invalid",
                    elapsedMs(t0), false, "length must be > 0");
        return AgentApiResult<DisassembleRangeResult>::fail(
            ErrorCode::InvalidArgument, "length must be > 0");
    }

    if (length > AgentLimits::MAX_DISASSEMBLE_IMAGE_LENGTH) {
        log_.record("disassembleImage", "length=" + std::to_string(length), "invalid",
                    elapsedMs(t0), false, "length exceeds maximum");
        return AgentApiResult<DisassembleRangeResult>::fail(
            ErrorCode::InvalidRange,
            "length exceeds maximum (" +
            std::to_string(AgentLimits::MAX_DISASSEMBLE_IMAGE_LENGTH) + ")");
    }

    uint32_t endAddr = static_cast<uint32_t>(address) + length;
    if (endAddr > 0x10000) {
        log_.record("disassembleImage", "address+length > 64K", "invalid",
                    elapsedMs(t0), false, "address wrap-around");
        return AgentApiResult<DisassembleRangeResult>::fail(
            ErrorCode::InvalidRange,
            "address + length exceeds 64K address space");
    }

    DisassembleRangeResult result = linearDisassemble(address, length);

    // §16: never silently truncate — a limit overflow is a hard error.
    if (result.instructions.size() > AgentLimits::MAX_DISASSEMBLE_IMAGE_INSTRUCTIONS) {
        log_.record("disassembleImage",
                    "address=" + std::to_string(address) + " length=" + std::to_string(length),
                    "limit", elapsedMs(t0), false, "instruction count exceeds limit");
        return AgentApiResult<DisassembleRangeResult>::fail(
            ErrorCode::LimitExceeded,
            "instruction count " + std::to_string(result.instructions.size()) +
            " exceeds limit (" +
            std::to_string(AgentLimits::MAX_DISASSEMBLE_IMAGE_INSTRUCTIONS) +
            "); use a smaller range");
    }

    std::ostringstream oss;
    oss << "address=" << std::hex << address
        << " length=" << std::hex << length
        << " instructions=" << std::dec << result.instructions.size();
    if (result.incomplete_instruction) oss << " INCOMPLETE";
    log_.record("disassembleImage", oss.str(),
                std::to_string(result.instructions.size()) + " instructions",
                elapsedMs(t0));

    return AgentApiResult<DisassembleRangeResult>::ok(std::move(result));
}

AgentApiResult<CoverageReportResult>
AgentApi::coverageReport(const std::vector<uint16_t> &entryPoints,
                         uint16_t imageStart, uint32_t imageLength,
                         size_t maxInstructions)
{
    auto t0 = std::chrono::steady_clock::now();

    if (entryPoints.empty()) {
        log_.record("coverageReport", "no entries", "invalid",
                    elapsedMs(t0), false, "at least one entry point required");
        return AgentApiResult<CoverageReportResult>::fail(
            ErrorCode::InvalidArgument, "at least one entry point is required");
    }

    if (imageLength == 0 ||
        static_cast<uint32_t>(imageStart) + imageLength > 0x10000) {
        log_.record("coverageReport", "bad image range", "invalid",
                    elapsedMs(t0), false, "invalid image range");
        return AgentApiResult<CoverageReportResult>::fail(
            ErrorCode::InvalidRange, "invalid image range");
    }

    if (maxInstructions == 0 ||
        maxInstructions > AgentLimits::MAX_CODE_ANALYSIS_INSTRUCTIONS) {
        log_.record("coverageReport", "max=" + std::to_string(maxInstructions), "invalid",
                    elapsedMs(t0), false, "maxInstructions out of range");
        return AgentApiResult<CoverageReportResult>::fail(
            ErrorCode::InvalidArgument,
            "maxInstructions must be 1.." +
            std::to_string(AgentLimits::MAX_CODE_ANALYSIS_INSTRUCTIONS));
    }

    // §15: single internal entry-point form — reuse the existing analyzer.
    auto analysis = analyzeCode(entryPoints, maxInstructions);
    if (!analysis.success) {
        return AgentApiResult<CoverageReportResult>::fail(
            analysis.error_code, analysis.error_message);
    }

    const uint32_t imgEnd = static_cast<uint32_t>(imageStart) + imageLength; // exclusive

    CoverageReportResult result;
    result.imageStart = imageStart;
    result.imageEnd   = static_cast<uint16_t>(imgEnd - 1);                  // inclusive
    result.imageBytes = imageLength;
    result.truncated  = analysis.value.truncated;
    result.instructionCount = analysis.value.instructionCount;

    // Byte-level coverage bitmap over the image range (deterministic).
    std::vector<bool> covered(imageLength, false);
    for (const auto &inst : analysis.value.instructions) {
        uint32_t s = inst.address;
        uint32_t e = s + inst.size;                     // exclusive
        if (e > 0x10000) continue;                      // wrapped — ignore
        for (uint32_t a = s; a < e; ++a) {
            if (a >= imageStart && a < imgEnd) {
                if (!covered[a - imageStart]) {
                    covered[a - imageStart] = true;
                    ++result.codeBytes;
                }
            }
        }
    }

    // Merged code ranges (ascending, inclusive end) from the bitmap.
    {
        size_t i = 0;
        while (i < imageLength) {
            if (!covered[i]) { ++i; continue; }
            CoverageRange rng;
            rng.start = static_cast<uint16_t>(imageStart + i);
            while (i < imageLength && covered[i]) ++i;
            rng.end = static_cast<uint16_t>(imageStart + i - 1);
            result.codeRanges.push_back(rng);
        }
    }

    // Uncovered gaps (ascending, inclusive end).
    {
        size_t i = 0;
        while (i < imageLength) {
            if (covered[i]) { ++i; continue; }
            CoverageRange rng;
            rng.start = static_cast<uint16_t>(imageStart + i);
            while (i < imageLength && !covered[i]) ++i;
            rng.end = static_cast<uint16_t>(imageStart + i - 1);
            result.uncoveredRanges.push_back(rng);
        }
    }

    // Branch targets — deterministic order (from, then to).
    result.branchTargets.reserve(analysis.value.references.size());
    for (const auto &ref : analysis.value.references) {
        result.branchTargets.push_back({ref.from, ref.to, ref.type});
    }
    std::sort(result.branchTargets.begin(), result.branchTargets.end(),
              [](const CoverageBranchTarget &a, const CoverageBranchTarget &b) {
                  return (a.from != b.from) ? (a.from < b.from) : (a.to < b.to);
              });

    // §6.2: JCC blind spots — targets that exist but have no analyzed code.
    {
        std::set<uint16_t> uncovered;
        for (const auto &bt : result.branchTargets) {
            bool isCovered = (bt.to >= imageStart &&
                              static_cast<uint32_t>(bt.to) < imgEnd &&
                              covered[bt.to - imageStart]);
            if (!isCovered) uncovered.insert(bt.to);
        }
        result.uncoveredBranchTargets.assign(uncovered.begin(), uncovered.end());
    }

    std::ostringstream oss;
    oss << "entries=" << std::dec << analysis.value.entryPoints.size()
        << " image=" << std::hex << imageStart << ".." << (imgEnd - 1)
        << " codeBytes=" << std::dec << result.codeBytes
        << "/" << result.imageBytes
        << " gaps=" << result.uncoveredRanges.size()
        << " uncoveredTargets=" << result.uncoveredBranchTargets.size();
    if (result.truncated) oss << " TRUNCATED";
    log_.record("coverageReport", oss.str(),
                std::to_string(result.codeBytes) + " covered bytes", elapsedMs(t0));

    return AgentApiResult<CoverageReportResult>::ok(std::move(result));
}

// ---------------------------------------------------------------------------
// Complete memory diff (Stage 6.26)
//
// Guarantees a full byte-for-byte diff over the requested range with old/new
// values — unlike compareMemorySnapshots(), which only reports merged range
// boundaries.  A dense diff that cannot fit the limits is a hard error
// (LimitExceeded), never a silent under-report.
// ---------------------------------------------------------------------------

AgentApiResult<MemoryDiffResult>
AgentApi::diffMemorySnapshots(uint32_t idA, uint32_t idB,
                              uint16_t start, uint32_t length)
{
    auto t0 = std::chrono::steady_clock::now();

    if (length == 0 || static_cast<uint32_t>(start) + length > 0x10000) {
        log_.record("diffMemorySnapshots", "bad range", "invalid",
                    elapsedMs(t0), false, "invalid diff range");
        return AgentApiResult<MemoryDiffResult>::fail(
            ErrorCode::InvalidRange, "invalid diff range");
    }

    auto snapA = backend_.getMemorySnapshot(idA);
    auto snapB = backend_.getMemorySnapshot(idB);
    if (snapA.data.empty() || snapB.data.empty()) {
        log_.record("diffMemorySnapshots",
                    "a=" + std::to_string(idA) + " b=" + std::to_string(idB),
                    "not found", elapsedMs(t0), false, "snapshot not found");
        return AgentApiResult<MemoryDiffResult>::fail(
            ErrorCode::NotFound, "One or both snapshots not found or invalidated");
    }

    auto covers = [&](const MemorySnapshotData &s) {
        uint32_t sStart = s.start_address;
        uint32_t sEnd = sStart + static_cast<uint32_t>(s.data.size());
        return static_cast<uint32_t>(start) >= sStart &&
               static_cast<uint32_t>(start) + length <= sEnd;
    };
    if (!covers(snapA) || !covers(snapB)) {
        log_.record("diffMemorySnapshots", "range outside snapshots", "invalid",
                    elapsedMs(t0), false, "range not covered by both snapshots");
        return AgentApiResult<MemoryDiffResult>::fail(
            ErrorCode::InvalidRange,
            "requested range is not covered by both snapshots");
    }

    const uint32_t offA = static_cast<uint32_t>(start) - snapA.start_address;
    const uint32_t offB = static_cast<uint32_t>(start) - snapB.start_address;

    MemoryDiffResult result;
    result.start = start;
    result.length = length;

    size_t i = 0;
    while (i < length) {
        uint8_t a = snapA.data[offA + i];
        uint8_t b = snapB.data[offB + i];
        if (a == b) { ++i; continue; }

        MemoryDiffRange rng;
        rng.address = static_cast<uint16_t>(start + i);
        size_t j = i;
        while (j < length &&
               snapA.data[offA + j] != snapB.data[offB + j]) {
            rng.oldBytes.push_back(snapA.data[offA + j]);
            rng.newBytes.push_back(snapB.data[offB + j]);
            ++j;
        }
        result.changedBytes += rng.oldBytes.size();
        result.ranges.push_back(std::move(rng));
        i = j;

        // §16: limits are hard errors, never silent cuts.
        if (result.ranges.size() > AgentLimits::MAX_DIFF_CHANGED_RANGES ||
            result.changedBytes > AgentLimits::MAX_DIFF_CHANGED_BYTES) {
            log_.record("diffMemorySnapshots",
                        "a=" + std::to_string(idA) + " b=" + std::to_string(idB),
                        "limit", elapsedMs(t0), false, "diff exceeds limits");
            return AgentApiResult<MemoryDiffResult>::fail(
                ErrorCode::LimitExceeded,
                "diff exceeds limits (ranges " +
                std::to_string(AgentLimits::MAX_DIFF_CHANGED_RANGES) + ", bytes " +
                std::to_string(AgentLimits::MAX_DIFF_CHANGED_BYTES) +
                "); narrow the range or delete/replace byte values");
        }
    }

    std::ostringstream oss;
    oss << "a=" << idA << " b=" << idB
        << " start=" << std::hex << start << std::dec
        << " length=" << length
        << " ranges=" << result.ranges.size()
        << " changedBytes=" << result.changedBytes;
    log_.record("diffMemorySnapshots", oss.str(),
                std::to_string(result.changedBytes) + " changed bytes", elapsedMs(t0));

    return AgentApiResult<MemoryDiffResult>::ok(std::move(result));
}

// ---------------------------------------------------------------------------
// Byte-pattern search (Stage 6.26)
// ---------------------------------------------------------------------------

AgentApiResult<ByteSequenceSearchResult>
AgentApi::findBytecodeSequence(uint16_t rangeStart, uint16_t rangeEnd,
                               const std::vector<uint8_t> &pattern,
                               const std::vector<uint8_t> &mask,
                               size_t maxMatches)
{
    auto t0 = std::chrono::steady_clock::now();

    if (pattern.empty()) {
        log_.record("findBytecodeSequence", "empty pattern", "invalid",
                    elapsedMs(t0), false, "pattern must not be empty");
        return AgentApiResult<ByteSequenceSearchResult>::fail(
            ErrorCode::InvalidArgument, "pattern must not be empty");
    }
    if (pattern.size() > AgentLimits::MAX_BYTECODE_PATTERN_LENGTH) {
        log_.record("findBytecodeSequence", "pattern too long", "invalid",
                    elapsedMs(t0), false, "pattern exceeds maximum");
        return AgentApiResult<ByteSequenceSearchResult>::fail(
            ErrorCode::InvalidArgument,
            "pattern length exceeds maximum (" +
            std::to_string(AgentLimits::MAX_BYTECODE_PATTERN_LENGTH) + ")");
    }
    if (!mask.empty() && mask.size() != pattern.size()) {
        log_.record("findBytecodeSequence", "mask size mismatch", "invalid",
                    elapsedMs(t0), false, "mask must be empty or same length as pattern");
        return AgentApiResult<ByteSequenceSearchResult>::fail(
            ErrorCode::InvalidArgument,
            "mask must be empty or have the same length as pattern");
    }
    if (rangeEnd < rangeStart) {
        log_.record("findBytecodeSequence", "rangeEnd < rangeStart", "invalid",
                    elapsedMs(t0), false, "invalid range");
        return AgentApiResult<ByteSequenceSearchResult>::fail(
            ErrorCode::InvalidRange, "range_end must be >= range_start");
    }
    if (maxMatches == 0 || maxMatches > AgentLimits::MAX_SEARCH_MATCHES_HARD) {
        log_.record("findBytecodeSequence", "max=" + std::to_string(maxMatches), "invalid",
                    elapsedMs(t0), false, "maxMatches out of range");
        return AgentApiResult<ByteSequenceSearchResult>::fail(
            ErrorCode::InvalidArgument,
            "maxMatches must be 1.." +
            std::to_string(AgentLimits::MAX_SEARCH_MATCHES_HARD));
    }

    const size_t scanLen = static_cast<size_t>(rangeEnd) - rangeStart + 1;
    auto bytes = readMemory(rangeStart, scanLen);
    if (!bytes.success) {
        return AgentApiResult<ByteSequenceSearchResult>::fail(
            bytes.error_code, bytes.error_message);
    }

    ByteSequenceSearchResult result;
    result.rangeStart = rangeStart;
    result.rangeEnd   = rangeEnd;
    result.scannedBytes = scanLen;

    // Naive O(n·m) scan — ascending addresses, overlapping matches kept.
    const auto &buf = bytes.value;
    if (scanLen >= pattern.size()) {
        for (size_t i = 0; i + pattern.size() <= scanLen; ++i) {
            bool match = true;
            for (size_t k = 0; k < pattern.size(); ++k) {
                if (!mask.empty() && mask[k] == 0) continue;   // wildcard byte
                if (buf[i + k] != pattern[k]) { match = false; break; }
            }
            if (match) {
                result.addresses.push_back(
                    static_cast<uint16_t>(rangeStart + i));
                if (result.addresses.size() > maxMatches) {
                    log_.record("findBytecodeSequence",
                                "range=" + std::to_string(rangeStart) + ".." +
                                std::to_string(rangeEnd),
                                "limit", elapsedMs(t0), false, "too many matches");
                    return AgentApiResult<ByteSequenceSearchResult>::fail(
                        ErrorCode::LimitExceeded,
                        "more than " + std::to_string(maxMatches) +
                        " matches; narrow the range or lower expectations");
                }
            }
        }
    }

    std::ostringstream oss;
    oss << "range=" << std::hex << rangeStart << ".." << rangeEnd
        << std::dec << " matches=" << result.addresses.size();
    log_.record("findBytecodeSequence", oss.str(),
                std::to_string(result.addresses.size()) + " matches", elapsedMs(t0));

    return AgentApiResult<ByteSequenceSearchResult>::ok(std::move(result));
}

// ---------------------------------------------------------------------------
// Immediate operand search (Stage 6.26)
//
// Instruction interpretation is delegated to the debugger disassembler
// (linearDisassemble) — this only inspects the decoded operand text, so no
// second opcode decoder exists in the Python layer.
// ---------------------------------------------------------------------------

static bool parseHexToken(const std::string &tok, uint16_t &out)
{
    // The disassembler formats numeric operands as %02X / %04X only —
    // require exactly 2 or 4 hex digits so register names (A..F) never match.
    if (tok.size() != 2 && tok.size() != 4) return false;
    unsigned v = 0;
    for (char c : tok) {
        unsigned d;
        if (c >= '0' && c <= '9')      d = c - '0';
        else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
        else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
        else return false;
        v = (v << 4) | d;
    }
    out = static_cast<uint16_t>(v);
    return true;
}

AgentApiResult<ImmediateSearchResult>
AgentApi::findImmediateInRange(uint16_t rangeStart, uint16_t rangeEnd,
                               uint16_t value, size_t maxMatches)
{
    auto t0 = std::chrono::steady_clock::now();

    if (rangeEnd < rangeStart) {
        log_.record("findImmediateInRange", "rangeEnd < rangeStart", "invalid",
                    elapsedMs(t0), false, "invalid range");
        return AgentApiResult<ImmediateSearchResult>::fail(
            ErrorCode::InvalidRange, "range_end must be >= range_start");
    }
    if (maxMatches == 0 || maxMatches > AgentLimits::MAX_SEARCH_MATCHES_HARD) {
        log_.record("findImmediateInRange", "max=" + std::to_string(maxMatches), "invalid",
                    elapsedMs(t0), false, "maxMatches out of range");
        return AgentApiResult<ImmediateSearchResult>::fail(
            ErrorCode::InvalidArgument,
            "maxMatches must be 1.." +
            std::to_string(AgentLimits::MAX_SEARCH_MATCHES_HARD));
    }

    const uint32_t length = static_cast<uint32_t>(rangeEnd) - rangeStart + 1;
    DisassembleRangeResult sweep = linearDisassemble(rangeStart, length);

    ImmediateSearchResult result;
    result.rangeStart = rangeStart;
    result.rangeEnd   = rangeEnd;
    result.value      = value;

    for (const auto &inst : sweep.instructions) {
        // Operands are "REG, XX" / "XXXX" — the disassembler already split
        // semantics; here we only take numeric hex tokens and compare values.
        bool hit = false;
        size_t pos = 0;
        while (pos <= inst.operands.size()) {
            size_t comma = inst.operands.find(',', pos);
            if (comma == std::string::npos) comma = inst.operands.size();
            size_t b = pos, e = comma;
            while (b < e && inst.operands[b] == ' ') ++b;
            while (e > b && inst.operands[e - 1] == ' ') --e;
            uint16_t tokVal = 0;
            if (e > b && parseHexToken(inst.operands.substr(b, e - b), tokVal) &&
                tokVal == value) {
                hit = true;
                break;
            }
            pos = comma + 1;
            if (comma == inst.operands.size()) break;
        }
        if (!hit) continue;

        ImmediateMatch m;
        m.address  = inst.address;
        m.mnemonic = inst.mnemonic;
        m.operands = inst.operands;
        m.bytes    = inst.bytes;
        result.matches.push_back(std::move(m));

        if (result.matches.size() > maxMatches) {
            log_.record("findImmediateInRange",
                        "value=" + std::to_string(value),
                        "limit", elapsedMs(t0), false, "too many matches");
            return AgentApiResult<ImmediateSearchResult>::fail(
                ErrorCode::LimitExceeded,
                "more than " + std::to_string(maxMatches) +
                " matches; narrow the range");
        }
    }

    std::ostringstream oss;
    oss << "range=" << std::hex << rangeStart << ".." << rangeEnd
        << " value=" << value << std::dec
        << " matches=" << result.matches.size();
    log_.record("findImmediateInRange", oss.str(),
                std::to_string(result.matches.size()) + " matches", elapsedMs(t0));

    return AgentApiResult<ImmediateSearchResult>::ok(std::move(result));
}

// ---------------------------------------------------------------------------
// VRAM raw bytes (Stage 6.26)
//
// VRAM is memory-mapped: 4 bit-planes of 8K at 0x8000..0xFFFF (see
// getVramInfo).  Reads go through the existing debug target API only.
// ---------------------------------------------------------------------------

AgentApiResult<std::vector<uint8_t>>
AgentApi::getVramBytes(uint16_t address, uint32_t length)
{
    auto t0 = std::chrono::steady_clock::now();

    static const uint16_t VRAM_REGION_START = 0x8000;

    if (length == 0) {
        log_.record("getVramBytes", "length=0", "invalid",
                    elapsedMs(t0), false, "length must be > 0");
        return AgentApiResult<std::vector<uint8_t>>::fail(
            ErrorCode::InvalidArgument, "length must be > 0");
    }
    if (address < VRAM_REGION_START) {
        log_.record("getVramBytes", "address below VRAM", "invalid",
                    elapsedMs(t0), false, "address outside VRAM region");
        return AgentApiResult<std::vector<uint8_t>>::fail(
            ErrorCode::InvalidAddress,
            "VRAM addresses start at 0x8000");
    }
    if (static_cast<uint32_t>(address) + length > 0x10000) {
        log_.record("getVramBytes", "range past 0xFFFF", "invalid",
                    elapsedMs(t0), false, "address + length exceeds VRAM region");
        return AgentApiResult<std::vector<uint8_t>>::fail(
            ErrorCode::InvalidRange, "address + length exceeds VRAM region (0x8000..0xFFFF)");
    }
    if (length > AgentLimits::MAX_VRAM_READ_RANGE) {
        log_.record("getVramBytes", "length too big", "invalid",
                    elapsedMs(t0), false, "length exceeds maximum");
        return AgentApiResult<std::vector<uint8_t>>::fail(
            ErrorCode::InvalidRange,
            "length exceeds maximum (" +
            std::to_string(AgentLimits::MAX_VRAM_READ_RANGE) + ")");
    }

    auto r = readMemory(address, length);
    if (!r.success) {
        return AgentApiResult<std::vector<uint8_t>>::fail(
            r.error_code, r.error_message);
    }

    std::ostringstream oss;
    oss << "address=" << std::hex << address << " length=" << std::dec << length;
    log_.record("getVramBytes", oss.str(),
                std::to_string(r.value.size()) + " bytes", elapsedMs(t0));

    return r;
}

// ---------------------------------------------------------------------------
// Instruction History (Stage 6.1 §11)
// ---------------------------------------------------------------------------

AgentApiResult<std::vector<InstructionHistoryEntry>>
AgentApi::getInstructionHistory(size_t count)
{
    auto t0 = std::chrono::steady_clock::now();

    if (count == 0) {
        log_.record("getInstructionHistory", "count=0", "invalid count",
                    elapsedMs(t0), false, "count must be > 0");
        return AgentApiResult<std::vector<InstructionHistoryEntry>>::fail(
            ErrorCode::InvalidArgument, "count must be > 0");
    }

    // Stage 6.3: safe maximum
    if (count > AgentLimits::MAX_HISTORY_ENTRIES) {
        count = AgentLimits::MAX_HISTORY_ENTRIES;
    }

    auto all = backend_.instructionHistorySnapshot();

    // Take last 'count' entries
    size_t start = 0;
    if (all.size() > count) {
        start = all.size() - count;
    }

    auto readByte = [this](uint16_t addr) -> uint8_t {
        return backend_.readMemory(addr);
    };

    std::vector<InstructionHistoryEntry> result;
    result.reserve(all.size() - start);

    for (size_t i = start; i < all.size(); ++i) {
        const auto &ev = all[i];

        InstructionHistoryEntry entry;
        entry.address = ev.pcBefore;
        entry.next_address = ev.pcAfter;

        // Build instruction bytes: opcode + operand bytes
        entry.bytes.push_back(ev.opcode);
        if (ev.length >= 2) entry.bytes.push_back(ev.operandBytes[0]);
        if (ev.length >= 3) entry.bytes.push_back(ev.operandBytes[1]);

        // Disassemble for text
        DisassembledInstruction di = ::disassemble(ev.pcBefore, readByte);
        entry.disassembly = di.text;

        result.push_back(std::move(entry));
    }

    std::ostringstream oss;
    oss << "count=" << count << " available=" << all.size();
    log_.record("getInstructionHistory", oss.str(),
                std::to_string(result.size()) + " entries",
                elapsedMs(t0));

    return AgentApiResult<std::vector<InstructionHistoryEntry>>::ok(
        std::move(result));
}

// ---------------------------------------------------------------------------
// Trace / I/O (Stage 6.3: AgentApiResult)
// ---------------------------------------------------------------------------

AgentApiResult<std::vector<InstructionEvent>> AgentApi::getExecutionTrace(size_t maxEntries)
{
    auto t0 = std::chrono::steady_clock::now();

    if (maxEntries == 0) {
        log_.record("getExecutionTrace", "maxEntries=0", "invalid",
                    elapsedMs(t0), false, "maxEntries must be > 0");
        return AgentApiResult<std::vector<InstructionEvent>>::fail(
            ErrorCode::InvalidArgument, "maxEntries must be > 0");
    }

    // Stage 6.3: safe maximum
    if (maxEntries > AgentLimits::MAX_TRACE_ENTRIES) {
        maxEntries = AgentLimits::MAX_TRACE_ENTRIES;
    }

    auto all = backend_.instructionHistorySnapshot();

    if (all.size() > maxEntries) {
        all.erase(all.begin(), all.end() - static_cast<ptrdiff_t>(maxEntries));
    }

    log_.record("getExecutionTrace",
                "max=" + std::to_string(maxEntries),
                std::to_string(all.size()) + " events",
                elapsedMs(t0));
    return AgentApiResult<std::vector<InstructionEvent>>::ok(std::move(all));
}

AgentApiResult<std::vector<IoAccessEvent>> AgentApi::getIoTrace(size_t maxEntries)
{
    auto t0 = std::chrono::steady_clock::now();

    if (maxEntries == 0) {
        log_.record("getIoTrace", "maxEntries=0", "invalid",
                    elapsedMs(t0), false, "maxEntries must be > 0");
        return AgentApiResult<std::vector<IoAccessEvent>>::fail(
            ErrorCode::InvalidArgument, "maxEntries must be > 0");
    }

    // Stage 6.3: safe maximum
    if (maxEntries > AgentLimits::MAX_TRACE_ENTRIES) {
        maxEntries = AgentLimits::MAX_TRACE_ENTRIES;
    }

    auto all = backend_.ioHistorySnapshot();

    if (all.size() > maxEntries) {
        all.erase(all.begin(), all.end() - static_cast<ptrdiff_t>(maxEntries));
    }

    log_.record("getIoTrace",
                "max=" + std::to_string(maxEntries),
                std::to_string(all.size()) + " events",
                elapsedMs(t0));
    return AgentApiResult<std::vector<IoAccessEvent>>::ok(std::move(all));
}

// ---------------------------------------------------------------------------
// Screen (Stage 6.3: AgentScreenSnapshot, no IDebugBackend types)
// ---------------------------------------------------------------------------

AgentApiResult<AgentScreenSnapshot> AgentApi::getScreen()
{
    auto t0 = std::chrono::steady_clock::now();
    auto snap = backend_.screenSnapshot();

    AgentScreenSnapshot result;
    result.pixels = std::move(snap.pixels);
    result.width  = snap.width;
    result.height = snap.height;

    std::ostringstream oss;
    oss << result.width << "x" << result.height;
    log_.record("getScreen", "", oss.str(), elapsedMs(t0));
    return AgentApiResult<AgentScreenSnapshot>::ok(std::move(result));
}

// ---------------------------------------------------------------------------
// Beam / raster state (Stage 6.27: racing-the-beam debugging)
//
// Read-only. Every value originates in DebugAdapter (the only Vector-aware
// layer) and is surfaced through the backend facade; the Agent API performs
// no timing computation of its own.
// ---------------------------------------------------------------------------

AgentApiResult<AgentBeamState> AgentApi::getBeamState()
{
    auto t0 = std::chrono::steady_clock::now();
    BeamState bs = backend_.beamState();

    if (!bs.available) {
        log_.record("getBeamState", "", "unavailable", elapsedMs(t0));
        return AgentApiResult<AgentBeamState>::fail(
            ErrorCode::Unsupported, "target does not expose beam/raster state");
    }

    AgentBeamState r;
    r.available      = bs.available;
    r.running        = bs.running;
    r.frame          = bs.frame;
    r.vCycleInFrame  = bs.vCycleInFrame;
    r.rasterLine     = bs.rasterLine;
    r.vCycleInLine   = bs.vCycleInLine;
    r.rpixel         = bs.rpixel;
    r.visible        = bs.visible;
    r.visibleX       = bs.visibleX;
    r.visibleY       = bs.visibleY;
    r.frameVCycles   = bs.frameVCycles;
    r.lineVCycles    = bs.lineVCycles;
    r.frameLines     = bs.frameLines;
    r.cpuPc          = bs.cpuPc;
    r.cpuOpcode      = bs.cpuOpcode;
    r.hasPaletteIndex = bs.hasPaletteIndex;
    r.paletteIndex   = bs.paletteIndex;
    r.paletteValue   = bs.paletteValue;
    r.borderIndex    = bs.borderIndex;

    std::ostringstream oss;
    oss << "frame=" << r.frame << " line=" << r.rasterLine
        << " vcycle=" << r.vCycleInFrame;
    log_.record("getBeamState", "", oss.str(), elapsedMs(t0));
    return AgentApiResult<AgentBeamState>::ok(std::move(r));
}

// ---------------------------------------------------------------------------
// Screen snapshot as PNG (Stage 6.27)
//
// The image is the REAL TV framebuffer (backend screenSnapshot), never a
// reconstruction from VRAM. Converted ARGB8888 -> RGBA, encoded to an
// in-memory PNG, base64 for JSON transport. No scaling, no geometry change.
// ---------------------------------------------------------------------------

AgentApiResult<AgentScreenSnapshot> AgentApi::getScreenSnapshot()
{
    auto t0 = std::chrono::steady_clock::now();

    BeamState bs = backend_.beamState();
    auto snap = backend_.screenSnapshot();

    if (snap.width <= 0 || snap.height <= 0 || snap.pixels.empty()) {
        log_.record("getScreenSnapshot", "", "no image", elapsedMs(t0));
        return AgentApiResult<AgentScreenSnapshot>::fail(
            ErrorCode::OperationFailed, "screen buffer is empty");
    }

    AgentScreenSnapshot result;
    result.width  = snap.width;
    result.height = snap.height;
    result.frame  = bs.frame;
    result.source = "tv";
    result.format = "RGB";
    result.paletteMode = "raster";
    // A guaranteed-complete frame only when the emulator is paused; while
    // running the buffer is still a committed frame but may lag the beam.
    result.completeFrame = !bs.running;

    // ARGB8888 -> RGBA8888 (byte order for the PNG encoder).
    const size_t n = static_cast<size_t>(snap.width) * static_cast<size_t>(snap.height);
    std::vector<uint8_t> rgba(n * 4);
    for (size_t i = 0; i < n; ++i) {
        const uint32_t p = snap.pixels[i];
        rgba[i * 4 + 0] = static_cast<uint8_t>((p >> 16) & 0xFF); // R
        rgba[i * 4 + 1] = static_cast<uint8_t>((p >> 8)  & 0xFF); // G
        rgba[i * 4 + 2] = static_cast<uint8_t>( p        & 0xFF); // B
        rgba[i * 4 + 3] = 0xFF;                                    // A opaque
    }

    std::vector<uint8_t> png = png::encodeRGBA(rgba.data(), snap.width, snap.height);
    result.pngBase64 = png::base64Encode(png);

    std::ostringstream oss;
    oss << result.width << "x" << result.height << " png=" << png.size() << "B";
    log_.record("getScreenSnapshot", "", oss.str(), elapsedMs(t0));
    return AgentApiResult<AgentScreenSnapshot>::ok(std::move(result));
}

// ---------------------------------------------------------------------------
// Raster events (Stage 6.27 P2)
//
// Delegates filtering to the backend ring; converts to plain AgentRasterEvent
// (no debugger types cross the Agent boundary).
// ---------------------------------------------------------------------------

AgentApiResult<std::vector<AgentRasterEvent>> AgentApi::getRasterEvents(
    uint64_t frame, uint32_t vCycleStart, uint32_t vCycleEnd,
    int port, uint16_t pc, size_t maxResults)
{
    auto t0 = std::chrono::steady_clock::now();

    // Formalized cap so a query can never return an unbounded vector.
    if (maxResults > AgentLimits::MAX_RASTER_EVENTS)
        maxResults = AgentLimits::MAX_RASTER_EVENTS;

    auto evs = backend_.rasterEvents(frame, vCycleStart, vCycleEnd, port, pc, maxResults);

    std::vector<AgentRasterEvent> out;
    out.reserve(evs.size());
    for (const auto &e : evs) {
        AgentRasterEvent a;
        a.frame         = e.frame;
        a.vCycle        = e.vCycle;
        a.rasterLine    = e.rasterLine;
        a.vCycleInLine  = e.vCycleInLine;
        a.pc            = e.pc;
        a.port          = e.port;
        a.value         = e.value;
        out.push_back(a);
    }

    std::ostringstream oss;
    oss << out.size() << " events";
    log_.record("getRasterEvents", "", oss.str(), elapsedMs(t0));
    return AgentApiResult<std::vector<AgentRasterEvent>>::ok(std::move(out));
}

AgentApiResult<void> AgentApi::clearRasterEvents()
{
    auto t0 = std::chrono::steady_clock::now();
    backend_.clearRasterEvents();
    log_.record("clearRasterEvents", "", "cleared", elapsedMs(t0));
    return AgentApiResult<void>::ok();
}

// ---------------------------------------------------------------------------
// Annotations — through Backend command protocol (Stage 6.3: AgentApiResult<void>)
// ---------------------------------------------------------------------------

AgentApiResult<void> AgentApi::createFunction(uint16_t address, uint16_t /*size*/)
{
    auto t0 = std::chrono::steady_clock::now();
    std::string name = SymbolDatabase::autoName(address);
    auto result = backend_.requestCreateFunction(address, name);

    std::ostringstream oss;
    oss << "addr=" << std::hex << address << " name=" << name;
    log_.record("createFunction", oss.str(),
                result.success ? "created" : result.error, elapsedMs(t0),
                result.success, result.error);

    if (!result.success) {
        return AgentApiResult<void>::fail(ErrorCode::OperationFailed, result.error);
    }
    return AgentApiResult<void>::ok();
}

AgentApiResult<void> AgentApi::renameFunction(uint16_t address, const std::string &name)
{
    auto t0 = std::chrono::steady_clock::now();
    auto result = backend_.requestRenameSymbol(address, name);

    std::ostringstream oss;
    oss << "addr=" << std::hex << address << " name=" << name;
    log_.record("renameFunction", oss.str(),
                result.success ? "renamed" : result.error, elapsedMs(t0),
                result.success, result.error);

    if (!result.success) {
        return AgentApiResult<void>::fail(ErrorCode::NotFound, result.error);
    }
    return AgentApiResult<void>::ok();
}

AgentApiResult<void> AgentApi::setFunctionComment(uint16_t address, const std::string &comment)
{
    auto t0 = std::chrono::steady_clock::now();
    auto result = backend_.requestSetComment(address, comment);

    std::ostringstream oss;
    oss << "addr=" << std::hex << address;
    log_.record("setFunctionComment", oss.str(),
                result.success ? "ok" : result.error, elapsedMs(t0),
                result.success, result.error);

    if (!result.success) {
        return AgentApiResult<void>::fail(ErrorCode::NotFound, result.error);
    }
    return AgentApiResult<void>::ok();
}

AgentApiResult<void> AgentApi::deleteFunction(uint16_t address)
{
    auto t0 = std::chrono::steady_clock::now();
    auto result = backend_.requestRemoveSymbol(address);

    std::ostringstream oss;
    oss << "addr=" << std::hex << address;
    log_.record("deleteFunction", oss.str(),
                result.success ? "deleted" : result.error, elapsedMs(t0),
                result.success, result.error);

    if (!result.success) {
        return AgentApiResult<void>::fail(ErrorCode::NotFound, result.error);
    }
    return AgentApiResult<void>::ok();
}

AgentApiResult<void> AgentApi::addLabel(uint16_t address, const std::string &name)
{
    auto t0 = std::chrono::steady_clock::now();
    auto result = backend_.requestAddLabel(address, name);

    std::ostringstream oss;
    oss << "addr=" << std::hex << address << " name=" << name;
    log_.record("addLabel", oss.str(),
                result.success ? "created" : result.error, elapsedMs(t0),
                result.success, result.error);

    if (!result.success) {
        return AgentApiResult<void>::fail(ErrorCode::OperationFailed, result.error);
    }
    return AgentApiResult<void>::ok();
}

AgentApiResult<void> AgentApi::setComment(uint16_t address, const std::string &comment)
{
    return setFunctionComment(address, comment);
}

AgentApiResult<void> AgentApi::applyAnnotation(const Annotation &annotation)
{
    auto t0 = std::chrono::steady_clock::now();
    AgentApiResult<void> result = AgentApiResult<void>::ok();

    switch (annotation.type) {
    case Annotation::Function:
        result = createFunction(annotation.address);
        if (result.success && !annotation.name.empty()) {
            result = renameFunction(annotation.address, annotation.name);
        }
        if (result.success && !annotation.comment.empty()) {
            result = setFunctionComment(annotation.address, annotation.comment);
        }
        break;

    case Annotation::Label:
        result = addLabel(annotation.address, annotation.name);
        break;

    case Annotation::Comment:
        result = setComment(annotation.address, annotation.comment);
        break;

    case Annotation::Rename:
        result = renameFunction(annotation.address, annotation.name);
        break;
    }

    std::ostringstream oss;
    oss << "type=" << static_cast<int>(annotation.type)
        << " addr=" << std::hex << annotation.address
        << " confidence=" << std::fixed << std::setprecision(2)
        << annotation.confidence;
    log_.record("applyAnnotation", oss.str(),
                result.success ? "applied" : result.error_message, elapsedMs(t0),
                result.success, result.error_message);
    return result;
}

// ---------------------------------------------------------------------------
// ROM (Stage 6.3: single loadRom method)
// ---------------------------------------------------------------------------

AgentApiResult<LoadRomResult> AgentApi::loadRom(const std::string &path, uint32_t org)
{
    auto t0 = std::chrono::steady_clock::now();

    // Auto-detect origin from extension if not explicitly given
    uint32_t origin = org;
    if (origin == 0 && path.find('.') != std::string::npos) {
        origin = getRomLoadAddress(path);
    }

    bool ok = backend_.loadRom(path, org);
    if (!ok) {
        log_.record("loadRom", path, "failed", elapsedMs(t0), false, "load failed");
        return AgentApiResult<LoadRomResult>::fail(
            ErrorCode::OperationFailed, "Failed to load ROM: " + path);
    }

    LoadRomResult result;
    result.path = path;
    result.origin = origin;
    result.pc = backend_.getCpuState().pc;

    std::ostringstream oss;
    oss << "path=" << path << " origin=" << std::hex << origin
        << " pc=" << result.pc;
    log_.record("loadRom", "", oss.str(), elapsedMs(t0));

    return AgentApiResult<LoadRomResult>::ok(std::move(result));
}

// ---------------------------------------------------------------------------
// Stack (Stage 6.1 §13)
// ---------------------------------------------------------------------------

AgentApiResult<std::vector<StackEntry>>
AgentApi::getStack(size_t limit)
{
    auto t0 = std::chrono::steady_clock::now();

    if (limit == 0) {
        log_.record("getStack", "limit=0", "invalid limit",
                    elapsedMs(t0), false, "limit must be > 0");
        return AgentApiResult<std::vector<StackEntry>>::fail(
            ErrorCode::InvalidArgument, "limit must be > 0");
    }

    // Stage 6.3: safe maximum
    if (limit > AgentLimits::MAX_STACK_LIMIT) {
        limit = AgentLimits::MAX_STACK_LIMIT;
    }

    uint16_t sp = backend_.getCpuState().sp;
    const auto &db = backend_.symbolDatabase();

    std::vector<StackEntry> result;
    result.reserve(limit);

    for (size_t i = 0; i < limit; ++i) {
        // Stage 6.3: overflow-safe address arithmetic
        uint32_t fullAddr = static_cast<uint32_t>(sp) + static_cast<uint32_t>(i) * 2;
        if (fullAddr > 0xFFFF) break;  // stop at memory boundary
        uint16_t addr = static_cast<uint16_t>(fullAddr);

        StackEntry entry;
        entry.address = addr;

        // Read 16-bit value (little-endian)
        uint8_t lo = backend_.readMemory(addr);
        // Check if hi byte address is still within bounds
        uint32_t hiAddr = fullAddr + 1;
        uint8_t hi = (hiAddr <= 0xFFFF) ? backend_.readMemory(static_cast<uint16_t>(hiAddr)) : 0;
        entry.value = static_cast<uint16_t>(lo | (hi << 8));

        // Look up symbol for this value
        const DebugSymbol *sym = db.findSymbol(entry.value);
        if (sym) {
            entry.symbol = sym->name;
        }

        result.push_back(entry);
    }

    std::ostringstream oss;
    oss << "sp=" << std::hex << sp << " limit=" << std::dec << limit;
    log_.record("getStack", oss.str(),
                std::to_string(result.size()) + " entries", elapsedMs(t0));

    return AgentApiResult<std::vector<StackEntry>>::ok(std::move(result));
}

// ---------------------------------------------------------------------------
// Memory Map (Stage 6.1 §18)
// ---------------------------------------------------------------------------

AgentApiResult<std::vector<MemoryMapBlock>>
AgentApi::getMemoryMap()
{
    auto t0 = std::chrono::steady_clock::now();

    auto activity = backend_.liveActivitySnapshot();
    const auto exec = backend_.activitySnapshot();   // per-address execute counts
    const auto &db = backend_.symbolDatabase();
    auto regions = db.allRegions();

    std::vector<MemoryMapBlock> result;
    result.reserve(256);

    for (int i = 0; i < 256; ++i) {
        MemoryMapBlock block;
        block.start = static_cast<uint16_t>(i * 256);
        block.end = static_cast<uint16_t>(block.start + 255);

        // Classification from SymbolDatabase regions
        block.classification = MemoryMapBlock::Classification::Unknown;
        for (const auto &r : regions) {
            if (r.start <= block.start && r.end >= block.end) {
                switch (r.type) {
                    case MemoryRegionType::Code:
                        block.classification = MemoryMapBlock::Classification::Code;
                        break;
                    case MemoryRegionType::Data:
                        block.classification = MemoryMapBlock::Classification::Data;
                        break;
                    default:
                        break;
                }
                break;
            }
        }

        // Activity counters
        block.read_activity = activity.blocks[i].lastReadTime.time_since_epoch().count() > 0
            ? 1 : 0;  // simplified: 1 = accessed, 0 = not
        block.write_activity = activity.blocks[i].lastWriteTime.time_since_epoch().count() > 0
            ? 1 : 0;

        // Stage 6.22 §4: execute_activity was declared and serialised but never
        // filled, so it reached every consumer as a constant zero that reads as
        // a measurement.  It is now the number of instructions that started in
        // this block — cumulative and monotonic while the ROM runs.  read_ and
        // write_activity above stay 0/1 flags; the asymmetry is deliberate and
        // documented in agent_types.h.
        block.execute_activity = 0;
        if (exec.executeCount.size() == 65536) {
            for (int a = block.start; a <= block.end; ++a)   // int: end is 0xFFFF for the last block
                block.execute_activity += exec.executeCount[a];
        }

        // Check if block has non-zero content
        auto data = backend_.readMemorySnapshot(block.start, 256);
        for (uint8_t b : data.data) {
            if (b != 0) {
                block.has_content = true;
                break;
            }
        }

        result.push_back(block);
    }

    log_.record("getMemoryMap", "", "256 blocks", elapsedMs(t0));

    return AgentApiResult<std::vector<MemoryMapBlock>>::ok(std::move(result));
}

// ---------------------------------------------------------------------------
// Runtime Memory Analysis (Stage 6.20)
// ---------------------------------------------------------------------------

AgentApiResult<void> AgentApi::clearMemoryAccessMap()
{
    auto t0 = std::chrono::steady_clock::now();
    backend_.clearRuntimeAccessMap();
    log_.record("clearMemoryAccessMap", "", "ok", elapsedMs(t0));
    return AgentApiResult<void>::ok();
}

AgentApiResult<std::vector<RuntimeAccessBlock>> AgentApi::getMemoryAccessMap()
{
    auto t0 = std::chrono::steady_clock::now();
    auto blocks = backend_.getRuntimeAccessMap();
    log_.record("getMemoryAccessMap", "", "256 blocks", elapsedMs(t0));
    return AgentApiResult<std::vector<RuntimeAccessBlock>>::ok(std::move(blocks));
}

AgentApiResult<std::vector<RuntimeAccessLogEntry>> AgentApi::getMemoryAccessLog(size_t maxEntries)
{
    auto t0 = std::chrono::steady_clock::now();
    if (maxEntries > AgentLimits::MAX_MEMORY_ACCESS_LOG) {
        maxEntries = AgentLimits::MAX_MEMORY_ACCESS_LOG;
    }
    auto entries = backend_.getRuntimeAccessLog(maxEntries);
    std::ostringstream oss;
    oss << "max=" << maxEntries << " got=" << entries.size();
    log_.record("getMemoryAccessLog", oss.str(),
                std::to_string(entries.size()) + " entries", elapsedMs(t0));
    return AgentApiResult<std::vector<RuntimeAccessLogEntry>>::ok(std::move(entries));
}

AgentApiResult<uint32_t> AgentApi::createMemorySnapshot(uint16_t start, size_t size)
{
    auto t0 = std::chrono::steady_clock::now();

    // Overflow check
    uint32_t endAddr = static_cast<uint32_t>(start) + size;
    if (endAddr > 0x10000) {
        log_.record("createMemorySnapshot",
                    "start=" + std::to_string(start) + " size=" + std::to_string(size),
                    "range overflow", elapsedMs(t0), false, "address + size exceeds 64K");
        return AgentApiResult<uint32_t>::fail(
            ErrorCode::InvalidRange, "address + size exceeds 64K address space");
    }

    uint32_t id = backend_.createMemorySnapshot(start, size);
    if (id == 0) {
        log_.record("createMemorySnapshot", "", "failed", elapsedMs(t0), false, "snapshot creation failed");
        return AgentApiResult<uint32_t>::fail(
            ErrorCode::OperationFailed, "Failed to create memory snapshot");
    }

    std::ostringstream oss;
    oss << "id=" << id << " start=" << std::hex << start << " size=" << std::dec << size;
    log_.record("createMemorySnapshot", oss.str(), "ok", elapsedMs(t0));
    return AgentApiResult<uint32_t>::ok(id);
}

AgentApiResult<MemorySnapshotData> AgentApi::getMemorySnapshot(uint32_t snapshotId)
{
    auto t0 = std::chrono::steady_clock::now();
    auto snap = backend_.getMemorySnapshot(snapshotId);
    if (snap.data.empty()) {
        log_.record("getMemorySnapshot", "id=" + std::to_string(snapshotId),
                    "not found", elapsedMs(t0), false, "snapshot not found or invalidated");
        return AgentApiResult<MemorySnapshotData>::fail(
            ErrorCode::NotFound, "Snapshot not found or invalidated");
    }
    std::ostringstream oss;
    oss << "id=" << snapshotId << " size=" << snap.data.size();
    log_.record("getMemorySnapshot", oss.str(), "ok", elapsedMs(t0));
    return AgentApiResult<MemorySnapshotData>::ok(std::move(snap));
}

AgentApiResult<MemorySnapshotDiff> AgentApi::compareMemorySnapshots(uint32_t idA, uint32_t idB)
{
    auto t0 = std::chrono::steady_clock::now();
    auto diff = backend_.compareMemorySnapshots(idA, idB);

    // Check if both snapshots existed (empty diff could mean identical or invalid)
    auto snapA = backend_.getMemorySnapshot(idA);
    auto snapB = backend_.getMemorySnapshot(idB);
    if (snapA.data.empty() || snapB.data.empty()) {
        log_.record("compareMemorySnapshots",
                    "a=" + std::to_string(idA) + " b=" + std::to_string(idB),
                    "not found", elapsedMs(t0), false, "one or both snapshots not found");
        return AgentApiResult<MemorySnapshotDiff>::fail(
            ErrorCode::NotFound, "One or both snapshots not found or invalidated");
    }

    std::ostringstream oss;
    oss << "a=" << idA << " b=" << idB << " ranges=" << diff.changed_ranges.size();
    log_.record("compareMemorySnapshots", oss.str(),
                std::to_string(diff.changed_ranges.size()) + " changed ranges",
                elapsedMs(t0));
    return AgentApiResult<MemorySnapshotDiff>::ok(std::move(diff));
}

// ---------------------------------------------------------------------------
// Screen Info (Stage 6.1 §20)
// ---------------------------------------------------------------------------

AgentApiResult<ScreenInfoResult> AgentApi::getScreenInfo()
{
    auto t0 = std::chrono::steady_clock::now();
    auto video = backend_.videoModeSnapshot();

    ScreenInfoResult result;
    result.width = video.screenWidth;
    result.height = video.screenHeight;
    result.visible_width = video.visibleWidth;
    result.visible_height = video.visibleHeight;
    result.mode512 = video.mode512;
    result.scroll_value = video.scrollValue;
    result.vram_base = video.vramBase;
    result.pixels_per_byte = video.pixelsPerByte;

    std::ostringstream oss;
    oss << result.width << "x" << result.height
        << (result.mode512 ? " 512-mode" : " 256-mode");
    log_.record("getScreenInfo", "", oss.str(), elapsedMs(t0));

    return AgentApiResult<ScreenInfoResult>::ok(std::move(result));
}

// ---------------------------------------------------------------------------
// VRAM Info (Stage 6.1 §19)
// ---------------------------------------------------------------------------

AgentApiResult<VramInfoResult> AgentApi::getVramInfo()
{
    auto t0 = std::chrono::steady_clock::now();
    auto video = backend_.videoModeSnapshot();

    VramInfoResult result;
    result.mode512 = video.mode512;
    result.vram_base = video.vramBase;
    result.scroll_value = video.scrollValue;

    // Vector-06C VRAM has 4 bit-planes at fixed addresses (32 KB total):
    //   Plane 0: 0xE000  (8 KB)
    //   Plane 1: 0xC000  (8 KB)
    //   Plane 2: 0xA000  (8 KB)
    //   Plane 3: 0x8000  (8 KB)
    // Each plane: 32 byte-columns × 256 bytes = 8192 bytes
    // Each byte: 8 horizontal pixels, bit 7 = left, bit 0 = right
    static const uint16_t planeAddresses[] = { 0xE000, 0xC000, 0xA000, 0x8000 };
    for (int i = 0; i < 4; ++i) {
        VramPlaneInfo plane;
        plane.plane = i;
        plane.address = planeAddresses[i];
        plane.size = 8192;
        result.planes.push_back(plane);
    }

    std::ostringstream oss;
    oss << "vram_base=" << std::hex << result.vram_base
        << (result.mode512 ? " 512-mode" : " 256-mode")
        << " " << std::dec << result.planes.size() << " planes";
    log_.record("getVramInfo", "", oss.str(), elapsedMs(t0));

    return AgentApiResult<VramInfoResult>::ok(std::move(result));
}

// ---------------------------------------------------------------------------
// Symbols (Stage 6.1 §15)
// ---------------------------------------------------------------------------

AgentApiResult<std::vector<SymbolInfo>>
AgentApi::getSymbols(size_t limit)
{
    auto t0 = std::chrono::steady_clock::now();
    const auto &db = backend_.symbolDatabase();
    auto all = db.allSymbols();

    std::vector<SymbolInfo> result;
    // Stage 6.3: limit=0 means "all" with safe maximum
    size_t count;
    if (limit == 0) {
        count = std::min(all.size(), AgentLimits::MAX_SYMBOLS_LIMIT);
    } else {
        count = std::min(limit, all.size());
    }
    result.reserve(count);

    for (size_t i = 0; i < count; ++i) {
        SymbolInfo info;
        info.address = all[i].address;
        info.name = all[i].name;
        info.comment = normalizeRdbText(all[i].comment);   // R2
        info.type = (all[i].type == SymbolType::Function)
            ? SymbolInfo::Type::Function
            : SymbolInfo::Type::Label;
        result.push_back(std::move(info));
    }

    log_.record("getSymbols", "",
                std::to_string(result.size()) + " / " + std::to_string(all.size()) + " symbols",
                elapsedMs(t0));

    return AgentApiResult<std::vector<SymbolInfo>>::ok(std::move(result));
}

AgentApiResult<SymbolInfo> AgentApi::getFunction(uint16_t address)
{
    auto t0 = std::chrono::steady_clock::now();
    const auto &db = backend_.symbolDatabase();
    const DebugSymbol *sym = db.findSymbol(address);

    if (!sym) {
        log_.record("getFunction", "addr=" + std::to_string(address),
                    "not found", elapsedMs(t0), false, "symbol not found");
        return AgentApiResult<SymbolInfo>::fail(
            ErrorCode::NotFound, "No symbol at address");
    }

    SymbolInfo info;
    info.address = sym->address;
    info.name = sym->name;
    info.comment = normalizeRdbText(sym->comment);   // R2
    info.type = (sym->type == SymbolType::Function)
        ? SymbolInfo::Type::Function
        : SymbolInfo::Type::Label;

    std::ostringstream oss;
    oss << "addr=" << std::hex << address << " name=" << sym->name;
    log_.record("getFunction", oss.str(), "found", elapsedMs(t0));

    return AgentApiResult<SymbolInfo>::ok(std::move(info));
}

// ---------------------------------------------------------------------------
// Xrefs (Stage 6.1 §16)
// ---------------------------------------------------------------------------

void AgentApi::ensureXrefsBuilt()
{
    // Rebuild the xref / call-target scan only when the cache is stale (ROM
    // load, symbol or region change). readMemory is a non-mutating peek, so
    // this stays a read-only query and never perturbs the emulation.
    SymbolDatabase &db = backend_.symbolDatabase();
    if (!db.xrefsAreDirty()) return;
    auto readByte = [this](uint16_t addr) -> uint8_t {
        return backend_.readMemory(addr);
    };
    db.rebuildXrefs(readByte);
}

AgentApiResult<std::vector<XrefResult>>
AgentApi::getXrefs(uint16_t address)
{
    auto t0 = std::chrono::steady_clock::now();
    ensureXrefsBuilt();
    const auto &db = backend_.symbolDatabase();

    auto toXrefs = db.xrefsTo(address);
    auto fromXrefs = db.xrefsFrom(address);

    std::vector<XrefResult> result;
    result.reserve(toXrefs.size() + fromXrefs.size());

    for (const auto &xr : toXrefs) {
        XrefResult r;
        r.from = xr.from;
        r.to = xr.to;
        result.push_back(r);
    }
    for (const auto &xr : fromXrefs) {
        XrefResult r;
        r.from = xr.from;
        r.to = xr.to;
        result.push_back(r);
    }

    std::ostringstream oss;
    oss << "addr=" << std::hex << address
        << " to=" << std::dec << toXrefs.size()
        << " from=" << fromXrefs.size();
    log_.record("getXrefs", oss.str(),
                std::to_string(result.size()) + " xrefs", elapsedMs(t0));

    return AgentApiResult<std::vector<XrefResult>>::ok(std::move(result));
}

// ---------------------------------------------------------------------------
// Call Graph (Stage 6.3: std::optional<uint16_t> — no $0000 ambiguity)
// ---------------------------------------------------------------------------

AgentApiResult<std::vector<CallGraphEdge>>
AgentApi::getCallGraph(std::optional<uint16_t> address, size_t limit)
{
    auto t0 = std::chrono::steady_clock::now();
    ensureXrefsBuilt();
    const auto &db = backend_.symbolDatabase();
    auto allEdges = db.callGraph();

    std::vector<CallGraphEdge> result;

    if (address.has_value()) {
        // Filter edges involving the given address (as caller or callee)
        uint16_t addr = address.value();
        for (const auto &e : allEdges) {
            if (e.from == addr || e.to == addr) {
                CallGraphEdge edge;
                edge.from = e.from;
                edge.to = e.to;
                result.push_back(edge);
            }
        }
    } else {
        // Return all edges
        for (const auto &e : allEdges) {
            CallGraphEdge edge;
            edge.from = e.from;
            edge.to = e.to;
            result.push_back(edge);
        }
    }

    // Stage 6.3: safe maximum for limit=0 (means "all")
    if (limit > 0 && result.size() > limit) {
        result.resize(limit);
    } else if (limit == 0 && result.size() > AgentLimits::MAX_CALL_GRAPH_LIMIT) {
        result.resize(AgentLimits::MAX_CALL_GRAPH_LIMIT);
    }

    std::ostringstream oss;
    if (address.has_value()) {
        oss << "addr=" << std::hex << address.value();
    } else {
        oss << "addr=all";
    }
    oss << " limit=" << std::dec << limit;
    log_.record("getCallGraph", oss.str(),
                std::to_string(result.size()) + " edges", elapsedMs(t0));

    return AgentApiResult<std::vector<CallGraphEdge>>::ok(std::move(result));
}

// ---------------------------------------------------------------------------
// Debug State (Stage 6.1 §21)
// ---------------------------------------------------------------------------

AgentApiResult<DebugStateResult> AgentApi::getDebugState()
{
    auto t0 = std::chrono::steady_clock::now();

    DebugStateResult result;
    result.running = !backend_.isPaused();
    result.cpu = backend_.getCpuState();
    result.breakpoints = backend_.getBreakpoints();

    // Disassemble current instruction
    auto readByte = [this](uint16_t addr) -> uint8_t {
        return backend_.readMemory(addr);
    };
    DisassembledInstruction di = ::disassemble(result.cpu.pc, readByte);
    result.current_instruction = di.text;

    // Look up function name at PC
    const auto &db = backend_.symbolDatabase();
    const DebugSymbol *sym = db.findSymbol(result.cpu.pc);
    if (sym) {
        result.current_function = sym->name;
    }

    std::ostringstream oss;
    oss << "pc=" << std::hex << result.cpu.pc
        << " running=" << result.running
        << " bps=" << std::dec << result.breakpoints.size();
    log_.record("getDebugState", "", oss.str(), elapsedMs(t0));

    return AgentApiResult<DebugStateResult>::ok(std::move(result));
}

// ---------------------------------------------------------------------------
// ROM Database (Stage 6.11)
// ---------------------------------------------------------------------------

namespace {

// Convert RdbObject → RdbObjectResult (agent-facing type).
RdbObjectResult rdbObjectToResult(const RdbObject &obj)
{
    RdbObjectResult r;
    r.address = obj.address;
    r.type    = rdbObjectTypeToString(obj.type);
    r.name    = obj.name;
    r.size    = obj.size;
    r.hasSize = obj.hasSize;
    r.comment = normalizeRdbText(obj.comment);   // R2: never emit a break into tool output
    r.links   = obj.links;
    // Flatten typed properties to string representation.
    for (const auto &kv : obj.properties) {
        switch (kv.second.type) {
            case RdbPropertyValue::Type::String:
                r.properties[kv.first] = normalizeRdbText(kv.second.stringValue);
                break;
            case RdbPropertyValue::Type::Integer:
                r.properties[kv.first] = std::to_string(kv.second.intValue);
                break;
            case RdbPropertyValue::Type::Boolean:
                r.properties[kv.first] = kv.second.boolValue ? "true" : "false";
                break;
            case RdbPropertyValue::Type::Double:
                r.properties[kv.first] = std::to_string(kv.second.doubleValue);
                break;
        }
    }
    return r;
}

} // anonymous namespace

AgentApiResult<RdbInfoResult> AgentApi::getRdbInfo()
{
    auto t0 = std::chrono::steady_clock::now();
    const auto &rdb = backend_.rdbController();

    RdbInfoResult info;
    info.path         = rdb.getPath();
    info.platform     = rdb.getPlatform();
    info.version      = rdb.getVersion();
    info.loaded       = rdb.isLoaded();
    info.dirty        = rdb.isDirty();
    info.existsOnDisk = rdb.existsOnDisk();
    info.objectCount  = rdb.objectCount();

    RdbRomIdentity rom = rdb.getRomIdentity();
    info.romFile   = rom.file;
    info.romSize   = rom.size;
    info.romSha256 = rom.sha256;

    std::ostringstream oss;
    oss << "objects=" << info.objectCount
        << " dirty=" << info.dirty
        << " platform=" << info.platform;
    log_.record("getRdbInfo", "", oss.str(), elapsedMs(t0));

    return AgentApiResult<RdbInfoResult>::ok(std::move(info));
}

AgentApiResult<std::vector<RdbObjectResult>> AgentApi::listRdbObjects(size_t limit)
{
    auto t0 = std::chrono::steady_clock::now();
    const auto &rdb = backend_.rdbController();

    auto all = rdb.listObjects();

    size_t count;
    if (limit == 0) {
        count = std::min(all.size(), AgentLimits::MAX_RDB_OBJECTS_LIMIT);
    } else {
        count = std::min(limit, all.size());
    }

    std::vector<RdbObjectResult> result;
    result.reserve(count);
    for (size_t i = 0; i < count; ++i) {
        result.push_back(rdbObjectToResult(all[i]));
    }

    log_.record("listRdbObjects", "",
                std::to_string(result.size()) + " / " + std::to_string(all.size()) + " objects",
                elapsedMs(t0));

    return AgentApiResult<std::vector<RdbObjectResult>>::ok(std::move(result));
}

AgentApiResult<RdbObjectResult> AgentApi::getRdbObject(uint16_t address)
{
    auto t0 = std::chrono::steady_clock::now();
    const auto &rdb = backend_.rdbController();

    const RdbObject *obj = rdb.getObject(address);
    if (!obj) {
        log_.record("getRdbObject", "addr=" + std::to_string(address),
                    "not found", elapsedMs(t0), false, "object not found");
        return AgentApiResult<RdbObjectResult>::fail(
            ErrorCode::NotFound, "No RDB object at address");
    }

    RdbObjectResult r = rdbObjectToResult(*obj);

    std::ostringstream oss;
    oss << "addr=" << std::hex << address << " name=" << r.name;
    log_.record("getRdbObject", oss.str(), "found", elapsedMs(t0));

    return AgentApiResult<RdbObjectResult>::ok(std::move(r));
}

AgentApiResult<RdbObjectResult> AgentApi::findRdbObject(const std::string &name)
{
    auto t0 = std::chrono::steady_clock::now();
    const auto &rdb = backend_.rdbController();

    const RdbObject *obj = rdb.findObject(name);
    if (!obj) {
        log_.record("findRdbObject", "name=" + name,
                    "not found", elapsedMs(t0), false, "object not found");
        return AgentApiResult<RdbObjectResult>::fail(
            ErrorCode::NotFound, "No RDB object with name '" + name + "'");
    }

    RdbObjectResult r = rdbObjectToResult(*obj);

    std::ostringstream oss;
    oss << "name=" << name << " addr=" << std::hex << obj->address;
    log_.record("findRdbObject", oss.str(), "found", elapsedMs(t0));

    return AgentApiResult<RdbObjectResult>::ok(std::move(r));
}

AgentApiResult<void> AgentApi::addRdbObject(uint16_t address, const std::string &name,
                                             const std::string &type, uint32_t size)
{
    auto t0 = std::chrono::steady_clock::now();
    auto &rdb = backend_.rdbController();

    RdbObject obj;
    obj.address = address;
    obj.type    = rdbObjectTypeFromString(type);
    obj.name    = name;
    obj.size    = size;
    obj.hasSize = (size > 0);

    if (!rdb.addObject(obj)) {
        log_.record("addRdbObject", "addr=" + std::to_string(address),
                    "failed", elapsedMs(t0), false, "address already exists");
        return AgentApiResult<void>::fail(
            ErrorCode::InvalidArgument, "RDB object already exists at this address");
    }

    std::ostringstream oss;
    oss << "addr=" << std::hex << address << " name=" << name << " type=" << type;
    log_.record("addRdbObject", oss.str(), "ok", elapsedMs(t0));

    return AgentApiResult<void>::ok();
}

AgentApiResult<void> AgentApi::updateRdbObject(uint16_t address, const std::string &name,
                                                const std::string &type,
                                                uint32_t size, bool hasSize)
{
    auto t0 = std::chrono::steady_clock::now();
    auto &rdb = backend_.rdbController();

    const RdbObject *existing = rdb.getObject(address);
    if (!existing) {
        log_.record("updateRdbObject", "addr=" + std::to_string(address),
                    "not found", elapsedMs(t0), false, "object not found");
        return AgentApiResult<void>::fail(
            ErrorCode::NotFound, "No RDB object at address");
    }

    RdbObject updated = *existing;
    updated.name    = name;
    updated.type    = rdbObjectTypeFromString(type);
    updated.size    = size;
    updated.hasSize = hasSize;

    if (!rdb.updateObject(updated)) {
        log_.record("updateRdbObject", "addr=" + std::to_string(address),
                    "failed", elapsedMs(t0), false, "update failed");
        return AgentApiResult<void>::fail(
            ErrorCode::OperationFailed, "Failed to update RDB object");
    }

    std::ostringstream oss;
    oss << "addr=" << std::hex << address << " name=" << name << " type=" << type;
    log_.record("updateRdbObject", oss.str(), "ok", elapsedMs(t0));

    return AgentApiResult<void>::ok();
}

AgentApiResult<void> AgentApi::removeRdbObject(uint16_t address)
{
    auto t0 = std::chrono::steady_clock::now();
    auto &rdb = backend_.rdbController();

    if (!rdb.removeObject(address)) {
        log_.record("removeRdbObject", "addr=" + std::to_string(address),
                    "not found", elapsedMs(t0), false, "object not found");
        return AgentApiResult<void>::fail(
            ErrorCode::NotFound, "No RDB object at address");
    }

    std::ostringstream oss;
    oss << "addr=" << std::hex << address;
    log_.record("removeRdbObject", oss.str(), "ok", elapsedMs(t0));

    return AgentApiResult<void>::ok();
}

AgentApiResult<void> AgentApi::setRdbComment(uint16_t address, const std::string &comment)
{
    auto t0 = std::chrono::steady_clock::now();
    auto &rdb = backend_.rdbController();

    if (!rdb.setComment(address, comment)) {
        log_.record("setRdbComment", "addr=" + std::to_string(address),
                    "not found", elapsedMs(t0), false, "object not found");
        return AgentApiResult<void>::fail(
            ErrorCode::NotFound, "No RDB object at address");
    }

    std::ostringstream oss;
    oss << "addr=" << std::hex << address << " comment=\"" << comment << "\"";
    log_.record("setRdbComment", oss.str(), "ok", elapsedMs(t0));

    return AgentApiResult<void>::ok();
}

AgentApiResult<void> AgentApi::setRdbProperty(uint16_t address,
                                               const std::string &propName,
                                               const std::string &propValue)
{
    auto t0 = std::chrono::steady_clock::now();
    auto &rdb = backend_.rdbController();

    RdbPropertyValue val = RdbPropertyValue::fromString(propValue);
    if (!rdb.setProperty(address, propName, val)) {
        log_.record("setRdbProperty", "addr=" + std::to_string(address),
                    "not found", elapsedMs(t0), false, "object not found");
        return AgentApiResult<void>::fail(
            ErrorCode::NotFound, "No RDB object at address");
    }

    std::ostringstream oss;
    oss << "addr=" << std::hex << address << " " << propName << "=\"" << propValue << "\"";
    log_.record("setRdbProperty", oss.str(), "ok", elapsedMs(t0));

    return AgentApiResult<void>::ok();
}

AgentApiResult<void> AgentApi::saveRdb()
{
    auto t0 = std::chrono::steady_clock::now();
    auto &rdb = backend_.rdbController();

    if (!rdb.save()) {
        log_.record("saveRdb", rdb.getPath(),
                    "failed", elapsedMs(t0), false, "save failed");
        return AgentApiResult<void>::fail(
            ErrorCode::OperationFailed, "Failed to save RDB");
    }

    log_.record("saveRdb", rdb.getPath(), "ok", elapsedMs(t0));

    return AgentApiResult<void>::ok();
}

AgentApiResult<void> AgentApi::reloadRdb()
{
    auto t0 = std::chrono::steady_clock::now();
    auto &rdb = backend_.rdbController();

    if (!rdb.reload()) {
        log_.record("reloadRdb", rdb.getPath(),
                    "failed", elapsedMs(t0), false, "reload failed");
        return AgentApiResult<void>::fail(
            ErrorCode::OperationFailed, "Failed to reload RDB");
    }

    log_.record("reloadRdb", rdb.getPath(), "ok", elapsedMs(t0));

    return AgentApiResult<void>::ok();
}

// ---------------------------------------------------------------------------
// RDB Links (Stage 6.13)
// ---------------------------------------------------------------------------

AgentApiResult<void> AgentApi::addRdbLink(uint16_t sourceAddress, uint16_t targetAddress)
{
    auto t0 = std::chrono::steady_clock::now();
    auto &rdb = backend_.rdbController();

    // Check source object exists
    if (!rdb.getObject(sourceAddress)) {
        std::ostringstream oss;
        oss << "source=" << std::hex << sourceAddress;
        log_.record("addRdbLink", oss.str(), "not found", elapsedMs(t0),
                    false, "source object not found");
        return AgentApiResult<void>::fail(
            ErrorCode::NotFound, "No RDB object at source address");
    }

    // addLink is idempotent — returns false only if source not found
    rdb.addLink(sourceAddress, targetAddress);

    std::ostringstream oss;
    oss << std::hex << sourceAddress << " -> " << targetAddress;
    log_.record("addRdbLink", oss.str(), "ok", elapsedMs(t0));

    return AgentApiResult<void>::ok();
}

AgentApiResult<void> AgentApi::removeRdbLink(uint16_t sourceAddress, uint16_t targetAddress)
{
    auto t0 = std::chrono::steady_clock::now();
    auto &rdb = backend_.rdbController();

    // Check source object exists
    if (!rdb.getObject(sourceAddress)) {
        std::ostringstream oss;
        oss << "source=" << std::hex << sourceAddress;
        log_.record("removeRdbLink", oss.str(), "not found", elapsedMs(t0),
                    false, "source object not found");
        return AgentApiResult<void>::fail(
            ErrorCode::NotFound, "No RDB object at source address");
    }

    // Check link exists
    if (!rdb.hasLink(sourceAddress, targetAddress)) {
        std::ostringstream oss;
        oss << std::hex << sourceAddress << " -> " << targetAddress;
        log_.record("removeRdbLink", oss.str(), "not found", elapsedMs(t0),
                    false, "link not found");
        return AgentApiResult<void>::fail(
            ErrorCode::NotFound, "Link does not exist");
    }

    rdb.removeLink(sourceAddress, targetAddress);

    std::ostringstream oss;
    oss << std::hex << sourceAddress << " -> " << targetAddress;
    log_.record("removeRdbLink", oss.str(), "ok", elapsedMs(t0));

    return AgentApiResult<void>::ok();
}

AgentApiResult<std::vector<uint16_t>> AgentApi::getRdbLinks(uint16_t sourceAddress)
{
    auto t0 = std::chrono::steady_clock::now();
    auto &rdb = backend_.rdbController();

    // Check source object exists
    if (!rdb.getObject(sourceAddress)) {
        std::ostringstream oss;
        oss << "source=" << std::hex << sourceAddress;
        log_.record("getRdbLinks", oss.str(), "not found", elapsedMs(t0),
                    false, "source object not found");
        return AgentApiResult<std::vector<uint16_t>>::fail(
            ErrorCode::NotFound, "No RDB object at source address");
    }

    std::vector<uint16_t> links = rdb.getLinks(sourceAddress);
    // Ensure stable ascending order
    std::sort(links.begin(), links.end());

    std::ostringstream oss;
    oss << "source=" << std::hex << sourceAddress << " links=" << links.size();
    log_.record("getRdbLinks", oss.str(), "ok", elapsedMs(t0));

    return AgentApiResult<std::vector<uint16_t>>::ok(std::move(links));
}

// ---------------------------------------------------------------------------
// Agent log
// ---------------------------------------------------------------------------

const AgentLog &AgentApi::log() const
{
    return log_;
}

void AgentApi::clearLog()
{
    log_.clear();
}

// ---------------------------------------------------------------------------
// Internal: disassembleFunction
//
// Linear disassembly starting at 'address' until:
//   - RET (0xC9) or HLT (0x76) instruction
//   - Unconditional JMP (0xC3) — code after is unreachable (Section 16)
//   - A known symbol is encountered (other than the start address)
//   - Maximum function size (4096 bytes) reached
// ---------------------------------------------------------------------------

std::vector<FunctionContext::Instruction>
AgentApi::disassembleFunction(uint16_t address)
{
    std::vector<FunctionContext::Instruction> result;

    auto readByte = [this](uint16_t addr) -> uint8_t {
        return backend_.readMemory(addr);
    };

    const auto &db = backend_.symbolDatabase();
    static const uint16_t MAX_FUNC_SIZE = 4096;

    uint16_t pc = address;
    for (uint16_t bytesDone = 0; bytesDone < MAX_FUNC_SIZE; ) {
        // Stop if we hit a known symbol (other than the entry point)
        if (pc != address && db.findSymbol(pc) != nullptr) {
            break;
        }

        DisassembledInstruction di = ::disassemble(pc, readByte);

        FunctionContext::Instruction fi;
        fi.address = di.address;
        fi.text = di.text;
        fi.length = di.length;
        for (int i = 0; i < 3; ++i) fi.bytes[i] = di.bytes[i];
        result.push_back(fi);

        bytesDone += di.length;

        // Stop after RET or HLT
        if (di.opcode == 0xC9 || di.opcode == 0x76) {
            break;
        }

        // Stop after unconditional JMP (0xC3) — code after is unreachable
        if (di.opcode == 0xC3) {
            break;
        }

        // Advance PC
        uint16_t nextPc = pc + di.length;
        if (nextPc <= pc) break;  // overflow guard
        pc = nextPc;
    }

    return result;
}

// ---------------------------------------------------------------------------
// Internal: estimateFunctionSize
// ---------------------------------------------------------------------------

uint16_t AgentApi::estimateFunctionSize(uint16_t address)
{
    auto instrs = disassembleFunction(address);
    if (instrs.empty()) return 0;

    auto &last = instrs.back();
    return static_cast<uint16_t>(
        (last.address + last.length) - address);
}

// ---------------------------------------------------------------------------
// Internal: collectTraceEvents
//
// Collect memory/IO/VRAM events from history that fall within the
// instruction sequence range [startSeq, endSeq).
//
// Memory events have instructionSequence — the sequence number of the
// instruction that caused the access.  IO events similarly.
//
// For VRAM: writes to 0xC000-0xC0FF are memory writes, so they appear
// in memHistory.  We filter them out separately.
// ---------------------------------------------------------------------------

void AgentApi::collectTraceEvents(
    uint64_t startSeq, uint64_t endSeq,
    std::vector<TraceMemoryAccess> &memReads,
    std::vector<TraceMemoryAccess> &memWrites,
    std::vector<TraceIoAccess> &ioReads,
    std::vector<TraceIoAccess> &ioWrites,
    std::vector<TraceVramWrite> &vramWrites)
{
    // --- Memory events ---
    auto memHistory = backend_.memoryHistorySnapshot();
    for (const auto &ev : memHistory) {
        if (ev.instructionSequence < startSeq || ev.instructionSequence >= endSeq)
            continue;

        // We need the PC of the instruction that caused this access.
        // The instruction history has pcBefore for each sequence number.
        // For efficiency, we look it up from the instruction trace.
        // But we don't have a direct mapping here — we'll use the
        // instruction history to find the PC.
        //
        // Actually, MemoryAccessEvent doesn't have PC directly.
        // The PC is the instruction at instructionSequence.
        // We'll find it from instructionHistorySnapshot.
        // (This is done in traceFunction where we have the instr trace.)
        //
        // For now, record with address and type.  PC attribution
        // requires cross-referencing with instruction history.

        if (ev.type == MemoryAccessType::Write) {
            // Check if VRAM (0xC000 - 0xC0FF)
            if (ev.virt >= 0xC000 && ev.virt < 0xC100) {
                TraceVramWrite vw;
                vw.address = ev.virt;
                vw.value = ev.value;
                // PC will be filled by the caller
                vramWrites.push_back(vw);
            } else {
                TraceMemoryAccess ma;
                ma.address = ev.virt;
                ma.type = TraceMemoryAccess::Write;
                ma.value = ev.value;
                memWrites.push_back(ma);
            }
        } else if (ev.type == MemoryAccessType::Read) {
            TraceMemoryAccess ma;
            ma.address = ev.virt;
            ma.type = TraceMemoryAccess::Read;
            ma.value = ev.value;
            memReads.push_back(ma);
        }
        // Skip Fetch events (opcode fetches are not data accesses)
    }

    // --- IO events ---
    auto ioHistory = backend_.ioHistorySnapshot();
    for (const auto &ev : ioHistory) {
        if (ev.instructionSequence < startSeq || ev.instructionSequence >= endSeq)
            continue;

        TraceIoAccess io;
        io.port = ev.port;
        io.isOutput = (ev.type == IoAccessType::Out);
        io.value = ev.value;

        if (ev.type == IoAccessType::Out) {
            ioWrites.push_back(io);
        } else {
            ioReads.push_back(io);
        }
    }
}

// ---------------------------------------------------------------------------
// High-level: getFunctionContext (Stage 6.3: AgentApiResult)
// ---------------------------------------------------------------------------

AgentApiResult<FunctionContext> AgentApi::getFunctionContext(uint16_t address)
{
    auto t0 = std::chrono::steady_clock::now();
    ensureXrefsBuilt();
    FunctionContext ctx;
    ctx.address = address;

    // 1. Name and comment from SymbolDatabase (read-only)
    const auto &db = backend_.symbolDatabase();
    const DebugSymbol *sym = db.findSymbol(address);
    if (sym) {
        ctx.name = sym->name;
        ctx.comment = normalizeRdbText(sym->comment);   // R2
    } else {
        ctx.name = SymbolDatabase::autoName(address);
    }

    // 2. Disassemble function (static — from Disassembler)
    ctx.instructions = disassembleFunction(address);
    if (!ctx.instructions.empty()) {
        auto &last = ctx.instructions.back();
        ctx.size = static_cast<uint16_t>(
            (last.address + last.length) - address);
    }

    // 3. Callers — xrefs TO this address (filter for CALL instructions)
    auto xrefs = db.xrefsTo(address);
    for (const auto &xr : xrefs) {
        uint8_t opcode = backend_.readMemory(xr.from);
        bool isCall = (opcode == 0xCD ||  // CALL
                       opcode == 0xC4 || opcode == 0xCC ||
                       opcode == 0xD4 || opcode == 0xDC ||
                       opcode == 0xE4 || opcode == 0xEC ||
                       opcode == 0xF4 || opcode == 0xFC);
        if (isCall) {
            ctx.callers.push_back(xr.from);
        }
    }

    // 4. Callees — scan function instructions for CALL targets
    for (const auto &instr : ctx.instructions) {
        uint8_t opcode = instr.bytes[0];
        bool isCall = (opcode == 0xCD ||
                       opcode == 0xC4 || opcode == 0xCC ||
                       opcode == 0xD4 || opcode == 0xDC ||
                       opcode == 0xE4 || opcode == 0xEC ||
                       opcode == 0xF4 || opcode == 0xFC);
        if (isCall && instr.length == 3) {
            uint16_t target = static_cast<uint16_t>(
                instr.bytes[1] | (instr.bytes[2] << 8));
            ctx.callees.push_back(target);
        }
        // RST instructions
        bool isRst = (opcode == 0xC7 || opcode == 0xCF ||
                      opcode == 0xD7 || opcode == 0xDF ||
                      opcode == 0xE7 || opcode == 0xEF ||
                      opcode == 0xF7 || opcode == 0xFF);
        if (isRst) {
            uint16_t target = static_cast<uint16_t>(opcode & 0x38);
            ctx.callees.push_back(target);
        }
    }

    // 5. Dynamic data: no trace available by default
    //    Memory/IO/VRAM fields remain empty with DataSource::Unknown.
    //    If a trace was previously run, the caller can populate these
    //    from the TraceResult.
    ctx.memorySource = DataSource::Unknown;
    ctx.ioSource = DataSource::Unknown;
    ctx.vramSource = DataSource::Unknown;
    ctx.stackBehavior = FunctionContext::Unknown;

    std::ostringstream oss;
    oss << "addr=" << std::hex << address
        << " size=" << std::dec << ctx.size
        << " instrs=" << ctx.instructions.size()
        << " callers=" << ctx.callers.size()
        << " callees=" << ctx.callees.size();
    log_.record("getFunctionContext", oss.str(), ctx.name, elapsedMs(t0));

    return AgentApiResult<FunctionContext>::ok(std::move(ctx));
}

// ---------------------------------------------------------------------------
// High-level: traceFunction — REAL execution experiment (Stage 6.3: AgentApiResult)
// ---------------------------------------------------------------------------

AgentApiResult<TraceResult> AgentApi::traceFunction(uint16_t address)
{
    auto t0 = std::chrono::steady_clock::now();
    TraceResult result;
    result.entryPc = address;

    // 1. Set temporary breakpoint at function entry
    auto bpResult = backend_.requestAddBreakpoint(address);
    if (!bpResult.success) {
        // Breakpoint already exists — that's OK for tracing
        // (the function address already has a breakpoint)
    }

    // 2. Run until breakpoint hit
    // Stage 5.3.3.2: No polling — wait for Emulation Thread pause via future.
    auto pauseFuture = backend_.requestRunFuture();
    pauseFuture.wait();  // blocks until breakpoint hit or emulation pauses

    if (!backend_.isPaused()) {
        // Failed to reach function entry
        backend_.requestRemoveBreakpoint(address);
        result.exitReason = ExitReason::Timeout;
        log_.record("traceFunction", "addr=" + std::to_string(address),
                     "failed to reach function entry", elapsedMs(t0));
        return AgentApiResult<TraceResult>::fail(
            ErrorCode::Timeout, "Failed to reach function entry");
    }

    // 3. Execute trace — step through function collecting events
    IDebugBackend::TraceExecutionParams params;
    params.startPc = address;
    params.maxInstructions = 10000;
    params.stopOnRet = true;
    params.stopOnCallerReturn = false;

    auto traceExec = backend_.requestExecuteTrace(params).get();

    // 4. Remove temporary breakpoint
    backend_.requestRemoveBreakpoint(address);

    // 5. Build TraceResult from execution data
    result.exitPc = traceExec.exitPc;
    result.entrySp = traceExec.entrySp;
    result.exitSp = traceExec.exitSp;
    result.minSp = traceExec.minSp;
    result.maxSp = traceExec.maxSp;
    result.instructionCount = traceExec.instructionsExecuted;
    result.executionCount = 1;  // single trace invocation
    result.exitReason = traceExec.exitReason;

    // 6. Collect events from history using sequence range
    auto instrTrace = backend_.instructionHistorySnapshot();
    auto memHistory = backend_.memoryHistorySnapshot();
    auto ioHistory = backend_.ioHistorySnapshot();

    // Build PC lookup: sequence → pcBefore
    std::map<uint64_t, uint16_t> seqToPc;
    for (const auto &ie : instrTrace) {
        if (ie.sequence >= traceExec.startSequence &&
            ie.sequence < traceExec.endSequence) {
            seqToPc[ie.sequence] = ie.pcBefore;
            result.executedPcs.push_back(ie.pcBefore);
        }
    }

    // Memory events with PC attribution
    for (const auto &ev : memHistory) {
        if (ev.instructionSequence < traceExec.startSequence ||
            ev.instructionSequence >= traceExec.endSequence)
            continue;

        // Stage 5.3.3: proper hasPc tracking — PC=0000 is not Unknown
        bool hasPc = false;
        uint16_t pc = 0;
        auto it = seqToPc.find(ev.instructionSequence);
        if (it != seqToPc.end()) { pc = it->second; hasPc = true; }

        if (ev.type == MemoryAccessType::Write) {
            if (ev.virt >= 0xC000 && ev.virt < 0xC100) {
                // VRAM write
                TraceVramWrite vw;
                vw.pc = pc;
                vw.hasPc = hasPc;
                vw.address = ev.virt;
                vw.value = ev.value;
                result.vramWrites.push_back(vw);
            } else {
                TraceMemoryAccess ma;
                ma.pc = pc;
                ma.hasPc = hasPc;
                ma.address = ev.virt;
                ma.type = TraceMemoryAccess::Write;
                ma.value = ev.value;
                result.memoryWrites.push_back(ma);
            }
        } else if (ev.type == MemoryAccessType::Read) {
            TraceMemoryAccess ma;
            ma.pc = pc;
            ma.hasPc = hasPc;
            ma.address = ev.virt;
            ma.type = TraceMemoryAccess::Read;
            ma.value = ev.value;
            result.memoryReads.push_back(ma);
        }
    }

    // IO events with PC attribution
    for (const auto &ev : ioHistory) {
        if (ev.instructionSequence < traceExec.startSequence ||
            ev.instructionSequence >= traceExec.endSequence)
            continue;

        // Stage 5.3.3: proper hasPc tracking
        bool hasPc = false;
        uint16_t pc = 0;
        auto it = seqToPc.find(ev.instructionSequence);
        if (it != seqToPc.end()) { pc = it->second; hasPc = true; }

        TraceIoAccess io;
        io.pc = pc;
        io.hasPc = hasPc;
        io.port = ev.port;
        io.isOutput = (ev.type == IoAccessType::Out);
        io.value = ev.value;

        if (ev.type == IoAccessType::Out) {
            result.ioWrites.push_back(io);
        } else {
            result.ioReads.push_back(io);
        }
    }

    // Extract called functions from instruction trace
    std::set<uint16_t> calledSet;
    for (const auto &ie : instrTrace) {
        if (ie.sequence < traceExec.startSequence ||
            ie.sequence >= traceExec.endSequence)
            continue;

        uint8_t opcode = ie.opcode;
        bool isCall = (opcode == 0xCD ||
                       opcode == 0xC4 || opcode == 0xCC ||
                       opcode == 0xD4 || opcode == 0xDC ||
                       opcode == 0xE4 || opcode == 0xEC ||
                       opcode == 0xF4 || opcode == 0xFC);
        if (isCall && ie.length == 3) {
            uint16_t target = static_cast<uint16_t>(
                ie.operandBytes[0] | (ie.operandBytes[1] << 8));
            calledSet.insert(target);
        }

        // RST instructions
        bool isRst = (opcode == 0xC7 || opcode == 0xCF ||
                      opcode == 0xD7 || opcode == 0xDF ||
                      opcode == 0xE7 || opcode == 0xEF ||
                      opcode == 0xF7 || opcode == 0xFF);
        if (isRst) {
            uint16_t target = static_cast<uint16_t>(opcode & 0x38);
            calledSet.insert(target);
        }
    }
    result.calledFunctions.assign(calledSet.begin(), calledSet.end());

    // Stack depth tracking
    result.callDepth = 0;
    int depth = 0;
    for (const auto &ie : instrTrace) {
        if (ie.sequence < traceExec.startSequence ||
            ie.sequence >= traceExec.endSequence)
            continue;

        uint8_t opcode = ie.opcode;
        bool isCall = (opcode == 0xCD ||
                       opcode == 0xC4 || opcode == 0xCC ||
                       opcode == 0xD4 || opcode == 0xDC ||
                       opcode == 0xE4 || opcode == 0xEC ||
                       opcode == 0xF4 || opcode == 0xFC);
        bool isRst = (opcode == 0xC7 || opcode == 0xCF ||
                      opcode == 0xD7 || opcode == 0xDF ||
                      opcode == 0xE7 || opcode == 0xEF ||
                      opcode == 0xF7 || opcode == 0xFF);
        if (isCall || isRst) depth++;
        if (opcode == 0xC9) depth--;  // RET
    }
    result.callDepth = depth;

    std::ostringstream oss;
    oss << "addr=" << std::hex << address
        << " instrs=" << std::dec << result.instructionCount
        << " exit=" << static_cast<int>(result.exitReason)
        << " callees=" << result.calledFunctions.size();
    log_.record("traceFunction", oss.str(), "done", elapsedMs(t0));

    return AgentApiResult<TraceResult>::ok(std::move(result));
}
