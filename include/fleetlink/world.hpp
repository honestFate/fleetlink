#pragma once

#include "fleetlink/logger.hpp"
#include "fleetlink/machine.hpp"
#include "fleetlink/transport.hpp"

#include <map>
#include <vector>

namespace fleetlink {

struct MachineSnapshot {
    MachineId id;
    MachineClass machine_class;
    MachineState state;
    std::int64_t position;
    int load;
    bool connected;
    bool on_route;
    std::optional<ResourceId> occupied_buffer;
    std::uint64_t completed_loads;
    std::uint64_t completed_unloads;
    bool halted;
};

struct ArmMachineSnapshot {
    MachineId id;
    std::optional<Telemetry> telemetry;
    std::optional<Tick> age;
};

enum class ZoneEventKind {
    REQUEST,
    ENTER,
    LEAVE,
    REMOVE,
};

struct ZoneEvent {
    Tick tick;
    MachineId machine;
    ZoneEventKind kind;
    ResourceId resource;
};

class World {
public:
    // An optional logger must outlive the world and its calls
    explicit World(std::vector<MachineId> members = {0, 1, 2, 3, 4, 5}, MachineConfig config = {},
                   Logger* logger = nullptr, ChannelConfig channel = {});

    // Send an addressed MISSION through the same transport as peer messages
    void dispatch(MachineId id, MissionTarget target);
    void step(Tick ticks = 1);
    // Inject a permanent physical failure at the current tick, without notifying peers
    void fail(MachineId id);
    // Apply a failure before message delivery and physical updates at the specified tick
    void failAt(MachineId id, Tick tick);
    void haltAll(NodeId initiator = ARM_NODE_ID);
    void resume(NodeId initiator = ARM_NODE_ID);
    void setArmOnline(bool online);
    bool armOnline() const noexcept { return arm_online_; }
    std::vector<ArmMachineSnapshot> armSnapshot() const;

    Tick currentTick() const noexcept { return tick_; }
    // Actual model state, independent of ARM telemetry
    std::vector<MachineSnapshot> snapshot() const;
    const std::vector<ZoneEvent>& events() const noexcept { return events_; }
    const ChannelCounters& channelCounters() const noexcept { return transport_.counters(); }

private:
    struct MachineRecord {
        Machine machine;
        std::uint64_t completed_loads = 0;
        std::uint64_t completed_unloads = 0;
    };

    void checkOccupancy() const;
    void recordZone(MachineId id, ZoneEventKind kind, ResourceId resource);
    void logMessage(const Message& message, const char* event, NodeId node,
                    std::optional<NodeId> recipient);
    void send(const Message& message);
    std::vector<Message> receive(NodeId recipient);
    void retryMissions();
    void sendControl(NodeId initiator, bool halted);
    void receiveArm();
    Message armMessage(MessagePayload payload, std::optional<NodeId> recipient = std::nullopt);
    struct TelemetryRecord {
        Telemetry data;
        Tick received_tick;
    };

    Logger* logger_;
    std::vector<MachineId> members_;
    InMemoryTransport transport_;
    std::map<MachineId, MachineRecord> machines_;
    Tick tick_ = 0;
    std::map<Tick, std::vector<MachineId>> scheduled_failures_;
    SequenceNumber arm_sequence_ = 0;
    LamportTime arm_clock_ = 0;
    Tick retry_interval_;
    std::map<MachineId, Message> pending_missions_;
    std::vector<ZoneEvent> events_;
    StopController arm_control_;
    bool arm_online_ = true;
    std::map<MachineId, TelemetryRecord> telemetry_;
};

} // namespace fleetlink
