#pragma once

#include "fleetlink/types.hpp"

#include <optional>
#include <variant>
#include <vector>

namespace fleetlink {

enum class MessageKind {
    HEARTBEAT,
    MISSION,
    REQUEST,
    REPLY,
    RELEASE,
    TELEMETRY,
    HALT,
    RESUME,
    ACK,
    CANCEL,
};

struct MessageId {
    NodeId sender;
    SequenceNumber sequence;
};

constexpr bool operator==(const MessageId& lhs, const MessageId& rhs) noexcept
{
    return lhs.sender == rhs.sender && lhs.sequence == rhs.sequence;
}

struct RequestId {
    ResourceId resource;
    MachineId requester;
    SequenceNumber sequence;
};

constexpr bool operator==(const RequestId& lhs, const RequestId& rhs) noexcept
{
    return lhs.resource == rhs.resource && lhs.requester == rhs.requester &&
           lhs.sequence == rhs.sequence;
}

struct RequestKey {
    LamportTime timestamp;
    MachineId requester;
};

constexpr bool operator==(const RequestKey& lhs, const RequestKey& rhs) noexcept
{
    return lhs.timestamp == rhs.timestamp && lhs.requester == rhs.requester;
}

constexpr bool operator<(const RequestKey& lhs, const RequestKey& rhs) noexcept
{
    return lhs.timestamp < rhs.timestamp ||
           (lhs.timestamp == rhs.timestamp && lhs.requester < rhs.requester);
}

struct Heartbeat {
    static constexpr MessageKind kind = MessageKind::HEARTBEAT;
    SequenceNumber sequence;
};

struct Mission {
    static constexpr MessageKind kind = MessageKind::MISSION;
    SequenceNumber sequence;
    MissionTarget first_target;
};

struct Request {
    static constexpr MessageKind kind = MessageKind::REQUEST;
    RequestId id;
    LamportTime timestamp;
};

constexpr RequestKey requestKey(const Request& request) noexcept
{
    return {request.timestamp, request.id.requester};
}

struct Reply {
    static constexpr MessageKind kind = MessageKind::REPLY;
    RequestId request;
};

struct Release {
    static constexpr MessageKind kind = MessageKind::RELEASE;
    RequestId request;
};

struct ChannelCounters {
    std::uint64_t sent;
    std::uint64_t delivered;
    std::uint64_t lost;
    std::uint64_t duplicates;
};

struct Telemetry {
    static constexpr MessageKind kind = MessageKind::TELEMETRY;
    SequenceNumber sequence;
    std::int64_t position;
    MachineState state;
    int load;
    bool connected;
    ChannelCounters channel;
    std::uint64_t zone_wait_count;
    Tick zone_wait_ticks;
};

struct Halt {
    static constexpr MessageKind kind = MessageKind::HALT;
    SequenceNumber sequence;
};

struct Resume {
    static constexpr MessageKind kind = MessageKind::RESUME;
    SequenceNumber sequence;
};

struct Ack {
    static constexpr MessageKind kind = MessageKind::ACK;
    MessageId message;
};

struct Cancel {
    static constexpr MessageKind kind = MessageKind::CANCEL;
    RequestId request;
};

using MessagePayload =
    std::variant<Heartbeat, Mission, Request, Reply, Release, Telemetry, Halt, Resume, Ack, Cancel>;

struct MessageHeader {
    MessageId id;
    std::optional<NodeId> recipient; // nullopt means broadcast
    Tick sent_tick;
    LamportTime clock;
};

struct Message {
    MessageHeader header;
    MessagePayload payload;
};

MessageKind messageKind(const Message& message);

std::vector<NodeId> recipientsFor(const Message& message, const std::vector<NodeId>& nodes);
bool messageBefore(const Message& lhs, const Message& rhs) noexcept;
Tick deliveryTick(Tick sent_tick, Tick delay);
LamportTime nextLamportTime(LamportTime local, LamportTime received = 0);

} // namespace fleetlink
