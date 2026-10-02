#pragma once

#include "agent_types.h"

#include <mutex>
#include <string>
#include <vector>

// ---------------------------------------------------------------------------
// AgentLog — Stage 5.3
//
// Thread-safe journal of Agent API operations.
// Each entry records: timestamp, tool name, arguments, result, execution time.
// Allows replay and analysis of the AI agent's decision-making process.
// ---------------------------------------------------------------------------

class AgentLog
{
public:
    AgentLog() = default;

    // Record a single operation.
    void record(const std::string &tool,
                const std::string &args,
                const std::string &result,
                double timeMs);

    // Record with success/error info (Stage 5.3.1).
    void record(const std::string &tool,
                const std::string &args,
                const std::string &result,
                double timeMs,
                bool success,
                const std::string &error = "");

    // All entries (thread-safe snapshot).
    std::vector<AgentLogEntry> entries() const;

    // Number of entries.
    size_t size() const;

    // Clear all entries.
    void clear();

    // Get the last entry (returns nullptr if empty).
    AgentLogEntry lastEntry() const;

private:
    std::vector<AgentLogEntry> entries_;
    mutable std::mutex mutex_;
};
