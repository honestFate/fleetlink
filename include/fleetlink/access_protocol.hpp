#pragma once

#include "fleetlink/messages.hpp"

#include <map>
#include <optional>
#include <set>
#include <vector>

namespace fleetlink {

// Local Ricart-Agrawala state for one machine and buffers of capacity one
class AccessProtocol {
public:
    // The fixed membership includes this machine and excludes ARM
    AccessProtocol(MachineId id, std::vector<MachineId> members);

    // Only one buffer may be requested or held at a time
    RequestId request(ResourceId resource, Tick tick);
    // Call only after physically leaving the buffer
    void release(ResourceId resource, Tick tick);
    void onMessage(const Message& message, Tick tick);

    bool isWaiting(ResourceId resource) const noexcept;
    // A logical reservation, independent of the machine's physical position
    bool hasPermission(ResourceId resource) const noexcept;

    // Drain and send at the current tick after processing the machines
    std::vector<Message> takeOutgoing();

private:
    MessageHeader makeHeader(Tick tick, std::optional<NodeId> recipient = std::nullopt);
    void onRequest(const Request& request, Tick tick);
    void onReply(MachineId sender, const Reply& reply);

    MachineId id_;
    std::vector<MachineId> members_;
    LamportTime clock_ = 0;
    SequenceNumber sequence_ = 0;
    std::optional<Request> request_;
    bool granted_ = false;
    std::set<MachineId> replies_;
    std::map<MachineId, RequestId> deferred_;
    std::vector<Message> outgoing_;
};

} // namespace fleetlink
