#include "fleetlink/machine.hpp"

#include <algorithm>
#include <stdexcept>
#include <utility>

namespace fleetlink {

Machine::Machine(MachineId id, MachineClass machine_class)
    : Machine(id, std::vector<MachineId>{id}, MachineConfig{}, machine_class)
{}

Machine::Machine(MachineId id, std::vector<MachineId> members, MachineConfig config,
                 MachineClass machine_class)
    : id_(id), machine_class_(machine_class), position_(config.initial_position),
      load_(config.initial_load), config_(config), protocol_(id, std::move(members))
{
    if (config_.road_length <= 0 || config_.speed <= 0 || config_.loading_rate <= 0 ||
        config_.loading_rate > 100 || config_.unloading_rate <= 0 || config_.unloading_rate > 100 ||
        position_ < 0 || position_ > config_.road_length || load_ < 0 || load_ > 100) {
        throw std::invalid_argument("Invalid machine configuration");
    }
}

std::vector<Message> Machine::tick(Tick now, const std::vector<Message>& incoming)
{
    if (last_tick_ && (now <= *last_tick_ || now - *last_tick_ != 1)) {
        throw std::invalid_argument("Machine ticks must be consecutive");
    }
    last_tick_ = now;

    for (const auto& message : incoming) {
        protocol_.onMessage(message, now);
        onMission(message);
    }
    if (state_ == MachineState::IDLE && pending_target_) {
        target_ = *pending_target_;
        pending_target_.reset();
        state_ = MachineState::EN_ROUTE;
    }

    if (state_ == MachineState::EN_ROUTE) {
        move(now);
        return protocol_.takeOutgoing();
    }
    if (state_ == MachineState::WAIT_PERMIT) {
        if (protocol_.hasPermission(targetBuffer())) {
            occupied_buffer_ = targetBuffer();
            state_ = MachineState::IN_ZONE;
        }
        return protocol_.takeOutgoing();
    }
    if (state_ == MachineState::IN_ZONE) {
        state_ = target_ == MissionTarget::LOAD ? MachineState::LOADING : MachineState::UNLOADING;
    }
    if (state_ == MachineState::LOADING || state_ == MachineState::UNLOADING) {
        service(now);
    }
    return protocol_.takeOutgoing();
}

ResourceId Machine::targetBuffer() const noexcept
{
    return target_ == MissionTarget::LOAD ? ResourceId::LOADING_BUFFER
                                          : ResourceId::UNLOADING_BUFFER;
}

void Machine::onMission(const Message& message)
{
    const auto* mission = std::get_if<Mission>(&message.payload);
    if (!mission || message.header.id.sender != ARM_NODE_ID || message.header.recipient != id_) {
        return;
    }
    if (mission->first_target != MissionTarget::LOAD &&
        mission->first_target != MissionTarget::DUMP) {
        return;
    }
    if (last_mission_sequence_ && mission->sequence <= *last_mission_sequence_) {
        return;
    }
    last_mission_sequence_ = mission->sequence;
    pending_target_ = mission->first_target;
}

void Machine::move(Tick now)
{
    const auto destination = target_ == MissionTarget::LOAD ? 0 : config_.road_length;
    if (position_ < destination) {
        position_ += std::min(config_.speed, destination - position_);
    } else if (position_ > destination) {
        position_ -= std::min(config_.speed, position_ - destination);
    }
    if (position_ == destination) {
        protocol_.request(targetBuffer(), now);
        state_ = MachineState::WAIT_PERMIT;
    }
}

void Machine::service(Tick now)
{
    if (target_ == MissionTarget::LOAD) {
        load_ = std::min(100, load_ + config_.loading_rate);
    } else {
        load_ = std::max(0, load_ - config_.unloading_rate);
    }
    const bool finished = target_ == MissionTarget::LOAD ? load_ == 100 : load_ == 0;
    if (!finished) {
        return;
    }

    const auto released = *occupied_buffer_;
    occupied_buffer_.reset();
    state_ = MachineState::EN_ROUTE;
    protocol_.release(released, now);
    const auto next = target_ == MissionTarget::LOAD ? MissionTarget::DUMP : MissionTarget::LOAD;
    target_ = pending_target_.value_or(next);
    pending_target_.reset();
}

} // namespace fleetlink
