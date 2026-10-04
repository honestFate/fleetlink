#pragma once

#include "fleetlink/messages.hpp"

#include <map>
#include <vector>

namespace fleetlink {

class Transport {
public:
    virtual ~Transport() = default;
    virtual void send(const Message& message) = 0;
    virtual std::vector<Message> receive(NodeId recipient) = 0;
};

class InMemoryTransport final : public Transport {
public:
    explicit InMemoryTransport(std::vector<NodeId> nodes);

    // The message must be stamped with the current transport tick
    void send(const Message& message) override;
    // Takes all delivered messages and empties this recipient's inbox
    std::vector<Message> receive(NodeId recipient) override;

    // Called by the simulation, not exposed through the machine's interface
    void advanceTo(Tick tick);

private:
    struct PendingDelivery {
        Tick tick;
        NodeId recipient;
        Message message;
    };

    Tick current_tick_ = 0;
    std::vector<NodeId> nodes_;
    std::vector<PendingDelivery> pending_;
    std::map<NodeId, std::vector<Message>> inboxes_;
};

} // namespace fleetlink
