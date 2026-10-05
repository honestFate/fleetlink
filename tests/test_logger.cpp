#include "fleetlink/logger.hpp"

#include <doctest/doctest.h>
#include <nlohmann/json.hpp>

#include <array>
#include <chrono>
#include <future>
#include <sstream>
#include <stdexcept>
#include <vector>

using namespace fleetlink;

namespace {

std::vector<nlohmann::json> rows(const std::string& text)
{
    std::istringstream input(text);
    std::vector<nlohmann::json> result;
    for (std::string line; std::getline(input, line);) {
        result.push_back(nlohmann::json::parse(line));
    }
    return result;
}

LogRecord record(Tick tick, LogLevel level = LogLevel::INFO)
{
    return {tick, "M1", level, "test", {}};
}

class ThreadBuffer : public std::stringbuf {
public:
    std::thread::id writer;

protected:
    std::streamsize xsputn(const char* data, std::streamsize size) override
    {
        writer = std::this_thread::get_id();
        return std::stringbuf::xsputn(data, size);
    }
};

class BlockingBuffer : public std::stringbuf {
public:
    BlockingBuffer() : released_(release_.get_future()) {}
    std::future<void> entered() { return entered_.get_future(); }
    void release() { release_.set_value(); }

protected:
    std::streamsize xsputn(const char* data, std::streamsize size) override
    {
        if (!blocked_) {
            blocked_ = true;
            entered_.set_value();
            released_.wait();
        }
        return std::stringbuf::xsputn(data, size);
    }

private:
    bool blocked_ = false;
    std::promise<void> entered_;
    std::promise<void> release_;
    std::future<void> released_;
};

class FailingBuffer : public std::stringbuf {
public:
    bool fail_write = false;
    bool fail_flush = false;

protected:
    std::streamsize xsputn(const char* data, std::streamsize size) override
    {
        return fail_write ? 0 : std::stringbuf::xsputn(data, size);
    }
    int sync() override { return fail_flush ? -1 : std::stringbuf::sync(); }
};

} // namespace

TEST_CASE("logger writes typed escaped JSONL in a separate consumer thread")
{
    ThreadBuffer buffer;
    std::ostream output(&buffer);
    Logger logger(output, LogLevel::TRACE);
    LogRecord input{7,
                    "M\"1",
                    LogLevel::INFO,
                    "line\nbreak",
                    {{"text", std::string("quotes \" and newline\n")},
                     {"signed", std::int64_t{-3}},
                     {"unsigned", std::uint64_t{42}},
                     {"flag", true}}};
    logger.log(input);
    input.node = "changed";
    input.fields["text"] = std::string("changed");
    logger.close();

    CHECK(buffer.writer != std::this_thread::get_id());
    const auto records = rows(buffer.str());
    REQUIRE(records.size() == 1);
    CHECK(records[0]["tick"] == 7);
    CHECK(records[0]["node"] == "M\"1");
    CHECK(records[0]["lvl"] == "INFO");
    CHECK(records[0]["ev"] == "line\nbreak");
    CHECK(records[0]["text"] == "quotes \" and newline\n");
    CHECK(records[0]["signed"] == -3);
    CHECK(records[0]["unsigned"] == 42);
    CHECK(records[0]["flag"] == true);
    CHECK_NOTHROW(logger.close());
    CHECK_THROWS_AS(logger.log(record(8)), std::logic_error);
}

TEST_CASE("logger supports every level and applies the configured threshold")
{
    constexpr std::array<LogLevel, 5> levels{LogLevel::TRACE, LogLevel::DEBUG, LogLevel::INFO,
                                             LogLevel::WARN, LogLevel::ERROR};
    constexpr std::array<const char*, 5> names{"TRACE", "DEBUG", "INFO", "WARN", "ERROR"};
    std::ostringstream all;
    std::ostringstream filtered;
    Logger full(all, LogLevel::TRACE);
    Logger limited(filtered, LogLevel::INFO);
    for (std::size_t index = 0; index < levels.size(); ++index) {
        full.log(record(index, levels[index]));
        limited.log(record(index, levels[index]));
    }
    full.close();
    limited.close();
    const auto all_rows = rows(all.str());
    const auto filtered_rows = rows(filtered.str());
    REQUIRE(all_rows.size() == 5);
    REQUIRE(filtered_rows.size() == 3);
    for (std::size_t index = 0; index < all_rows.size(); ++index) {
        CHECK(all_rows[index]["lvl"] == names[index]);
        CHECK(all_rows[index]["tick"] == index);
    }
    for (std::size_t index = 0; index < filtered_rows.size(); ++index) {
        CHECK(filtered_rows[index]["lvl"] == names[index + 2]);
    }
}

TEST_CASE("destruction drains every accepted record in FIFO order")
{
    std::ostringstream output;
    {
        Logger logger(output);
        for (Tick tick = 0; tick < 500; ++tick) {
            logger.log(record(tick));
        }
    }
    const auto records = rows(output.str());
    REQUIRE(records.size() == 500);
    for (std::size_t index = 0; index < records.size(); ++index) {
        CHECK(records[index]["tick"] == index);
    }
}

