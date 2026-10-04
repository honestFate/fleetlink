#include "fleetlink/access_protocol.hpp"

#include <algorithm>
#include <limits>
#include <stdexcept>
#include <utility>

namespace fleetlink {

namespace {

bool isBuffer(ResourceId resource)
{
    return resource == ResourceId::LOADING_BUFFER || resource == ResourceId::UNLOADING_BUFFER;
}

} // namespace

AccessProtocol::AccessProtocol(MachineId id, std::vector<MachineId> members)
    : id_(id), members_(std::move(members))
{
    std::sort(members_.begin(), members_.end());
    if (std::adjacent_find(members_.begin(), members_.end()) != members_.end()) {
        throw std::invalid_argument("Duplicate machine id in protocol membership");
    }
    if (!std::binary_search(members_.begin(), members_.end(), id_)) {
        throw std::invalid_argument("Protocol membership must include this machine");
    }
}

RequestId AccessProtocol::request(ResourceId resource, Tick tick)
{
    if (!isBuffer(resource)) {
        throw std::invalid_argument("Access protocol supports buffers only");
    }
    if (request_) {
        throw std::logic_error("Release the current buffer before requesting another");
    }

    const auto header = makeHeader(tick);
    const Request attempt{{resource, id_, header.id.sequence}, header.clock};
    outgoing_.push_back({header, attempt});
    request_ = attempt;
    replies_.clear();
    granted_ = members_.size() == 1;
    return attempt.id;
}

void AccessProtocol::release(ResourceId resource, Tick tick)
{
    if (!hasPermission(resource)) {
        throw std::logic_error("Cannot release a buffer without its permission");
    }

    outgoing_.push_back({makeHeader(tick), Release{request_->id}});
    for (const auto& [peer, attempt] : deferred_) {
        outgoing_.push_back({makeHeader(tick, peer), Reply{attempt}});
    }
    deferred_.clear();
    replies_.clear();
    request_.reset();
    granted_ = false;
}

void AccessProtocol::onMessage(const Message& message, Tick tick)
{
    if (message.header.recipient && *message.header.recipient != id_) {
        return;
    }
    const auto sender = message.header.id.sender;
    const bool peer = sender != id_ && std::binary_search(members_.begin(), members_.end(), sender);
    const auto* request = std::get_if<Request>(&message.payload);
    const auto* reply = std::get_if<Reply>(&message.payload);
    const auto* release = std::get_if<Release>(&message.payload);
    if (request || reply || release) {
        if (!peer) {
            return;
        }
        if (request && (request->id.requester != sender || !isBuffer(request->id.resource))) {
            return;
        }
        if (release &&
            (release->request.requester != sender || !isBuffer(release->request.resource))) {
            return;
        }
    } else if (!peer && sender != ARM_NODE_ID) {
        return;
    }

    clock_ = nextLamportTime(clock_, message.header.clock);
    if (request) {
        onRequest(*request, tick);
    } else if (reply) {
        onReply(static_cast<MachineId>(sender), *reply);
    }
    // RELEASE is informational and never substitutes for a matching REPLY
}

bool AccessProtocol::isWaiting(ResourceId resource) const noexcept
{
    return request_ && request_->id.resource == resource && !granted_;
}

bool AccessProtocol::hasPermission(ResourceId resource) const noexcept
{
    return request_ && request_->id.resource == resource && granted_;
}

std::vector<Message> AccessProtocol::takeOutgoing()
{
    std::vector<Message> messages;
    messages.swap(outgoing_);
    return messages;
}

MessageHeader AccessProtocol::makeHeader(Tick tick, std::optional<NodeId> recipient)
{
    const auto next_clock = nextLamportTime(clock_);
    if (sequence_ == std::numeric_limits<SequenceNumber>::max()) {
        throw std::overflow_error("Protocol message sequence overflow");
    }
    clock_ = next_clock;
    ++sequence_;
    return {{id_, sequence_}, recipient, tick, clock_};
}

void AccessProtocol::onRequest(const Request& request, Tick tick)
{
    const bool same_resource = request_ && request_->id.resource == request.id.resource;
    if (same_resource && (granted_ || requestKey(*request_) < requestKey(request))) {
        deferred_.insert_or_assign(request.id.requester, request.id);
        return;
    }
    outgoing_.push_back({makeHeader(tick, request.id.requester), Reply{request.id}});
}

void AccessProtocol::onReply(MachineId sender, const Reply& reply)
{
    if (!request_ || granted_ || !(reply.request == request_->id)) {
        return;
    }
    replies_.insert(sender);
    granted_ = replies_.size() == members_.size() - 1;
}

} // namespace fleetlink
