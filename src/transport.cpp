#include "fleetlink/transport.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <random>
#include <stdexcept>
#include <tuple>
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
        sender_counters_.emplace(node, ChannelCounters{});
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
        auto& local = sender_counters_.at(message.header.id.sender);
        ++local.sent;
        if (!isOnline(message.header.id.sender) || !isOnline(recipient)) {
            ++counters_.lost;
            ++local.lost;
            logDelivery({current_tick_, recipient, message}, "msg_drop", LogLevel::WARN, "offline");
            continue;
        }
        auto random = deliveryRandom(config_.seed, message, recipient);
        if (chance(random, config_.loss)) {
            ++counters_.lost;
            ++local.lost;
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
                ++local.duplicates;
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
        const auto& left = lhs.message.header;
        const auto& right = rhs.message.header;
        return std::tie(lhs.tick, left.sent_tick, left.id.sender, left.id.sequence, lhs.recipient,
                        lhs.copy) < std::tie(rhs.tick, right.sent_tick, right.id.sender,
                                             right.id.sequence, rhs.recipient, rhs.copy);
    });

    const auto waiting =
        std::find_if(pending_.begin(), pending_.end(),
                     [tick](const auto& delivery) { return delivery.tick > tick; });
    current_tick_ = tick;
    for (auto delivery = pending_.begin(); delivery != waiting; ++delivery) {
        auto& local = sender_counters_.at(delivery->message.header.id.sender);
        if (!isOnline(delivery->recipient) || offline_.count(delivery->message.header.id.sender)) {
            ++counters_.lost;
            ++local.lost;
            logDelivery(*delivery, "msg_drop", LogLevel::WARN, "offline");
            continue;
        }
        inboxes_.at(delivery->recipient).push_back(delivery->message);
        ++counters_.delivered;
        ++local.delivered;
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

bool InMemoryTransport::isOnline(NodeId node) const
{
    if (!inboxes_.count(node)) {
        throw std::invalid_argument("Unknown node");
    }
    return !disconnected_.count(node) && !offline_.count(node);
}

void InMemoryTransport::setOnline(NodeId node, bool online)
{
    if (!inboxes_.count(node)) {
        throw std::invalid_argument("Unknown node");
    }
    if (online && disconnected_.count(node)) {
        throw std::logic_error("Cannot reconnect a permanently failed node");
    }
    if (online) {
        offline_.erase(node);
    } else {
        offline_.insert(node);
        inboxes_.at(node).clear();
    }
}

const ChannelCounters& InMemoryTransport::counters(NodeId sender) const
{
    const auto found = sender_counters_.find(sender);
    if (found == sender_counters_.end()) {
        throw std::invalid_argument("Unknown sender");
    }
    return found->second;
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
