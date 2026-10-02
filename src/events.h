#pragma once

#include <cstdint>
#include <chrono>
#include "backend.h"  // CpuState

// ---------------------------------------------------------------------------
// Instruction event — recorded after every executed 8080 instruction
// ---------------------------------------------------------------------------

struct InstructionEvent
{
    uint64_t sequence;

    uint16_t pcBefore;
    uint16_t pcAfter;

    uint8_t opcode;
    uint8_t length;

    int cycles;

    // Instruction bytes at PC+1 and PC+2 (captured before execution).
    // Together with opcode, these form the full instruction bytes.
    // Correct even for self-modifying code (captured at execution time).
    uint8_t operandBytes[2] = {0, 0};

    CpuState before;
    CpuState after;
};

// ---------------------------------------------------------------------------
// Memory access event — recorded via Memory::onread / onwrite callbacks
// ---------------------------------------------------------------------------

enum class MemoryAccessType
{
    Fetch,
    Read,
    Write
};

struct MemoryAccessEvent
{
    uint64_t instructionSequence;

    MemoryAccessType type;

    uint16_t virt;
    uint32_t phys;

    uint8_t value;

    bool stack;
};

// ---------------------------------------------------------------------------
// I/O access event — recorded via IO::onread / onwrite callbacks
// ---------------------------------------------------------------------------

enum class IoAccessType
{
    In,
    Out
};

struct IoAccessEvent
{
    uint64_t instructionSequence;

    IoAccessType type;

    uint8_t port;
    uint8_t value;
};

// ---------------------------------------------------------------------------
// Per-address cumulative memory statistics (for future Memory Map)
// ---------------------------------------------------------------------------

struct MemoryStats
{
    uint64_t reads;
    uint64_t writes;

    uint64_t lastReadSequence;
    uint64_t lastWriteSequence;

    // Wall-clock timestamps for Live Map (Stage 5.2)
    using Clock = std::chrono::steady_clock;
    using TimePoint = std::chrono::steady_clock::time_point;

    TimePoint lastReadTime;
    TimePoint lastWriteTime;
};
