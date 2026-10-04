#pragma once

#include <cstdint>
#include <limits>

namespace fleetlink {

using MachineId = std::uint32_t;
using NodeId = std::uint64_t;
using Tick = std::uint64_t;
using LamportTime = std::uint64_t;
using SequenceNumber = std::uint64_t;

// ARM has an address outside the MachineId range
constexpr NodeId ARM_NODE_ID = std::numeric_limits<NodeId>::max();

enum class ResourceId {
    LOADING_BUFFER,
    UNLOADING_BUFFER,
    LOADING_SERVICE,
    UNLOADING_SERVICE,
};

enum class MissionTarget {
    LOAD,
    DUMP,
};

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
