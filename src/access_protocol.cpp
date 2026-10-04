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

AccessProtocol::AccessProtocol(MachineId id, std::vector<MachineId> members, Tick retry_interval,
                               Logger* logger, HeartbeatConfig heartbeat, unsigned zone_capacity)
    : id_(id), members_(std::move(members)), retry_interval_(retry_interval), logger_(logger),
      heartbeat_(heartbeat)
{
    if (retry_interval_ == 0 || zone_capacity == 0) {
        throw std::invalid_argument("Retry interval and buffer capacity must be positive");
    }
    if (heartbeat_.interval == 0 || heartbeat_.timeout <= heartbeat_.interval) {
        throw std::invalid_argument("Heartbeat timeout must exceed a positive interval");
    }
    std::sort(members_.begin(), members_.end());
    if (std::adjacent_find(members_.begin(), members_.end()) != members_.end()) {
        throw std::invalid_argument("Duplicate machine id in protocol membership");
    }
    if (!std::binary_search(members_.begin(), members_.end(), id_)) {
        throw std::invalid_argument("Protocol membership must include this machine");
    }
    for (const auto peer : members_) {
        if (peer != id_) {
            peers_.emplace(peer, PeerHealth{});
        }
    }
    for (const auto resource : {ResourceId::LOADING_BUFFER, ResourceId::UNLOADING_BUFFER,
                                ResourceId::LOADING_SERVICE, ResourceId::UNLOADING_SERVICE}) {
        resources_[resource].capacity = isBuffer(resource) ? zone_capacity : 1;
    }
}

RequestId AccessProtocol::request(ResourceId resource, Tick tick)
{
    if (!resources_.count(resource)) {
        throw std::invalid_argument("Unknown resource");
    }
    if (isBuffer(resource) && (resources_.at(ResourceId::LOADING_BUFFER).request ||
                               resources_.at(ResourceId::UNLOADING_BUFFER).request)) {
        throw std::logic_error("Release the current buffer before requesting another");
    }
    const auto buffer = resource == ResourceId::LOADING_SERVICE ? ResourceId::LOADING_BUFFER
                                                                : ResourceId::UNLOADING_BUFFER;
    auto& state = resources_.at(resource);
    if (state.request || (!isBuffer(resource) && !hasPermission(buffer))) {
        throw std::logic_error("Service requires its buffer and no existing service request");
    }

    const auto header = makeHeader(tick);
    const Request attempt{{resource, id_, header.id.sequence}, header.clock};
    outgoing_.push_back({header, attempt});
    state.request = attempt;
    state.last_request_tick = tick;
    state.replies.clear();
    state.granted = false;
    updatePermission(state);
    return attempt.id;
}

void AccessProtocol::release(ResourceId resource, Tick tick)
{
    if (!hasPermission(resource)) {
        throw std::logic_error("Cannot release a resource without its permission");
    }
    const auto service = resource == ResourceId::LOADING_BUFFER ? ResourceId::LOADING_SERVICE
                                                                : ResourceId::UNLOADING_SERVICE;
    if (isBuffer(resource) && resources_.at(service).request) {
        throw std::logic_error("Release service before its buffer");
    }
    auto& state = resources_.at(resource);
    outgoing_.push_back({makeHeader(tick), Release{state.request->id}});
    replyDeferred(state, tick);
    state.replies.clear();
    state.request.reset();
    state.granted = false;
}

void AccessProtocol::cancel(ResourceId resource, Tick tick)
{
    auto& state = resources_.at(resource);
    if (!state.request) {
        return;
    }
    const auto service = resource == ResourceId::LOADING_BUFFER ? ResourceId::LOADING_SERVICE
                                                                : ResourceId::UNLOADING_SERVICE;
    if (isBuffer(resource) && resources_.at(service).request) {
        throw std::logic_error("Cancel service before its buffer");
    }
    outgoing_.push_back({makeHeader(tick), Cancel{state.request->id}});
    replyDeferred(state, tick);
    state.replies.clear();
    state.request.reset();
    state.granted = false;
}

Message AccessProtocol::send(MessagePayload payload, Tick tick, std::optional<NodeId> recipient)
{
    Message message{makeHeader(tick, recipient), payload};
    outgoing_.push_back(message);
    return message;
}

