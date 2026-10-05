#pragma once

#include "fleetlink/types.hpp"

#include <condition_variable>
#include <deque>
#include <exception>
#include <map>
#include <mutex>
#include <ostream>
#include <string>
#include <thread>
#include <variant>

namespace fleetlink {

std::string nodeName(NodeId node);

enum class LogLevel {
    TRACE,
    DEBUG,
    INFO,
    WARN,
    ERROR,
};

using LogValue = std::variant<std::string, std::int64_t, std::uint64_t, bool>;

struct LogRecord {
    Tick tick;
    std::string node;
    LogLevel level;
    std::string event;
    std::map<std::string, LogValue> fields;
};

class Logger {
public:
    // The stream must outlive the logger and be used only by its consumer until close
    explicit Logger(std::ostream& output, LogLevel minimum = LogLevel::INFO);
    ~Logger();
    Logger(const Logger&) = delete;
    Logger& operator=(const Logger&) = delete;

    bool enabled(LogLevel level) const noexcept { return level >= minimum_; }
    // Thread-safe enqueue, formatting and output happen in the consumer
    void log(LogRecord record);
    // Wait until all records submitted before this call have been written and flushed
    void flush();
    // Stop accepting records, drain, flush and join, then report any output error
    void close();

private:
    void consume() noexcept;

    std::ostream& output_;
    const LogLevel minimum_;
    std::mutex mutex_;
    std::condition_variable ready_;
    std::condition_variable flushed_;
    std::deque<LogRecord> queue_;
    std::uint64_t submitted_ = 0;
    std::uint64_t written_ = 0;
    bool closing_ = false;
    std::exception_ptr error_;
    std::mutex close_mutex_;
    std::thread consumer_;
};

} // namespace fleetlink
