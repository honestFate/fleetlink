#include "fleetlink/stop_controller.hpp"

#include <limits>
#include <stdexcept>

namespace fleetlink {

StopController::StopController(NodeId id, std::vector<NodeId> nodes, Tick retry_interval)
    : id_(id), nodes_(nodes.begin(), nodes.end()), retry_interval_(retry_interval)
{
    if (!nodes_.count(id) || nodes_.size() != nodes.size() || retry_interval == 0) {
        throw std::invalid_argument("Invalid stop controller configuration");
    }
}

SequenceNumber StopController::nextSequence() const
{
    const auto found = versions_.find(id_);
    const auto current = found == versions_.end() ? 0 : found->second;
    if (current == std::numeric_limits<SequenceNumber>::max()) {
        throw std::overflow_error("Control sequence overflow");
    }
    return current + 1;
}

void StopController::issue(const Message& command)
{
    if (command.header.id.sender != id_ || command.header.recipient ||
        (!std::holds_alternative<Halt>(command.payload) &&
         !std::holds_alternative<Resume>(command.payload))) {
        throw std::invalid_argument("Expected a locally originated broadcast control command");
    }
    apply(command);
    pending_ = Pending{command, nodes_, command.header.sent_tick};
    pending_->awaiting.erase(id_);
}

bool StopController::onMessage(const Message& message)
{
    if ((message.header.recipient && *message.header.recipient != id_) ||
        !nodes_.count(message.header.id.sender) || message.header.id.sender == id_) {
        return false;
    }
    if (std::holds_alternative<Halt>(message.payload) ||
        std::holds_alternative<Resume>(message.payload)) {
        apply(message);
        return true;
    }
    const auto* ack = std::get_if<Ack>(&message.payload);
    if (ack && message.header.recipient == id_ && pending_ &&
        ack->message == pending_->command.header.id) {
        pending_->awaiting.erase(message.header.id.sender);
    }
    return false;
}

std::optional<Message> StopController::retry(Tick now)
{
    if (!pending_ || pending_->awaiting.empty() || now < pending_->last_sent ||
        now - pending_->last_sent < retry_interval_) {
        return std::nullopt;
    }
    pending_->last_sent = now;
    return pending_->command;
}

void StopController::apply(const Message& command)
{
    const auto* halt = std::get_if<Halt>(&command.payload);
    const auto sequence = halt ? halt->sequence : std::get<Resume>(command.payload).sequence;
    const auto origin = command.header.id.sender;
    const auto found = versions_.find(origin);
    if (found != versions_.end() && sequence <= found->second) {
        return;
    }
    versions_.insert_or_assign(origin, sequence);
    if (halt) {
        stops_.insert(origin);
    } else {
        stops_.erase(origin);
    }
}

} // namespace fleetlink
