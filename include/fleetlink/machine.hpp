#pragma once

#include "fleetlink/access_protocol.hpp"

#include <optional>
#include <vector>

namespace fleetlink {

struct MachineConfig {
    std::int64_t road_length = 100; // Loading at 0, unloading at road_length
    std::int64_t speed = 10;        // Position units per tick
    int loading_rate = 20;          // Percentage points per tick
    int unloading_rate = 20;        // Percentage points per tick
    std::int64_t initial_position = 0;
    int initial_load = 0;
};

class Machine {
public:
    // A standalone machine with a one-member protocol
    explicit Machine(MachineId id, MachineClass machine_class = MachineClass::HAULER);
    Machine(MachineId id, std::vector<MachineId> members, MachineConfig config = {},
            MachineClass machine_class = MachineClass::HAULER);

    // Process one tick and return messages for the simulation's send phase
    // Tick values must be consecutive after the first call
    std::vector<Message> tick(Tick now, const std::vector<Message>& incoming);

    MachineId id() const noexcept { return id_; }
    MachineClass machineClass() const noexcept { return machine_class_; }
    MachineState state() const noexcept { return state_; }
    std::int64_t position() const noexcept { return position_; }
    int load() const noexcept { return load_; }
    bool connected() const noexcept { return connected_; }
    // Physical occupancy, separate from the protocol's permission to enter
    std::optional<ResourceId> occupiedBuffer() const noexcept { return occupied_buffer_; }

private:
    ResourceId targetBuffer() const noexcept;
    void onMission(const Message& message);
    void move(Tick now);
    void service(Tick now);

    MachineId id_;
    MachineClass machine_class_;
    MachineState state_ = MachineState::IDLE;
    std::int64_t position_ = 0; // Position on the road
    int load_ = 0;              // 0..100%
    bool connected_ = true;     // Connection accessibility sign
    MachineConfig config_;
    AccessProtocol protocol_;
    MissionTarget target_ = MissionTarget::LOAD;
    std::optional<MissionTarget> pending_target_;
    std::optional<SequenceNumber> last_mission_sequence_;
    std::optional<ResourceId> occupied_buffer_;
    std::optional<Tick> last_tick_;
};

} // namespace fleetlink
