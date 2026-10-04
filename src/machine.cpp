#include "fleetlink/machine.hpp"

namespace fleetlink {

Machine::Machine(MachineId id, MachineClass machine_class) noexcept
    : id_(id), machine_class_(machine_class) {}

} // namespace fleetlink
