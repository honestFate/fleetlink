#pragma once

#include "fleetlink/types.hpp"

namespace fleetlink {

class Machine {
public:
    explicit Machine(MachineId id, MachineClass machine_class = MachineClass::HAULER) noexcept;

    MachineId id() const noexcept { return id_; }
    MachineClass machineClass() const noexcept { return machine_class_; }
    MachineState state() const noexcept { return state_; }
    std::int64_t position() const noexcept { return position_; }
    int load() const noexcept { return load_; }
    bool connected() const noexcept { return connected_; }

private:
    MachineId id_;
    MachineClass machine_class_;
    MachineState state_ = MachineState::IDLE;
    std::int64_t position_ = 0; // Position on the road
    int load_ = 0;              // 0..100%
    bool connected_ = true;     // Connection accessibility sign
};

} // namespace fleetlink
