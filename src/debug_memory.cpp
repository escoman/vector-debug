#include "debug_memory.h"
#include "memory.h"

// ---------------------------------------------------------------------------
// DebugMemoryAccess implementation
// ---------------------------------------------------------------------------

uint8_t DebugMemoryAccess::peek(Memory& memory, uint16_t address, bool stackrq)
{
    // Callback-free read (V06C_DEBUGGER). It runs the same address translation
    // as Memory::read (bigram_select -> bootbytes -> tobank) but never touches
    // Memory::onread.
    //
    // The previous implementation cleared and restored Memory::onread via an
    // RAII guard on the transport/worker thread while the emulation thread was
    // inside Memory::read invoking that very std::function. That was a data
    // race on the std::function object: the check "if (onread)" passed, then
    // the guard reassigned it, leaving a torn/null invoker to be called
    // (SIGSEGV) or a partially destroyed functor (bad_function_call ->
    // std::terminate -> SIGABRT). See
    // docs/Known_Issue_MCP_Snapshot_onread_Race.md.
    return memory.peek(address, stackrq);
}
