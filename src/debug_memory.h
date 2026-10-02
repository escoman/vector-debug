#pragma once

#include <cstdint>

class Memory;

// ---------------------------------------------------------------------------
// DebugMemoryAccess
//
// Debugger-side memory peek adapter that reads memory without triggering
// instrumentation callbacks. Uses the real Memory::read() path to ensure
// correct address translation (bigram_select, bootbytes, tobank).
//
// This avoids duplicating the banking logic in the debugger while preventing
// side effects on instrumentation.
// ---------------------------------------------------------------------------

class DebugMemoryAccess {
public:
    // Read memory at the given virtual address without triggering onread callback.
    // Uses the full address translation path: bigram_select -> bootbytes -> tobank.
    //
    // Parameters:
    //   memory   - Reference to the Memory instance
    //   address  - Virtual address to read (0x0000-0xFFFF)
    //   stackrq  - Stack request flag (affects banking in stack mode)
    //
    // Returns:
    //   The byte value at the translated physical address
    //
    // Thread safety:
    //   Must not be called concurrently with CPU execution, as it temporarily
    //   modifies memory.onread. Use DebugBackend synchronization mechanisms.
    static uint8_t peek(Memory& memory, uint16_t address, bool stackrq = false);
};
