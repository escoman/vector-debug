#include "debug_memory.h"
#include "memory.h"

// ---------------------------------------------------------------------------
// RAII guard for temporarily disabling and restoring Memory::onread callback
// ---------------------------------------------------------------------------

namespace {
    class CallbackGuard {
    public:
        explicit CallbackGuard(Memory& memory)
            : memory_(memory)
            , saved_callback_(memory.onread)
        {
            memory_.onread = nullptr;
        }

        ~CallbackGuard() {
            memory_.onread = saved_callback_;
        }

        // Non-copyable
        CallbackGuard(const CallbackGuard&) = delete;
        CallbackGuard& operator=(const CallbackGuard&) = delete;

    private:
        Memory& memory_;
        std::function<void(uint32_t, uint32_t, bool, uint8_t)> saved_callback_;
    };
}

// ---------------------------------------------------------------------------
// DebugMemoryAccess implementation
// ---------------------------------------------------------------------------

uint8_t DebugMemoryAccess::peek(Memory& memory, uint16_t address, bool stackrq)
{
    // RAII guard ensures callback is restored even if an exception occurs
    CallbackGuard guard(memory);
    
    // Use the real Memory::read() path which handles:
    // - bigram_select() for banking
    // - bootbytes for boot ROM
    // - tobank() for physical address translation
    return memory.read(address, stackrq);
}