TEST_CASE("concurrent producers preserve complete records and each producer's order")
{
    std::ostringstream output;
    Logger logger(output);
    std::vector<std::future<void>> producers;
    producers.reserve(4);
    for (unsigned producer = 0; producer < 4; ++producer) {
        producers.push_back(std::async(std::launch::async, [&logger, producer] {
            for (Tick index = 0; index < 100; ++index) {
                logger.log({index, "M" + std::to_string(producer), LogLevel::INFO, "test", {}});
            }
        }));
    }
    for (auto& producer : producers) {
        producer.get();
    }
    logger.close();

    const auto records = rows(output.str());
    REQUIRE(records.size() == 400);
    std::map<std::string, Tick> next;
    for (const auto& row : records) {
        const auto node = row.at("node").get<std::string>();
        CHECK(row.at("tick") == next[node]++);
    }
    CHECK(next.size() == 4);
    for (const auto& [node, count] : next) {
        INFO(node);
        CHECK(count == 100);
    }
}

TEST_CASE("a blocked output does not hold the producer queue mutex")
{
    BlockingBuffer buffer;
    auto entered = buffer.entered();
    std::ostream output(&buffer);
    Logger logger(output);
    logger.log(record(0));
    const auto consumer_status = entered.wait_for(std::chrono::seconds(5));
    if (consumer_status != std::future_status::ready) {
        buffer.release();
        FAIL("Consumer did not reach the output");
    }
    auto producer = std::async(std::launch::async, [&logger] {
        for (Tick tick = 1; tick <= 100; ++tick) {
            logger.log(record(tick));
        }
    });
    const auto producer_status = producer.wait_for(std::chrono::seconds(5));
    buffer.release();
    producer.get();
    logger.close();
    CHECK(producer_status == std::future_status::ready);
    CHECK(rows(buffer.str()).size() == 101);
}

TEST_CASE("output and flush failures are reported by close and subsequent logging")
{
    FailingBuffer buffer;
    bool throw_on_error = false;
    SUBCASE("write failure")
    {
        buffer.fail_write = true;
    }
    SUBCASE("flush failure")
    {
        buffer.fail_flush = true;
    }
    SUBCASE("stream exceptions")
    {
        buffer.fail_write = true;
        throw_on_error = true;
    }
    std::ostream output(&buffer);
    if (throw_on_error) {
        output.exceptions(std::ios::badbit | std::ios::failbit);
    }
    Logger logger(output);
    logger.log(record(0));
    CHECK_THROWS_AS(logger.close(), std::runtime_error);
    CHECK_THROWS_AS(logger.close(), std::runtime_error);
    CHECK_THROWS_AS(logger.log(record(1)), std::runtime_error);
}

TEST_CASE("concurrent close drains accepted records and rejects further producers")
{
    std::ostringstream output;
    Logger logger(output);
    std::promise<void> started;
    auto ready = started.get_future();
    auto producer = std::async(std::launch::async, [&logger, &started] {
        logger.log(record(0));
        started.set_value();
        Tick accepted = 1;
        try {
            for (; accepted < 1000; ++accepted) {
                logger.log(record(accepted));
            }
        } catch (const std::logic_error&) {
            // A close racing with enqueue may reject records not yet accepted
        }
        return accepted;
    });
    ready.wait();
    auto closer = std::async(std::launch::async, [&logger] { logger.close(); });
    logger.close();
    closer.get();
    const auto accepted = producer.get();
    const auto records = rows(output.str());
    REQUIRE(records.size() == accepted);
    for (std::size_t index = 0; index < records.size(); ++index) {
        CHECK(records[index]["tick"] == index);
    }
}

TEST_CASE("additional fields cannot overwrite mandatory log metadata")
{
    std::ostringstream output;
    Logger logger(output);
    for (const auto* name : {"tick", "node", "lvl", "ev"}) {
        auto invalid = record(0);
        invalid.fields.emplace(name, std::string("replacement"));
        CHECK_THROWS_AS(logger.log(invalid), std::invalid_argument);
    }
    logger.log(record(1));
    logger.close();
    const auto records = rows(output.str());
    REQUIRE(records.size() == 1);
    CHECK(records[0]["tick"] == 1);
}

TEST_CASE("flush publishes all preceding records without closing the logger")
{
    std::ostringstream output;
    Logger logger(output);
    logger.flush();
    logger.log(record(1));
    logger.flush();
    REQUIRE(rows(output.str()).size() == 1);
    logger.log(record(2));
    logger.flush();
    const auto records = rows(output.str());
    REQUIRE(records.size() == 2);
    CHECK(records[0]["tick"] == 1);
    CHECK(records[1]["tick"] == 2);
    logger.close();
    CHECK_NOTHROW(logger.flush());
}

TEST_CASE("flush reports consumer failures instead of waiting forever")
{
    FailingBuffer buffer;
    buffer.fail_flush = true;
    std::ostream output(&buffer);
    Logger logger(output);
    logger.log(record(1));
    CHECK_THROWS_AS(logger.flush(), std::runtime_error);
}

TEST_CASE("flush waits for a blocked consumer and leaves the logger usable")
{
    BlockingBuffer buffer;
    auto entered = buffer.entered();
    std::ostream output(&buffer);
    Logger logger(output);
    logger.log(record(1));
    entered.wait();
    auto flushing = std::async(std::launch::async, [&logger] { logger.flush(); });
    CHECK(flushing.wait_for(std::chrono::milliseconds(10)) == std::future_status::timeout);
    buffer.release();
    flushing.get();
    CHECK(rows(buffer.str()).size() == 1);
    logger.log(record(2));
    logger.close();
    CHECK(rows(buffer.str()).size() == 2);
}
