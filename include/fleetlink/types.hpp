#pragma once

#include <cstdint>

namespace fleetlink {

using MachineId = std::uint32_t;

enum class MachineClass {
    HAULER,
    LOADER,
};

enum class MachineState {
    IDLE,
    EN_ROUTE,
    WAIT_PERMIT,
    IN_ZONE,
    LOADING,
    UNLOADING,
    FAULT,
    OFFLINE,
};

} // namespace fleetlink
