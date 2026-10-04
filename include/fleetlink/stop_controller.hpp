#pragma once

#include "fleetlink/messages.hpp"

#include <map>
#include <optional>
#include <set>
#include <vector>

namespace fleetlink {

// Every origin owns one ordered stop flag, RESUME clears only that origin's HALT
class StopController {
public:
    StopController(NodeId id, std::vector<NodeId> nodes, Tick retry_interval);
    SequenceNumber nextSequence() const;
    void issue(const Message& command);
    // True means a valid command needs an ACK, including a duplicate or stale command
    bool onMessage(const Message& message);
    std::optional<Message> retry(Tick now);
    bool halted() const noexcept { return !stops_.empty(); }

private:
    struct Pending {
        Message command;
        std::set<NodeId> awaiting;
        Tick last_sent;
    };
    void apply(const Message& command);

    NodeId id_;
    std::set<NodeId> nodes_;
    Tick retry_interval_;
    std::map<NodeId, SequenceNumber> versions_;
    std::set<NodeId> stops_;
    std::optional<Pending> pending_;
};

} // namespace fleetlink