void AccessProtocol::resend(Message message, Tick tick)
{
    clock_ = nextLamportTime(clock_);
    message.header.clock = clock_;
    message.header.sent_tick = tick;
    outgoing_.push_back(message);
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
    const auto* cancel = std::get_if<Cancel>(&message.payload);
    const auto* heartbeat = std::get_if<Heartbeat>(&message.payload);
    if (request || reply || release || cancel || heartbeat) {
        if (!peer) {
            return;
        }
        if (request &&
            (request->id.requester != sender || !resources_.count(request->id.resource))) {
            return;
        }
        if (release && (release->request.requester != sender ||
                        !resources_.count(release->request.resource))) {
            return;
        }
        if (cancel &&
            (cancel->request.requester != sender || !resources_.count(cancel->request.resource))) {
            return;
        }
    } else if (!peer && sender != ARM_NODE_ID) {
        return;
    }

    clock_ = nextLamportTime(clock_, message.header.clock);
    if (heartbeat) {
        onHeartbeat(static_cast<MachineId>(sender), *heartbeat, tick);
        return;
    }
    if (peer && !isActive(static_cast<MachineId>(sender))) {
        return;
    }
    if (request) {
        onRequest(message, tick);
    } else if (reply) {
        if (!onReply(static_cast<MachineId>(sender), *reply)) {
            logMessage("reply_ignored", message, tick);
        }
    } else if (release) {
        closePeerRequest(release->request);
    } else if (cancel) {
        closePeerRequest(cancel->request);
    }
    // RELEASE is informational and never substitutes for a matching REPLY
}

void AccessProtocol::tick(Tick now)
{
    for (auto& [peer, health] : peers_) {
        if (health.active && now > health.last_seen &&
            now - health.last_seen > heartbeat_.timeout) {
            logPeer("peer_suspected", peer, now, LogLevel::WARN);
            health.active = false;
            for (auto& [resource, state] : resources_) {
                state.replies.erase(peer);
            }
            logPeer("peer_excluded", peer, now, LogLevel::WARN);
        }
    }
    for (auto& [resource, state] : resources_) {
        updatePermission(state);
    }
    if (now >= last_heartbeat_tick_ && now - last_heartbeat_tick_ >= heartbeat_.interval) {
        const auto header = makeHeader(now);
        outgoing_.push_back({header, Heartbeat{header.id.sequence}});
        last_heartbeat_tick_ = now;
    }
    retry(now);
}

bool AccessProtocol::isActive(MachineId peer) const noexcept
{
    const auto found = peers_.find(peer);
    return peer == id_ || (found != peers_.end() && found->second.active);
}

void AccessProtocol::retry(Tick tick)
{
    for (auto& [resource, state] : resources_) {
        if (!state.request || state.granted || tick < state.last_request_tick ||
            tick - state.last_request_tick < retry_interval_) {
            continue;
        }
        outgoing_.push_back({makeHeader(tick), *state.request});
        state.last_request_tick = tick;
        logMessage("request_retry", outgoing_.back(), tick);
    }
}

void AccessProtocol::acknowledge(MessageId message, Tick tick)
{
    outgoing_.push_back({makeHeader(tick, message.sender), Ack{message}});
}

bool AccessProtocol::isWaiting(ResourceId resource) const noexcept
{
    const auto found = resources_.find(resource);
    return found != resources_.end() && found->second.request && !found->second.granted;
}

