#pragma once

#include "fleetlink/logger.hpp"
#include "fleetlink/messages.hpp"

#include <map>
#include <vector>

namespace fleetlink {

struct ChannelConfig {
    std::uint64_t seed = 42;
    double loss = 0;
    Tick delay_min = 0;
    Tick delay_max = 0;
    double dup = 0;
};

class Transport {
public:
    virtual ~Transport() = default;
    virtual void send(const Message& message) = 0;
    virtual std::vector<Message> receive(NodeId recipient) = 0;
};

class InMemoryTransport final : public Transport {
public:
    explicit InMemoryTransport(std::vector<NodeId> nodes, ChannelConfig config = {},
                               Logger* logger = nullptr);

    // The message must be stamped with the current transport tick
    void send(const Message& message) override;
    // Takes all delivered messages and empties this recipient's inbox
    std::vector<Message> receive(NodeId recipient) override;

    // Called by the simulation, not exposed through the machine's interface
    void advanceTo(Tick tick);
    const ChannelCounters& counters() const noexcept { return counters_; }

private:
    struct PendingDelivery {
        Tick tick;
        NodeId recipient;
        Message message;
        unsigned copy = 0;
    };

    void logDelivery(const PendingDelivery& delivery, const char* event, LogLevel level);

    Tick current_tick_ = 0;
    ChannelConfig config_;
    Logger* logger_;
    ChannelCounters counters_{};
    std::vector<NodeId> nodes_;
    std::vector<PendingDelivery> pending_;
    std::map<NodeId, std::vector<Message>> inboxes_;
};

} // namespace fleetlink
