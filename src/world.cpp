#include "fleetlink/world.hpp"

#include <algorithm>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>

namespace fleetlink {

namespace {

std::vector<MachineId> validatedMembers(std::vector<MachineId> members)
{
    if (members.size() < 4 || members.size() > 10) {
        throw std::invalid_argument("World requires 4 to 10 machines");
    }
    std::sort(members.begin(), members.end());
    if (std::adjacent_find(members.begin(), members.end()) != members.end()) {
        throw std::invalid_argument("Duplicate machine id in world");
    }
    return members;
}

std::vector<NodeId> transportNodes(const std::vector<MachineId>& members)
{
    std::vector<NodeId> nodes(members.begin(), members.end());
    nodes.push_back(ARM_NODE_ID);
    return nodes;
}

} // namespace

World::World(std::vector<MachineId> members, MachineConfig config)
    : members_(validatedMembers(std::move(members))), transport_(transportNodes(members_))
{
    for (const auto id : members_) {
        machines_.emplace(id, MachineRecord{Machine(id, members_, config)});
    }
}

void World::dispatch(MachineId id, MissionTarget target)
{
    if (machines_.find(id) == machines_.end()) {
        throw std::invalid_argument("Unknown machine id");
    }
    if (target != MissionTarget::LOAD && target != MissionTarget::DUMP) {
        throw std::invalid_argument("Unknown mission target");
    }
    if (arm_sequence_ == std::numeric_limits<SequenceNumber>::max()) {
        throw std::overflow_error("ARM message sequence overflow");
    }
    const auto sequence = arm_sequence_ + 1;
    const auto clock = nextLamportTime(arm_clock_);
    transport_.send({{{ARM_NODE_ID, sequence}, id, tick_, clock}, Mission{sequence, target}});
    arm_sequence_ = sequence;
    arm_clock_ = clock;
}

void World::step(Tick ticks)
{
    if (ticks > std::numeric_limits<Tick>::max() - tick_) {
        throw std::overflow_error("World tick overflow");
    }
    for (Tick step = 0; step < ticks; ++step) {
        transport_.advanceTo(++tick_);
        for (const auto& message : transport_.receive(ARM_NODE_ID)) {
            arm_clock_ = nextLamportTime(arm_clock_, message.header.clock);
        }

        std::vector<Message> outgoing;
        for (auto& [id, record] : machines_) {
            const auto before = record.machine.occupiedBuffer();
            const auto messages = record.machine.tick(tick_, transport_.receive(id));
            // Observe physical occupancy after every update, never grant permissions here
            checkOccupancy();
            const auto after = record.machine.occupiedBuffer();
            if (before && !after) {
                events_.push_back({tick_, id, ZoneEventKind::LEAVE, *before});
                if (*before == ResourceId::LOADING_BUFFER) {
                    ++record.completed_loads;
                } else {
                    ++record.completed_unloads;
                }
            }
            if (!before && after) {
                events_.push_back({tick_, id, ZoneEventKind::ENTER, *after});
            }
            for (const auto& message : messages) {
                if (const auto* request = std::get_if<Request>(&message.payload)) {
                    events_.push_back({tick_, id, ZoneEventKind::REQUEST, request->id.resource});
                }
            }
            outgoing.insert(outgoing.end(), messages.begin(), messages.end());
        }
        std::sort(outgoing.begin(), outgoing.end(), messageBefore);
        for (const auto& message : outgoing) {
            transport_.send(message);
        }
    }
}

std::vector<MachineSnapshot> World::snapshot() const
{
    std::vector<MachineSnapshot> result;
    result.reserve(machines_.size());
    for (const auto& [id, record] : machines_) {
        const auto& machine = record.machine;
        result.push_back({id, machine.machineClass(), machine.state(), machine.position(),
                          machine.load(), machine.connected(), machine.occupiedBuffer(),
                          record.completed_loads, record.completed_unloads});
    }
    return result;
}

void World::checkOccupancy() const
{
    unsigned loading_count = 0;
    unsigned unloading_count = 0;
    for (const auto& [id, record] : machines_) {
        const auto buffer = record.machine.occupiedBuffer();
        loading_count += buffer == ResourceId::LOADING_BUFFER;
        unloading_count += buffer == ResourceId::UNLOADING_BUFFER;
    }
    if (loading_count > 1 || unloading_count > 1) {
        throw std::logic_error("Buffer capacity exceeded at tick " + std::to_string(tick_));
    }
}

} // namespace fleetlink
