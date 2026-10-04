#pragma once

#include "fleetlink/logger.hpp"
#include "fleetlink/messages.hpp"

#include <map>
#include <optional>
#include <set>
#include <vector>

namespace fleetlink {

struct HeartbeatConfig {
    Tick interval = 5;
    Tick timeout = 200;
};

// Local permission state for bounded buffers and exclusive service equipment
class AccessProtocol {
public:
    // The configured roster includes this machine and excludes ARM
    AccessProtocol(MachineId id, std::vector<MachineId> members, Tick retry_interval = 50,
                   Logger* logger = nullptr, HeartbeatConfig heartbeat = {},
                   unsigned zone_capacity = 1);

    // Acquire one buffer, then its service resource
    RequestId request(ResourceId resource, Tick tick);
    // Call after physically releasing equipment or leaving a buffer, in that order
    void release(ResourceId resource, Tick tick);
    // Cancel only before physical entry, including a permission obtained on this tick
    void cancel(ResourceId resource, Tick tick);
    void onMessage(const Message& message, Tick tick);
    // Run after incoming messages so an arriving reply can cancel a retry
    void retry(Tick tick);
    // Process failure detection, heartbeat and retries after incoming messages
    void tick(Tick now);
    void acknowledge(MessageId message, Tick tick);
    bool isActive(MachineId peer) const noexcept;
    Message send(MessagePayload payload, Tick tick, std::optional<NodeId> recipient = std::nullopt);
    void resend(Message message, Tick tick);

    bool isWaiting(ResourceId resource) const noexcept;
    // A logical reservation, independent of the machine's physical position
    bool hasPermission(ResourceId resource) const noexcept;

    // Drain and send at the current tick after processing the machines
    std::vector<Message> takeOutgoing();

private:
    MessageHeader makeHeader(Tick tick, std::optional<NodeId> recipient = std::nullopt);
    struct PeerRequest {
        RequestId id;
        LamportTime timestamp = 0;
        bool replied = false;
        bool released = false;
    };
    struct PeerHealth {
        Tick last_seen = 0;
        std::optional<SequenceNumber> sequence;
        bool active = true;
    };
    struct ResourceState {
        unsigned capacity = 1;
        Tick last_request_tick = 0;
        std::optional<Request> request;
        bool granted = false;
        std::set<MachineId> replies;
        std::map<MachineId, PeerRequest> peer_requests;
    };

    void onRequest(const Message& message, Tick tick);
    bool onReply(MachineId sender, const Reply& reply);
    void closePeerRequest(const RequestId& request);
    void onHeartbeat(MachineId peer, const Heartbeat& heartbeat, Tick tick);
    void updatePermission(ResourceState& state);
    void replyDeferred(ResourceState& state, Tick tick);
    void logPeer(const char* event, MachineId peer, Tick tick, LogLevel level);
    void logMessage(const char* event, const Message& message, Tick tick);

    MachineId id_;
    std::vector<MachineId> members_;
    Tick retry_interval_;
    Logger* logger_;
    HeartbeatConfig heartbeat_;
    Tick last_heartbeat_tick_ = 0;
    std::map<MachineId, PeerHealth> peers_;
    LamportTime clock_ = 0;
    SequenceNumber sequence_ = 0;
    std::map<ResourceId, ResourceState> resources_;
    std::vector<Message> outgoing_;
};

} // namespace fleetlink
