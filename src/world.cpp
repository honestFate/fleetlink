#include "fleetlink/world.hpp"

#include <algorithm>
#include <array>
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

std::string resourceName(ResourceId resource)
{
    constexpr std::array<const char*, 4> names{"LOADING_BUFFER", "UNLOADING_BUFFER",
                                               "LOADING_SERVICE", "UNLOADING_SERVICE"};
    return names.at(static_cast<std::size_t>(resource));
}

std::string stateName(MachineState state)
{
    constexpr std::array<const char*, 8> names{"IDLE",    "EN_ROUTE",  "WAIT_PERMIT", "IN_ZONE",
                                               "LOADING", "UNLOADING", "FAULT",       "OFFLINE"};
    return names.at(static_cast<std::size_t>(state));
}

} // namespace

World::World(std::vector<MachineId> members, MachineConfig config, Logger* logger,
             ChannelConfig channel)
    : logger_(logger), members_(validatedMembers(std::move(members))),
      transport_(transportNodes(members_), channel, logger), retry_interval_(config.retry_interval),
      arm_control_(ARM_NODE_ID, transportNodes(members_), config.retry_interval)
{
    for (const auto id : members_) {
        machines_.emplace(
            id, MachineRecord{Machine(id, members_, config, MachineClass::HAULER, logger)});
    }
}

void World::dispatch(MachineId id, MissionTarget target)
{
    if (!arm_online_) {
        throw std::logic_error("ARM is offline");
    }
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
    const Message mission{{{ARM_NODE_ID, sequence}, id, tick_, clock}, Mission{sequence, target}};
    send(mission);
    pending_missions_.insert_or_assign(id, mission);
    arm_sequence_ = sequence;
    arm_clock_ = clock;
}

void World::step(Tick ticks)
{
    if (ticks > std::numeric_limits<Tick>::max() - tick_) {
        throw std::overflow_error("World tick overflow");
    }
    for (Tick step = 0; step < ticks; ++step) {
        ++tick_;
        const auto failures = scheduled_failures_.find(tick_);
        if (failures != scheduled_failures_.end()) {
            for (const auto id : failures->second) {
                fail(id);
            }
            scheduled_failures_.erase(failures);
        }
        transport_.advanceTo(tick_);
        if (arm_online_) {
            receiveArm();
            retryMissions();
            if (auto repeat = arm_control_.retry(tick_)) {
                arm_clock_ = nextLamportTime(arm_clock_);
                repeat->header.clock = arm_clock_;
                repeat->header.sent_tick = tick_;
                send(*repeat);
            }
        }

        std::vector<Message> outgoing;
        for (auto& [id, record] : machines_) {
            if (!record.machine.onRoute()) {
                continue;
            }
            const auto before = record.machine.occupiedBuffer();
            const auto state_before = record.machine.state();
            const auto messages = record.machine.tick(tick_, receive(id), transport_.counters(id));
            // Observe physical occupancy after every update, never grant permissions here
            checkOccupancy();
            const auto after = record.machine.occupiedBuffer();
            if (before && !after) {
                recordZone(id, ZoneEventKind::LEAVE, *before);
                if (*before == ResourceId::LOADING_BUFFER) {
                    ++record.completed_loads;
                } else {
                    ++record.completed_unloads;
                }
            }
            if (!before && after) {
                recordZone(id, ZoneEventKind::ENTER, *after);
            }
            for (const auto& message : messages) {
                if (const auto* request = std::get_if<Request>(&message.payload)) {
                    if (request->id.sequence == message.header.id.sequence &&
                        state_before != MachineState::WAIT_PERMIT) {
                        recordZone(id, ZoneEventKind::REQUEST, request->id.resource);
                    }
                }
            }
            if (logger_ && logger_->enabled(LogLevel::DEBUG) &&
                state_before != record.machine.state()) {
                logger_->log({tick_,
                              nodeName(id),
                              LogLevel::DEBUG,
                              "state_change",
                              {{"from", stateName(state_before)},
                               {"to", stateName(record.machine.state())},
                               {"pos", record.machine.position()},
                               {"load", std::int64_t{record.machine.load()}}}});
            }
            outgoing.insert(outgoing.end(), messages.begin(), messages.end());
        }
        std::sort(outgoing.begin(), outgoing.end(), messageBefore);
        for (const auto& message : outgoing) {
            send(message);
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
                          machine.load(), machine.connected(), machine.onRoute(),
                          machine.occupiedBuffer(), record.completed_loads,
                          record.completed_unloads, machine.halted()});
    }
    return result;
}

void World::failAt(MachineId id, Tick tick)
{
    if (tick <= tick_ || !machines_.count(id)) {
        throw std::invalid_argument("Scheduled failure requires a known machine and future tick");
    }
    scheduled_failures_[tick].push_back(id);
}

