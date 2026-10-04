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
      transport_(transportNodes(members_), channel, logger), retry_interval_(config.retry_interval)
{
    for (const auto id : members_) {
        machines_.emplace(
            id, MachineRecord{Machine(id, members_, config, MachineClass::HAULER, logger)});
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
        transport_.advanceTo(++tick_);
        for (const auto& message : receive(ARM_NODE_ID)) {
            arm_clock_ = nextLamportTime(arm_clock_, message.header.clock);
            const auto* ack = std::get_if<Ack>(&message.payload);
            if (ack && message.header.id.sender <= std::numeric_limits<MachineId>::max()) {
                const auto pending =
                    pending_missions_.find(static_cast<MachineId>(message.header.id.sender));
                if (pending != pending_missions_.end() &&
                    ack->message == pending->second.header.id) {
                    pending_missions_.erase(pending);
                }
            }
        }
        retryMissions();

        std::vector<Message> outgoing;
        for (auto& [id, record] : machines_) {
            const auto before = record.machine.occupiedBuffer();
            const auto state_before = record.machine.state();
            const auto messages = record.machine.tick(tick_, receive(id));
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
                    if (request->id.sequence == message.header.id.sequence) {
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
        constexpr std::array<const char*, 3> names{"zone_request", "zone_enter", "zone_leave"};
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
    std::optional<RequestId> attempt;
    if (request) {
        attempt = request->id;
        record.fields.emplace("request_clock", request->timestamp);
    } else if (reply) {
        attempt = reply->request;
    } else if (release) {
        attempt = release->request;
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

} // namespace fleetlink
