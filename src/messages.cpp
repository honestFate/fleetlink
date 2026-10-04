#include "fleetlink/messages.hpp"

#include <algorithm>
#include <limits>
#include <stdexcept>
#include <tuple>

namespace fleetlink {

MessageKind messageKind(const Message& message)
{
    return std::visit([](const auto& payload) { return payload.kind; }, message.payload);
}

std::vector<NodeId> recipientsFor(const Message& message, const std::vector<NodeId>& nodes)
{
    auto recipients = nodes;
    std::sort(recipients.begin(), recipients.end());

    if (std::adjacent_find(recipients.begin(), recipients.end()) != recipients.end()) {
        throw std::invalid_argument("Duplicate node id");
    }
    if (!std::binary_search(recipients.begin(), recipients.end(), message.header.id.sender)) {
        throw std::invalid_argument("Unknown message sender");
    }

    if (message.header.recipient) {
        const auto recipient = *message.header.recipient;
        if (!std::binary_search(recipients.begin(), recipients.end(), recipient)) {
            throw std::invalid_argument("Unknown message recipient");
        }
        return {recipient};
    }

    const auto sender =
        std::lower_bound(recipients.begin(), recipients.end(), message.header.id.sender);
    recipients.erase(sender);
    return recipients;
}

bool messageBefore(const Message& lhs, const Message& rhs) noexcept
{
    const auto& left = lhs.header;
    const auto& right = rhs.header;
    return std::tie(left.sent_tick, left.id.sender, left.id.sequence) <
           std::tie(right.sent_tick, right.id.sender, right.id.sequence);
}

Tick deliveryTick(Tick sent_tick, Tick delay)
{
    const auto offset = std::max<Tick>(1, delay);
    if (sent_tick > std::numeric_limits<Tick>::max() - offset) {
        throw std::overflow_error("Delivery tick overflow");
    }
    return sent_tick + offset;
}

LamportTime nextLamportTime(LamportTime local, LamportTime received)
{
    const auto current = std::max(local, received);
    if (current == std::numeric_limits<LamportTime>::max()) {
        throw std::overflow_error("Lamport clock overflow");
    }
    return current + 1;
}

} // namespace fleetlink