void World::fail(MachineId id)
{
    const auto found = machines_.find(id);
    if (found == machines_.end()) {
        throw std::invalid_argument("Unknown machine id");
    }
    auto& machine = found->second.machine;
    if (!machine.onRoute()) {
        return;
    }
    const auto occupied = machine.occupiedBuffer();
    const auto previous = machine.state();
    transport_.disconnect(id);
    machine.fail();
    if (logger_) {
        logger_->log({tick_,
                      nodeName(id),
                      LogLevel::WARN,
                      "machine_failed",
                      {{"from", stateName(previous)},
                       {"pos", machine.position()},
                       {"load", std::int64_t{machine.load()}},
                       {"on_route", false}}});
    }
    if (occupied) {
        recordZone(id, ZoneEventKind::REMOVE, *occupied);
    }
    checkOccupancy();
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
        if (logger_) {
            logger_->log({tick_,
                          "WORLD",
                          LogLevel::ERROR,
                          "capacity_violation",
                          {{"loading", std::uint64_t{loading_count}},
                           {"unloading", std::uint64_t{unloading_count}}}});
        }
        throw std::logic_error("Buffer capacity exceeded at tick " + std::to_string(tick_));
    }
}

void World::recordZone(MachineId id, ZoneEventKind kind, ResourceId resource)
{
    events_.push_back({tick_, id, kind, resource});
    if (logger_) {
        constexpr std::array<const char*, 4> names{"zone_request", "zone_enter", "zone_leave",
                                                   "zone_removed"};
        logger_->log({tick_,
                      nodeName(id),
                      LogLevel::INFO,
                      names.at(static_cast<std::size_t>(kind)),
                      {{"zone", resourceName(resource)}}});
    }
}

void World::logMessage(const Message& message, const char* event, NodeId node,
                       std::optional<NodeId> recipient)
{
    if (!logger_ || !logger_->enabled(LogLevel::TRACE)) {
        return;
    }
    constexpr std::array<const char*, 10> kinds{"HEARTBEAT", "MISSION",   "REQUEST", "REPLY",
                                                "RELEASE",   "TELEMETRY", "HALT",    "RESUME",
                                                "ACK",       "CANCEL"};
    LogRecord record{
        tick_,
        nodeName(node),
        LogLevel::TRACE,
        event,
        {{"from", nodeName(message.header.id.sender)},
         {"to", recipient ? nodeName(*recipient) : "broadcast"},
         {"kind", std::string(kinds.at(static_cast<std::size_t>(messageKind(message))))},
         {"seq", message.header.id.sequence},
         {"sent_tick", message.header.sent_tick},
         {"clock", message.header.clock}}};
    const auto* request = std::get_if<Request>(&message.payload);
    const auto* reply = std::get_if<Reply>(&message.payload);
    const auto* release = std::get_if<Release>(&message.payload);
    const auto* cancel = std::get_if<Cancel>(&message.payload);
    std::optional<RequestId> attempt;
    if (request) {
        attempt = request->id;
        record.fields.emplace("request_clock", request->timestamp);
    } else if (reply) {
        attempt = reply->request;
    } else if (release) {
        attempt = release->request;
    } else if (cancel) {
        attempt = cancel->request;
    }
    if (attempt) {
        record.fields.emplace("zone", resourceName(attempt->resource));
        record.fields.emplace("requester", nodeName(attempt->requester));
        record.fields.emplace("request_seq", attempt->sequence);
    }
    if (const auto* mission = std::get_if<Mission>(&message.payload)) {
        record.fields.emplace("mission_seq", mission->sequence);
        record.fields.emplace(
            "target", std::string(mission->first_target == MissionTarget::LOAD ? "load" : "dump"));
    }
    if (const auto* ack = std::get_if<Ack>(&message.payload)) {
        record.fields.emplace("ack_from", nodeName(ack->message.sender));
        record.fields.emplace("ack_seq", ack->message.sequence);
    }
    if (const auto* heartbeat = std::get_if<Heartbeat>(&message.payload)) {
        record.fields.emplace("heartbeat_seq", heartbeat->sequence);
    }
    logger_->log(std::move(record));
}

void World::send(const Message& message)
{
    transport_.send(message);
    logMessage(message, "msg_send", message.header.id.sender, message.header.recipient);
}

std::vector<Message> World::receive(NodeId recipient)
{
    auto incoming = transport_.receive(recipient);
    for (const auto& message : incoming) {
        logMessage(message, "msg_receive", recipient, recipient);
    }
    return incoming;
}