bool AccessProtocol::hasPermission(ResourceId resource) const noexcept
{
    const auto found = resources_.find(resource);
    return found != resources_.end() && found->second.request && found->second.granted;
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

void AccessProtocol::onRequest(const Message& message, Tick tick)
{
    const auto& request = std::get<Request>(message.payload);
    const auto peer = request.id.requester;
    auto& state = resources_.at(request.id.resource);
    const auto known = state.peer_requests.find(peer);
    if (known != state.peer_requests.end() && request.id.sequence <= known->second.id.sequence) {
        const auto& previous = known->second;
        if (!(request.id == previous.id) || previous.released ||
            request.timestamp != previous.timestamp) {
            logMessage("request_ignored", message, tick);
            return;
        }
        // Never retract a reply already issued for this attempt, even after our state changes
        logMessage("request_repeat", message, tick);
        if (previous.replied) {
            outgoing_.push_back({makeHeader(tick, peer), Reply{request.id}});
            return;
        }
    } else {
        state.peer_requests.insert_or_assign(peer, PeerRequest{request.id, request.timestamp});
    }
    auto& remembered = state.peer_requests.at(peer);
    if (state.request && (state.granted || requestKey(*state.request) < requestKey(request))) {
        return;
    }
    outgoing_.push_back({makeHeader(tick, peer), Reply{request.id}});
    remembered.replied = true;
}

bool AccessProtocol::onReply(MachineId sender, const Reply& reply)
{
    const auto found = resources_.find(reply.request.resource);
    if (found == resources_.end()) {
        return false;
    }
    auto& state = found->second;
    if (!state.request || state.granted || !(reply.request == state.request->id)) {
        return false;
    }
    const bool accepted = state.replies.insert(sender).second;
    updatePermission(state);
    return accepted;
}

void AccessProtocol::closePeerRequest(const RequestId& request)
{
    const auto peer = request.requester;
    auto& state = resources_.at(request.resource);
    const auto known = state.peer_requests.find(peer);
    if (known == state.peer_requests.end() || request.sequence >= known->second.id.sequence) {
        state.peer_requests.insert_or_assign(peer, PeerRequest{request, 0, false, true});
    }
}

void AccessProtocol::updatePermission(ResourceState& state)
{
    if (state.request && !state.granted) {
        const auto missing =
            std::count_if(peers_.begin(), peers_.end(), [&state](const auto& peer) {
                return peer.second.active && !state.replies.count(peer.first);
            });
        // At most capacity - 1 active peers may still withhold their replies
        state.granted = static_cast<std::size_t>(missing) < state.capacity;
    }
}

void AccessProtocol::replyDeferred(ResourceState& state, Tick tick)
{
    for (auto& [peer, attempt] : state.peer_requests) {
        if (isActive(peer) && !attempt.replied && !attempt.released) {
            outgoing_.push_back({makeHeader(tick, peer), Reply{attempt.id}});
            attempt.replied = true;
        }
    }
}

void AccessProtocol::onHeartbeat(MachineId peer, const Heartbeat& heartbeat, Tick tick)
{
    auto& health = peers_.at(peer);
    if (health.sequence && heartbeat.sequence <= *health.sequence) {
        logPeer("heartbeat_ignored", peer, tick, LogLevel::TRACE);
        return;
    }
    health.sequence = heartbeat.sequence;
    health.last_seen = tick;
    if (health.active) {
        return;
    }
    health.active = true;
    logPeer("peer_returned", peer, tick, LogLevel::WARN);
    for (auto& [resource, state] : resources_) {
        if (state.request && !state.granted) {
            cancel(resource, tick);
            request(resource, tick);
            logMessage("request_restart", outgoing_.back(), tick);
        }
    }
}

void AccessProtocol::logPeer(const char* event, MachineId peer, Tick tick, LogLevel level)
{
    if (logger_ && logger_->enabled(level)) {
        const auto& health = peers_.at(peer);
        logger_->log({tick,
                      nodeName(id_),
                      level,
                      event,
                      {{"peer", nodeName(peer)},
                       {"last_seen", health.last_seen},
                       {"heartbeat_seq", health.sequence.value_or(0)},
                       {"timeout", heartbeat_.timeout}}});
    }
}

void AccessProtocol::logMessage(const char* event, const Message& message, Tick tick)
{
    if (logger_ && logger_->enabled(LogLevel::DEBUG)) {
        const auto* request = std::get_if<Request>(&message.payload);
        const auto attempt = request ? request->id : std::get<Reply>(message.payload).request;
        logger_->log({tick,
                      nodeName(id_),
                      LogLevel::DEBUG,
                      event,
                      {{"from", nodeName(message.header.id.sender)},
                       {"seq", message.header.id.sequence},
                       {"requester", nodeName(attempt.requester)},
                       {"request_seq", attempt.sequence}}});
    }
}

} // namespace fleetlink
