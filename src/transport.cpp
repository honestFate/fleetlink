#include "fleetlink/transport.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <random>
#include <stdexcept>
#include <utility>

namespace fleetlink {

namespace {

// Each recipient has its own deterministic draw, independent of send and roster order
std::mt19937_64 deliveryRandom(std::uint64_t seed, const Message& message, NodeId recipient)
{
    const auto sender = message.header.id.sender;
    const auto sequence = message.header.id.sequence;
    const auto tick = message.header.sent_tick;
    std::seed_seq words{
        static_cast<std::uint32_t>(seed),      static_cast<std::uint32_t>(seed >> 32),
        static_cast<std::uint32_t>(sender),    static_cast<std::uint32_t>(sender >> 32),
        static_cast<std::uint32_t>(sequence),  static_cast<std::uint32_t>(sequence >> 32),
        static_cast<std::uint32_t>(tick),      static_cast<std::uint32_t>(tick >> 32),
        static_cast<std::uint32_t>(recipient), static_cast<std::uint32_t>(recipient >> 32)};
    return std::mt19937_64(words);
}

bool chance(std::mt19937_64& random, double probability)
{
    // Avoid implementation-dependent uniform_real_distribution mappings
    return static_cast<double>(random() >> 11) / 9007199254740992.0 < probability;
}

Tick delay(std::mt19937_64& random, Tick minimum, Tick maximum)
{
    const auto range = maximum - minimum;
    if (range == std::numeric_limits<Tick>::max()) {
        return random();
    }
    const auto span = range + 1;
    const auto threshold = (Tick{0} - span) % span;
    auto value = random();
    while (value < threshold) {
        value = random();
    }
    return minimum + value % span;
}

} // namespace

InMemoryTransport::InMemoryTransport(std::vector<NodeId> nodes, ChannelConfig config,
                                     Logger* logger)
    : config_(config), logger_(logger), nodes_(std::move(nodes))
{
    if (!std::isfinite(config.loss) || config.loss < 0 || config.loss > 1 ||
        !std::isfinite(config.dup) || config.dup < 0 || config.dup > 1 ||
        config.delay_min > config.delay_max) {
        throw std::invalid_argument("Invalid channel configuration");
    }
    if (nodes_.empty()) {
        throw std::invalid_argument("Transport requires at least one node");
    }
    std::sort(nodes_.begin(), nodes_.end());
    if (std::adjacent_find(nodes_.begin(), nodes_.end()) != nodes_.end()) {
        throw std::invalid_argument("Duplicate node id");
    }
    for (const auto node : nodes_) {
        inboxes_.emplace(node, std::vector<Message>{});
    }
}

void InMemoryTransport::send(const Message& message)
{
    if (message.header.sent_tick != current_tick_) {
        throw std::invalid_argument("Message send tick does not match transport time");
    }
    const auto recipients = recipientsFor(message, nodes_);
    // Validate the entire configured range before changing queues or counters
    deliveryTick(current_tick_, config_.delay_max);
    for (const auto recipient : recipients) {
        ++counters_.sent;
        if (disconnected_.count(message.header.id.sender) || disconnected_.count(recipient)) {
            ++counters_.lost;
            logDelivery({current_tick_, recipient, message}, "msg_drop", LogLevel::WARN, "offline");
            continue;
        }
        auto random = deliveryRandom(config_.seed, message, recipient);
        if (chance(random, config_.loss)) {
            ++counters_.lost;
            logDelivery({current_tick_, recipient, message}, "msg_drop", LogLevel::WARN, "loss");
            continue;
        }
        const bool duplicate = chance(random, config_.dup);
        for (unsigned copy = 0; copy < (duplicate ? 2U : 1U); ++copy) {
            const auto arrival =
                deliveryTick(current_tick_, delay(random, config_.delay_min, config_.delay_max));
            pending_.push_back({arrival, recipient, message, copy});
            logDelivery(pending_.back(), "msg_schedule", LogLevel::TRACE);
            if (copy != 0) {
                ++counters_.duplicates;
                logDelivery(pending_.back(), "msg_duplicate", LogLevel::DEBUG);
            }
        }
    }
}

std::vector<Message> InMemoryTransport::receive(NodeId recipient)
{
    const auto inbox = inboxes_.find(recipient);
    if (inbox == inboxes_.end()) {
        throw std::invalid_argument("Unknown message recipient");
    }
    std::vector<Message> messages;
    messages.swap(inbox->second);
    return messages;
}

void InMemoryTransport::advanceTo(Tick tick)
{
    if (tick < current_tick_) {
        throw std::invalid_argument("Transport time cannot move backwards");
    }
    if (tick == current_tick_) {
        return;
    }

    std::sort(pending_.begin(), pending_.end(), [](const auto& lhs, const auto& rhs) {
        if (lhs.tick != rhs.tick) {
            return lhs.tick < rhs.tick;
        }
        if (messageBefore(lhs.message, rhs.message)) {
            return true;
        }
        if (messageBefore(rhs.message, lhs.message)) {
            return false;
        }
        if (lhs.recipient != rhs.recipient) {
            return lhs.recipient < rhs.recipient;
        }
        return lhs.copy < rhs.copy;
    });

    const auto waiting =
        std::find_if(pending_.begin(), pending_.end(),
                     [tick](const auto& delivery) { return delivery.tick > tick; });
    current_tick_ = tick;
    for (auto delivery = pending_.begin(); delivery != waiting; ++delivery) {
        if (disconnected_.count(delivery->recipient)) {
            ++counters_.lost;
            logDelivery(*delivery, "msg_drop", LogLevel::WARN, "offline");
            continue;
        }
        inboxes_.at(delivery->recipient).push_back(delivery->message);
        ++counters_.delivered;
        logDelivery(*delivery, "msg_deliver", LogLevel::TRACE);
    }
    pending_.erase(pending_.begin(), waiting);
}

void InMemoryTransport::disconnect(NodeId node)
{
    const auto inbox = inboxes_.find(node);
    if (inbox == inboxes_.end()) {
        throw std::invalid_argument("Unknown node to disconnect");
    }
    disconnected_.insert(node);
    inbox->second.clear();
}

void InMemoryTransport::logDelivery(const PendingDelivery& delivery, const char* event,
                                    LogLevel level, const char* reason)
{
    if (logger_ && logger_->enabled(level)) {
        logger_->log({current_tick_,
                      "CHANNEL",
                      level,
                      event,
                      {{"from", nodeName(delivery.message.header.id.sender)},
                       {"to", nodeName(delivery.recipient)},
                       {"seq", delivery.message.header.id.sequence},
                       {"sent_tick", delivery.message.header.sent_tick},
                       {"delivery_tick", delivery.tick},
                       {"copy", std::uint64_t{delivery.copy}},
                       {"reason", std::string(reason)}}});
    }
}

} // namespace fleetlink
