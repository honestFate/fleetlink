#include "fleetlink/machine.hpp"

#include <algorithm>
#include <stdexcept>
#include <utility>

namespace fleetlink {

namespace {
std::vector<NodeId> controlNodes(const std::vector<MachineId>& members)
{
    std::vector<NodeId> nodes(members.begin(), members.end());
    nodes.push_back(ARM_NODE_ID);
    return nodes;
}
} // namespace

Machine::Machine(MachineId id, MachineClass machine_class)
    : Machine(id, std::vector<MachineId>{id}, MachineConfig{}, machine_class)
{}

Machine::Machine(MachineId id, std::vector<MachineId> members, MachineConfig config,
                 MachineClass machine_class, Logger* logger)
    : id_(id), machine_class_(machine_class), position_(config.initial_position),
      load_(config.initial_load), config_(config), logger_(logger),
      control_(id, controlNodes(members), config.retry_interval),
      protocol_(id, std::move(members), config.retry_interval, logger, config.heartbeat,
                config.zone_capacity)
{
    if (config_.road_length <= 0 || config_.speed <= 0 || config_.loading_rate <= 0 ||
        config_.loading_rate > 100 || config_.unloading_rate <= 0 || config_.unloading_rate > 100 ||
        position_ < 0 || position_ > config_.road_length || load_ < 0 || load_ > 100 ||
        config_.telemetry_interval == 0) {
        throw std::invalid_argument("Invalid machine configuration");
    }
}

std::vector<Message> Machine::tick(Tick now, const std::vector<Message>& incoming,
                                   ChannelCounters channel)
{
    if (last_tick_ && (now <= *last_tick_ || now - *last_tick_ != 1)) {
        throw std::invalid_argument("Machine ticks must be consecutive");
    }
    last_tick_ = now;
    if (state_ == MachineState::FAULT) {
        return {};
    }
    if (state_ == MachineState::WAIT_PERMIT) {
        ++zone_wait_ticks_;
    }

    for (const auto& message : incoming) {
        protocol_.onMessage(message, now);
        onMission(message, now);
        const bool before = halted();
        if (control_.onMessage(message)) {
            protocol_.acknowledge(message.header.id, now);
            applyStop(now, before);
        }
    }
    protocol_.tick(now);
    if (const auto repeat = control_.retry(now)) {
        protocol_.resend(*repeat, now);
    }
    if (!halted()) {
        execute(now);
    }
    if (now >= last_telemetry_tick_ && now - last_telemetry_tick_ >= config_.telemetry_interval) {
        last_telemetry_tick_ = now;
        protocol_.send(Telemetry{now, position_, state_, load_, connected_, channel,
                                 zone_wait_count_, zone_wait_ticks_, halted()},
                       now, ARM_NODE_ID);
    }
    return protocol_.takeOutgoing();
}

void Machine::execute(Tick now)
{
    if (state_ == MachineState::IDLE && pending_target_) {
        target_ = *pending_target_;
        pending_target_.reset();
        state_ = MachineState::EN_ROUTE;
    }

    if (state_ == MachineState::EN_ROUTE) {
        move(now);
        return;
    }
    if (state_ == MachineState::WAIT_PERMIT) {
        if (!protocol_.isWaiting(targetBuffer()) && !protocol_.hasPermission(targetBuffer())) {
            protocol_.request(targetBuffer(), now);
            return;
        }
        if (protocol_.hasPermission(targetBuffer())) {
            occupied_buffer_ = targetBuffer();
            state_ = MachineState::IN_ZONE;
        }
        return;
    }
    if (state_ == MachineState::IN_ZONE) {
        // With k = 1, exclusive buffer ownership already guarantees exclusive service
        if (config_.zone_capacity > 1) {
            if (!protocol_.isWaiting(targetService()) &&
                !protocol_.hasPermission(targetService())) {
                protocol_.request(targetService(), now);
                return;
            }
            if (!protocol_.hasPermission(targetService())) {
                return;
            }
        }
        state_ = target_ == MissionTarget::LOAD ? MachineState::LOADING : MachineState::UNLOADING;
        logService("service_start", now);
    }
    if (state_ == MachineState::LOADING || state_ == MachineState::UNLOADING) {
        service(now);
    }
}

std::vector<Message> Machine::setHalted(bool stopped, Tick now)
{
    if (!onRoute()) {
        throw std::logic_error("Failed machine cannot initiate control commands");
    }
    const bool before = halted();
    const auto version = control_.nextSequence();
    const MessagePayload payload =
        stopped ? MessagePayload{Halt{version}} : MessagePayload{Resume{version}};
    control_.issue(protocol_.send(payload, now));
    applyStop(now, before);
    return protocol_.takeOutgoing();
}

void Machine::applyStop(Tick now, bool previously_halted)
{
    if (halted() && !occupied_buffer_) {
        protocol_.cancel(targetBuffer(), now);
    }
    if (halted() && state_ == MachineState::IN_ZONE) {
        protocol_.cancel(targetService(), now);
    }
    if (logger_ && previously_halted != halted()) {
        logger_->log({now,
                      nodeName(id_),
                      LogLevel::INFO,
                      halted() ? "halt_applied" : "resume_applied",
                      {{"pos", position_}, {"load", std::int64_t{load_}}}});
    }
}

void Machine::fail() noexcept
{
    state_ = MachineState::FAULT;
    connected_ = false;
    occupied_buffer_.reset();
    pending_target_.reset();
}

ResourceId Machine::targetBuffer() const noexcept
{
    return target_ == MissionTarget::LOAD ? ResourceId::LOADING_BUFFER
                                          : ResourceId::UNLOADING_BUFFER;
}

ResourceId Machine::targetService() const noexcept
{
    return target_ == MissionTarget::LOAD ? ResourceId::LOADING_SERVICE
                                          : ResourceId::UNLOADING_SERVICE;
}

void Machine::onMission(const Message& message, Tick now)
{
    const auto* mission = std::get_if<Mission>(&message.payload);
    if (!mission || message.header.id.sender != ARM_NODE_ID || message.header.recipient != id_) {
        return;
    }
    if (mission->first_target != MissionTarget::LOAD &&
        mission->first_target != MissionTarget::DUMP) {
        return;
    }
    protocol_.acknowledge(message.header.id, now);
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
        ++zone_wait_count_;
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
    logService("service_finish", now);
    if (config_.zone_capacity > 1) {
        protocol_.release(targetService(), now);
    }
    occupied_buffer_.reset();
    state_ = MachineState::EN_ROUTE;
    protocol_.release(released, now);
    const auto next = target_ == MissionTarget::LOAD ? MissionTarget::DUMP : MissionTarget::LOAD;
    target_ = pending_target_.value_or(next);
    pending_target_.reset();
}

void Machine::logService(const char* event, Tick now)
{
    if (logger_) {
        logger_->log(
            {now,
             nodeName(id_),
             LogLevel::INFO,
             event,
             {{"operation", std::string(target_ == MissionTarget::LOAD ? "load" : "dump")}}});
    }
}

} // namespace fleetlink