void World::retryMissions()
{
    for (auto& [id, message] : pending_missions_) {
        if (tick_ - message.header.sent_tick < retry_interval_) {
            continue;
        }
        // A MISSION keeps its identity so a delayed ACK can finish any retransmission
        message.header.sent_tick = tick_;
        arm_clock_ = nextLamportTime(arm_clock_);
        message.header.clock = arm_clock_;
        logMessage(message, "mission_retry", ARM_NODE_ID, id);
        send(message);
    }
}

Message World::armMessage(MessagePayload payload, std::optional<NodeId> recipient)
{
    if (arm_sequence_ == std::numeric_limits<SequenceNumber>::max()) {
        throw std::overflow_error("ARM message sequence overflow");
    }
    arm_clock_ = nextLamportTime(arm_clock_);
    return {{{ARM_NODE_ID, ++arm_sequence_}, recipient, tick_, arm_clock_}, payload};
}

void World::haltAll(NodeId initiator)
{
    sendControl(initiator, true);
}
void World::resume(NodeId initiator)
{
    sendControl(initiator, false);
}

void World::sendControl(NodeId initiator, bool halted)
{
    if (initiator == ARM_NODE_ID) {
        if (!arm_online_) {
            throw std::logic_error("ARM is offline");
        }
        const auto version = arm_control_.nextSequence();
        const MessagePayload payload =
            halted ? MessagePayload{Halt{version}} : MessagePayload{Resume{version}};
        const auto message = armMessage(payload);
        arm_control_.issue(message);
        send(message);
        return;
    }
    if (initiator > std::numeric_limits<MachineId>::max() ||
        !machines_.count(static_cast<MachineId>(initiator))) {
        throw std::invalid_argument("Unknown control initiator");
    }
    for (const auto& message :
         machines_.at(static_cast<MachineId>(initiator)).machine.setHalted(halted, tick_)) {
        send(message);
    }
}

void World::setArmOnline(bool online)
{
    if (arm_online_ == online) {
        return;
    }
    transport_.setOnline(ARM_NODE_ID, online);
    arm_online_ = online;
    if (logger_) {
        logger_->log({tick_, "ARM", LogLevel::INFO, online ? "arm_online" : "arm_offline", {}});
    }
}

std::vector<ArmMachineSnapshot> World::armSnapshot() const
{
    std::vector<ArmMachineSnapshot> result;
    for (const auto id : members_) {
        const auto found = telemetry_.find(id);
        if (found == telemetry_.end()) {
            result.push_back({id, std::nullopt, std::nullopt});
        } else {
            result.push_back({id, found->second.data, tick_ - found->second.received_tick});
        }
    }
    return result;
}

void World::receiveArm()
{
    for (const auto& message : receive(ARM_NODE_ID)) {
        arm_clock_ = nextLamportTime(arm_clock_, message.header.clock);
        if (arm_control_.onMessage(message)) {
            send(armMessage(Ack{message.header.id}, message.header.id.sender));
        }
        if (message.header.id.sender > std::numeric_limits<MachineId>::max()) {
            continue;
        }
        const auto id = static_cast<MachineId>(message.header.id.sender);
        if (const auto* ack = std::get_if<Ack>(&message.payload)) {
            const auto pending = pending_missions_.find(id);
            if (pending != pending_missions_.end() && ack->message == pending->second.header.id) {
                pending_missions_.erase(pending);
            }
        }
        const auto* sample = std::get_if<Telemetry>(&message.payload);
        if (!sample || message.header.recipient != ARM_NODE_ID || !machines_.count(id)) {
            continue;
        }
        const auto previous = telemetry_.find(id);
        if (previous != telemetry_.end() && sample->sequence <= previous->second.data.sequence) {
            logMessage(message, "telemetry_ignored", ARM_NODE_ID, ARM_NODE_ID);
            continue;
        }
        telemetry_.insert_or_assign(id, TelemetryRecord{*sample, tick_});
        if (logger_) {
            logger_->log({tick_,
                          "ARM",
                          LogLevel::INFO,
                          "telemetry",
                          {{"from", nodeName(id)},
                           {"sample_tick", message.header.sent_tick},
                           {"seq", sample->sequence},
                           {"pos", sample->position},
                           {"state", stateName(sample->state)},
                           {"load", std::int64_t{sample->load}},
                           {"connected", sample->connected},
                           {"halted", sample->halted},
                           {"sent", sample->channel.sent},
                           {"delivered", sample->channel.delivered},
                           {"lost", sample->channel.lost},
                           {"duplicates", sample->channel.duplicates},
                           {"zone_wait_count", sample->zone_wait_count},
                           {"zone_wait_ticks", sample->zone_wait_ticks}}});
        }
    }
}

} // namespace fleetlink
