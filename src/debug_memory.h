#pragma once

#include <cstdint>

class Memory;

// ---------------------------------------------------------------------------
// DebugMemoryAccess
//
// Debugger-side memory peek adapter that reads memory without triggering
// instrumentation callbacks. Delegates to the callback-free Memory::peek()
// (V06C_DEBUGGER), which runs the real address translation path
// (bigram_select, bootbytes, tobank) without touching Memory::onread.
//
// This avoids duplicating the banking logic in the debugger while preventing
// side effects on instrumentation.
// ---------------------------------------------------------------------------

class DebugMemoryAccess {
public:
    // Read memory at the given virtual address without triggering the onread
    // callback. Uses the full address translation path: bigram_select ->
    // bootbytes -> tobank.
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
    //   Does not modify Memory::onread, so it is safe to call while the CPU is
    //   running (no callback race). As with any lock-free peek the byte may be
    //   captured mid-write, so use DebugBackend synchronization when a
    //   point-in-time consistent view is required.
    static uint8_t peek(Memory& memory, uint16_t address, bool stackrq = false);
};
