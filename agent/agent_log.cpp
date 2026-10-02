#include "agent_log.h"

// ---------------------------------------------------------------------------
// AgentLog implementation
// ---------------------------------------------------------------------------

void AgentLog::record(const std::string &tool,
                      const std::string &args,
                      const std::string &result,
                      double timeMs)
{
    AgentLogEntry entry;
    entry.timestamp = std::chrono::steady_clock::now();
    entry.tool = tool;
    entry.arguments = args;
    entry.result = result;
    entry.executionTimeMs = timeMs;

    std::lock_guard<std::mutex> lock(mutex_);
    entries_.push_back(std::move(entry));
}

void AgentLog::record(const std::string &tool,
                      const std::string &args,
                      const std::string &result,
                      double timeMs,
                      bool success,
                      const std::string &error)
{
    AgentLogEntry entry;
    entry.timestamp = std::chrono::steady_clock::now();
    entry.tool = tool;
    entry.arguments = args;
    entry.result = result;
    entry.executionTimeMs = timeMs;
    entry.success = success;
    entry.error = error;

    std::lock_guard<std::mutex> lock(mutex_);
    entries_.push_back(std::move(entry));
}

std::vector<AgentLogEntry> AgentLog::entries() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return entries_;
}

size_t AgentLog::size() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return entries_.size();
}

void AgentLog::clear()
{
    std::lock_guard<std::mutex> lock(mutex_);
    entries_.clear();
}

AgentLogEntry AgentLog::lastEntry() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (entries_.empty()) return {};
    return entries_.back();
}
