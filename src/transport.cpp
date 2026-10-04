#include "fleetlink/transport.hpp"

#include <algorithm>
#include <stdexcept>
#include <utility>

namespace fleetlink {

InMemoryTransport::InMemoryTransport(std::vector<NodeId> nodes) : nodes_(std::move(nodes))
{
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
    const auto arrival = deliveryTick(current_tick_, 0);
    for (const auto recipient : recipients) {
        pending_.push_back({arrival, recipient, message});
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
        return lhs.recipient < rhs.recipient;
    });

    const auto waiting =
        std::find_if(pending_.begin(), pending_.end(),
                     [tick](const auto& delivery) { return delivery.tick > tick; });
    for (auto delivery = pending_.begin(); delivery != waiting; ++delivery) {
        inboxes_.at(delivery->recipient).push_back(delivery->message);
    }
    pending_.erase(pending_.begin(), waiting);
    current_tick_ = tick;
}

} // namespace fleetlink
