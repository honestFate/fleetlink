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
    std::optional<ResourceId> occupied_buffer;
    std::uint64_t completed_loads;
    std::uint64_t completed_unloads;
};

enum class ZoneEventKind {
    REQUEST,
    ENTER,
    LEAVE,
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
                   Logger* logger = nullptr);

    // Send an addressed MISSION through the same transport as peer messages
    void dispatch(MachineId id, MissionTarget target);
    void step(Tick ticks = 1);

    Tick currentTick() const noexcept { return tick_; }
    // Actual model state, independent of ARM telemetry
    std::vector<MachineSnapshot> snapshot() const;
    const std::vector<ZoneEvent>& events() const noexcept { return events_; }

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

    Logger* logger_;
    std::vector<MachineId> members_;
    InMemoryTransport transport_;
    std::map<MachineId, MachineRecord> machines_;
    Tick tick_ = 0;
    SequenceNumber arm_sequence_ = 0;
    LamportTime arm_clock_ = 0;
    std::vector<ZoneEvent> events_;
};

} // namespace fleetlink
