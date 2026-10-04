#include "fleetlink/logger.hpp"

#include <nlohmann/json.hpp>

#include <array>
#include <stdexcept>
#include <utility>

namespace fleetlink {

namespace {

std::string serialize(const LogRecord& record)
{
    constexpr std::array<const char*, 5> levels{"TRACE", "DEBUG", "INFO", "WARN", "ERROR"};
    nlohmann::json row{{"tick", record.tick},
                       {"node", record.node},
                       {"lvl", levels.at(static_cast<std::size_t>(record.level))},
                       {"ev", record.event}};
    for (const auto& [name, value] : record.fields) {
        row[name] = std::visit([](const auto& field) { return nlohmann::json(field); }, value);
    }
    return row.dump();
}

} // namespace

std::string nodeName(NodeId node)
{
    return node == ARM_NODE_ID ? "ARM" : "M" + std::to_string(node);
}

Logger::Logger(std::ostream& output, LogLevel minimum)
    : output_(output), minimum_(minimum), consumer_(&Logger::consume, this)
{}

Logger::~Logger()
{
    try {
        close();
    } catch (...) {
        // Destruction still joins the consumer, explicit close reports failures
    }
}

void Logger::log(LogRecord record)
{
    for (const auto& [name, value] : record.fields) {
        if (name == "tick" || name == "node" || name == "lvl" || name == "ev") {
            throw std::invalid_argument("Log fields cannot replace mandatory fields");
        }
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (error_) {
            std::rethrow_exception(error_);
        }
        if (closing_) {
            throw std::logic_error("Logger is closed");
        }
        if (!enabled(record.level)) {
            return;
        }
        queue_.push_back(std::move(record));
    }
    ready_.notify_one();
}

void Logger::close()
{
    std::lock_guard<std::mutex> closing_lock(close_mutex_);
    {
        std::lock_guard<std::mutex> lock(mutex_);
        closing_ = true;
    }
    ready_.notify_one();
    if (consumer_.joinable()) {
        consumer_.join();
    }
    std::lock_guard<std::mutex> lock(mutex_);
    if (error_) {
        std::rethrow_exception(error_);
    }
}

void Logger::consume() noexcept
{
    try {
        while (true) {
            std::deque<LogRecord> batch;
            {
                std::unique_lock<std::mutex> lock(mutex_);
                ready_.wait(lock, [this] { return closing_ || !queue_.empty(); });
                if (queue_.empty() && closing_) {
                    break;
                }
                batch.swap(queue_);
            }
            for (const auto& record : batch) {
                output_ << serialize(record) << '\n';
                if (!output_) {
                    throw std::runtime_error("Failed to write JSONL log");
                }
            }
            output_.flush();
            if (!output_) {
                throw std::runtime_error("Failed to flush JSONL log");
            }
        }
        output_.flush();
        if (!output_) {
            throw std::runtime_error("Failed to flush JSONL log");
        }
    } catch (...) {
        std::lock_guard<std::mutex> lock(mutex_);
        error_ = std::current_exception();
        closing_ = true;
        queue_.clear();
    }
}

} // namespace fleetlink
