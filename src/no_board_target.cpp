#include "no_board_target.h"
#include "memory.h"
#include "i8080.h"
#include "i8080_hal.h"
#include "debug_memory.h"
#include "rom_load_address.h"

#include <cstdio>
#include <fstream>

using namespace i8080cpu;

// ---------------------------------------------------------------------------
// Construction / destruction
// ---------------------------------------------------------------------------

NoBoardTarget::NoBoardTarget(Memory &memory)
    : memory_(memory)
{
}

NoBoardTarget::~NoBoardTarget()
{
}

// ---------------------------------------------------------------------------
// Memory access
// ---------------------------------------------------------------------------

uint8_t NoBoardTarget::readMemory(uint16_t addr)
{
    return memory_.read(addr, false);
}

uint8_t NoBoardTarget::peekMemory(uint16_t addr)
{
    return DebugMemoryAccess::peek(memory_, addr);
}

uint8_t NoBoardTarget::readMemoryRaw(uint16_t addr)
{
    return memory_.peek(addr, false);
}

void NoBoardTarget::writeMemory(uint16_t addr, uint8_t val)
{
    memory_.write(addr, val, false);
}

void NoBoardTarget::setMemoryCallbacks(MemoryReadCallback onRead,
                                       MemoryWriteCallback onWrite)
{
    if (onRead) {
        // Save previous callbacks for chaining
        prevOnRead_  = memory_.onread;
        prevOnWrite_ = memory_.onwrite;

        memory_.onread = [this, onRead](uint32_t virt, uint32_t phys,
                                        bool stack, uint8_t value) {
            // Stage 6.24: pass the CPU's PC at callback time (see debug_adapter.cpp).
            onRead(virt, phys, stack, value, static_cast<uint16_t>(i8080_pc()));
            if (prevOnRead_) prevOnRead_(virt, phys, stack, value);
        };

        memory_.onwrite = [this, onWrite](uint32_t virt, uint32_t phys,
                                          bool stack, uint8_t value) {
            onWrite(virt, phys, stack, value);
            if (prevOnWrite_) prevOnWrite_(virt, phys, stack, value);
        };
    } else {
        // Clear: restore previous callbacks
        memory_.onread  = prevOnRead_;
        memory_.onwrite = prevOnWrite_;
        prevOnRead_  = nullptr;
        prevOnWrite_ = nullptr;
    }
}

// ---------------------------------------------------------------------------
// CPU state
// ---------------------------------------------------------------------------

CpuState NoBoardTarget::getCpuState()
{
    CpuState s;
    s.pc    = static_cast<uint16_t>(i8080_pc());
    s.sp    = static_cast<uint16_t>(i8080_regs_sp());
    s.a     = static_cast<uint8_t>(i8080_regs_a());
    s.b     = static_cast<uint8_t>(i8080_regs_b());
    s.c     = static_cast<uint8_t>(i8080_regs_c());
    s.d     = static_cast<uint8_t>(i8080_regs_d());
    s.e     = static_cast<uint8_t>(i8080_regs_e());
    s.h     = static_cast<uint8_t>(i8080_regs_h());
    s.l     = static_cast<uint8_t>(i8080_regs_l());
    s.flags = static_cast<uint8_t>(i8080_regs_f());
    s.iff   = i8080_iff();
    s.cycles     = static_cast<uint32_t>(i8080_cycles());
    s.ei_pending = false;
    s.last_pc    = 0;
    return s;
}

void NoBoardTarget::writeCpuRegister(int reg, uint16_t val)
{
    // Register encoding matches DebugBackend::RegisterId enum:
    // 0=AF, 1=BC, 2=DE, 3=HL, 4=SP, 5=PC
    switch (reg) {
        case 0: // AF
            i8080_setreg_a((val >> 8) & 0xFF);
            i8080_setreg_f(val & 0xFF);
            break;
        case 1: // BC
            i8080_setreg_b((val >> 8) & 0xFF);
            i8080_setreg_c(val & 0xFF);
            break;
        case 2: // DE
            i8080_setreg_d((val >> 8) & 0xFF);
            i8080_setreg_e(val & 0xFF);
            break;
        case 3: // HL
            i8080_setreg_h((val >> 8) & 0xFF);
            i8080_setreg_l(val & 0xFF);
            break;
        case 4: // SP
            i8080_setreg_sp(val);
            break;
        case 5: // PC
            i8080_jump(val);
            break;
    }
}

// ---------------------------------------------------------------------------
// Execution control
// ---------------------------------------------------------------------------

void NoBoardTarget::stepInstruction()
{
    // Stage 6.24: instruction-boundary detection is now done inside
    // DebugBackend::onMemoryRead() using the pc passed with MemoryReadCallback.
    // No separate begin-callback is required here.
    int report_opcode = 0;
    i8080_instruction(&report_opcode);
}

void NoBoardTarget::executeFrame()
{
    // No Board — execute a single instruction as one "frame"
    stepInstruction();
}

void NoBoardTarget::reset(bool attachBoot)
{
    i8080_init();
    cpuInitialized_ = true;
}

// ---------------------------------------------------------------------------
// ROM / init
// ---------------------------------------------------------------------------

bool NoBoardTarget::loadRom(const std::string &path, uint32_t org)
{
    loadRomThread_ = std::this_thread::get_id();

    // Load ROM file using standard C++ I/O (no dependency on util.cpp)
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file.is_open()) {
        printf("NoBoardTarget::loadRom(): failed to load %s\n", path.c_str());
        return false;
    }
    auto size = file.tellg();
    file.seekg(0, std::ios::beg);
    std::vector<uint8_t> rom_data(size);
    file.read(reinterpret_cast<char*>(rom_data.data()), size);

    // Auto-detect load address from file extension if org not explicitly given
    if (org == 0 && path.find('.') != std::string::npos) {
        org = getRomLoadAddress(path);
    }

    printf("NoBoardTarget::loadRom(): loaded %s (%zu bytes) at %04x\n",
           path.c_str(), rom_data.size(), org);

    memory_.init_from_vector(rom_data, org);
    // LOADROM convention: set SP=0xc300 (standard Vector-06C stack pointer).
    // i8080_init() resets PC to 0 and clears flags (F=0x02).
    i8080_setreg_sp(0xc300);
    i8080_init();

    return true;
}

void NoBoardTarget::initCpu(uint16_t pc, uint16_t sp)
{
    i8080_jump(pc);
    i8080_setreg_sp(sp);
    i8080_init();
}
