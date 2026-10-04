#include "fleetlink/access_protocol.hpp"
#include "fleetlink/transport.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <map>
#include <stdexcept>
#include <utility>
#include <vector>

using namespace fleetlink;

namespace {

constexpr auto loading = ResourceId::LOADING_BUFFER;
constexpr auto unloading = ResourceId::UNLOADING_BUFFER;

Message requestFrom(MachineId sender, SequenceNumber sequence, LamportTime timestamp,
                    ResourceId resource = loading)
{
    return {{{sender, sequence}, std::nullopt, 0, timestamp},
            Request{{resource, sender, sequence}, timestamp}};
}

Message replyFrom(NodeId sender, const RequestId& request, SequenceNumber sequence = 1)
{
    return {{{sender, sequence}, request.requester, 0, 20}, Reply{request}};
}

std::vector<NodeId> transportNodes(const std::vector<MachineId>& members)
{
    std::vector<NodeId> nodes(members.begin(), members.end());
    nodes.push_back(ARM_NODE_ID);
    return nodes;
}

// Test driver with the same delivery and processing phases as the future World
class ProtocolFleet {
public:
    explicit ProtocolFleet(std::vector<MachineId> order)
        : order_(std::move(order)), channel_(transportNodes(order_))
    {
        for (const auto id : order_) {
            protocols_.try_emplace(id, id, order_);
        }
    }

    AccessProtocol& at(MachineId id) { return protocols_.at(id); }
    Tick tick() const { return tick_; }

    void step()
    {
        for (const auto id : order_) {
            for (const auto& message : at(id).takeOutgoing()) {
                channel_.send(message);
            }
        }
        channel_.advanceTo(++tick_);
        for (const auto id : order_) {
            for (const auto& message : channel_.receive(id)) {
                at(id).onMessage(message, tick_);
            }
        }
        // ARM receives broadcasts but never grants access
        channel_.receive(ARM_NODE_ID);
    }

private:
    std::vector<MachineId> order_;
    InMemoryTransport channel_;
    std::map<MachineId, AccessProtocol> protocols_;
    Tick tick_ = 0;
};

using Entry = std::pair<Tick, MachineId>;

std::vector<Entry> runContended(const std::vector<MachineId>& order)
{
    ProtocolFleet fleet(order);
    std::map<MachineId, unsigned> visits;
    std::optional<MachineId> occupant;
    Tick leave_at = 0;
    constexpr unsigned rounds = 3;
    std::vector<Entry> entries;
    entries.reserve(order.size() * rounds);
    for (const auto id : order) {
        fleet.at(id).request(loading, fleet.tick());
        visits.emplace(id, 0);
    }

    for (unsigned step = 0; step < 200 && entries.size() < order.size() * rounds; ++step) {
        fleet.step();
        std::vector<MachineId> permitted;
        for (const auto id : order) {
            if (fleet.at(id).hasPermission(loading)) {
                permitted.push_back(id);
            }
        }
        REQUIRE(permitted.size() <= 1);
        if (!permitted.empty() && !occupant) {
            occupant = permitted.front();
            entries.emplace_back(fleet.tick(), *occupant);
            ++visits.at(*occupant);
            leave_at = fleet.tick() + 2;
        }
        if (occupant && fleet.tick() == leave_at) {
            const auto id = *occupant;
            fleet.at(id).release(loading, fleet.tick());
            occupant.reset();
            // Immediate re-entry must not overtake machines already waiting
            if (visits.at(id) < rounds) {
                fleet.at(id).request(loading, fleet.tick());
            }
        }
    }
    CHECK(entries.size() == order.size() * rounds);
    for (const auto& [id, count] : visits) {
        INFO("machine " << id);
        CHECK(count == rounds);
    }
    return entries;
}

} // namespace

TEST_CASE("protocol validates membership and permits a lone machine immediately")
{
    CHECK_THROWS_AS(AccessProtocol(1, {}), std::invalid_argument);
    CHECK_THROWS_AS(AccessProtocol(1, {2, 3}), std::invalid_argument);
    CHECK_THROWS_AS(AccessProtocol(1, {1, 2, 1}), std::invalid_argument);

    AccessProtocol node(0, {0});
    CHECK_THROWS_AS(node.request(ResourceId::LOADING_SERVICE, 0), std::invalid_argument);
    CHECK_FALSE(node.isWaiting(loading));
    CHECK_FALSE(node.hasPermission(loading));
    const auto first = node.request(loading, 0);
    CHECK(node.hasPermission(loading));
    CHECK_FALSE(node.hasPermission(unloading));
    node.release(loading, 1);
    CHECK_FALSE(node.hasPermission(loading));
    const auto second = node.request(unloading, 1);
    CHECK(node.hasPermission(unloading));
    CHECK(second.sequence > first.sequence);

    const auto outgoing = node.takeOutgoing();
    REQUIRE(outgoing.size() == 3);
    CHECK(messageKind(outgoing[0]) == MessageKind::REQUEST);
    CHECK(messageKind(outgoing[1]) == MessageKind::RELEASE);
    CHECK(messageKind(outgoing[2]) == MessageKind::REQUEST);
    CHECK(std::get<Request>(outgoing[0].payload).id == first);
    CHECK(std::get<Release>(outgoing[1].payload).request == first);
    CHECK(std::get<Request>(outgoing[2].payload).id == second);
    for (std::size_t index = 0; index < outgoing.size(); ++index) {
        CHECK_FALSE(outgoing[index].header.recipient.has_value());
        CHECK(outgoing[index].header.id.sender == 0);
        CHECK(outgoing[index].header.id.sequence == index + 1);
        CHECK(outgoing[index].header.clock == index + 1);
    }
    CHECK(node.takeOutgoing().empty());
}

TEST_CASE("an idle machine replies immediately with the original request identity")
{
    AccessProtocol node(7, {1, 7});
    const auto incoming = requestFrom(1, 4, 10);
    node.onMessage(incoming, 1);

    const auto outgoing = node.takeOutgoing();
    REQUIRE(outgoing.size() == 1);
    REQUIRE(messageKind(outgoing.front()) == MessageKind::REPLY);
    CHECK(std::get<Reply>(outgoing.front().payload).request ==
          std::get<Request>(incoming.payload).id);
    CHECK(outgoing.front().header.recipient == NodeId{1});
    CHECK(outgoing.front().header.sent_tick == 1);
    CHECK(outgoing.front().header.clock == 12);
    CHECK_FALSE(node.hasPermission(loading));
}

TEST_CASE("a waiting machine yields to an earlier timestamp even with a larger peer id")
{
    AccessProtocol node(1, {1, 9});
    node.onMessage({{{ARM_NODE_ID, 1}, NodeId{1}, 0, 10}, Mission{1, MissionTarget::LOAD}}, 1);
    node.request(loading, 1);
    const auto own = node.takeOutgoing();
    REQUIRE(own.size() == 1);
    CHECK(std::get<Request>(own.front().payload).timestamp == 12);

    auto earlier = requestFrom(9, 1, 1);
    earlier.header.clock = 50;
    node.onMessage(earlier, 2);
    const auto outgoing = node.takeOutgoing();
    REQUIRE(outgoing.size() == 1);
    REQUIRE(messageKind(outgoing.front()) == MessageKind::REPLY);
    CHECK(std::get<Reply>(outgoing.front().payload).request ==
          std::get<Request>(earlier.payload).id);
    CHECK(outgoing.front().header.clock == 52);
    CHECK(node.isWaiting(loading));
}

TEST_CASE("a holder defers even an older request and replies once after release")
{
    AccessProtocol node(9, {1, 9});
    const auto own = node.request(loading, 0);
    node.takeOutgoing();
    node.onMessage(replyFrom(1, own), 1);
    REQUIRE(node.hasPermission(loading));

    const auto incoming = requestFrom(1, 2, 0);
    node.onMessage(incoming, 2);
    node.onMessage(incoming, 2);
    CHECK(node.takeOutgoing().empty());
    CHECK(node.hasPermission(loading));
    node.release(loading, 3);

    const auto outgoing = node.takeOutgoing();
    REQUIRE(outgoing.size() == 2);
    REQUIRE(messageKind(outgoing[0]) == MessageKind::RELEASE);
    CHECK(std::get<Release>(outgoing[0].payload).request == own);
    CHECK_FALSE(outgoing[0].header.recipient.has_value());
    REQUIRE(messageKind(outgoing[1]) == MessageKind::REPLY);
    CHECK(outgoing[1].header.recipient == NodeId{1});
    CHECK(std::get<Reply>(outgoing[1].payload).request == std::get<Request>(incoming.payload).id);
    CHECK(outgoing[0].header.sent_tick == 3);
    CHECK(outgoing[1].header.sent_tick == 3);
    CHECK_FALSE(node.hasPermission(loading));
    CHECK_FALSE(node.isWaiting(loading));
}

TEST_CASE("only distinct peers replying to the current request can grant permission")
{
    AccessProtocol node(1, {4, 3, 2, 1});
    const auto attempt = node.request(loading, 0);
    node.takeOutgoing();

    node.onMessage(replyFrom(2, attempt), 1);
    node.onMessage(replyFrom(2, attempt, 2), 1);
    node.onMessage(replyFrom(1, attempt), 1);
    node.onMessage(replyFrom(ARM_NODE_ID, attempt), 1);
    node.onMessage(replyFrom(99, attempt), 1);
    node.onMessage(replyFrom(3, {unloading, 1, attempt.sequence}), 1);
    node.onMessage(replyFrom(3, {loading, 1, attempt.sequence + 1}), 1);
    node.onMessage(replyFrom(3, {loading, 2, attempt.sequence}), 1);
    auto misaddressed = replyFrom(3, attempt);
    misaddressed.header.recipient = 4;
    node.onMessage(misaddressed, 1);
    CHECK(node.isWaiting(loading));

    node.onMessage(replyFrom(4, attempt), 1);
    CHECK(node.isWaiting(loading));
    node.onMessage({{{3, 1}, std::nullopt, 0, 20}, Release{{loading, 3, 1}}}, 1);
    CHECK(node.isWaiting(loading));
    node.onMessage(replyFrom(3, attempt, 2), 1);
    CHECK(node.hasPermission(loading));
    CHECK(node.takeOutgoing().empty());

    node.release(loading, 2);
    const auto next = node.request(loading, 2);
    node.onMessage(replyFrom(2, attempt, 3), 3);
    node.onMessage(replyFrom(3, next, 3), 3);
    node.onMessage(replyFrom(4, next, 3), 3);
    CHECK(node.isWaiting(loading));
    node.onMessage(replyFrom(2, next, 4), 3);
    CHECK(node.hasPermission(loading));
}

TEST_CASE("request and release reject transitions that could hold two buffers")
{
    AccessProtocol node(1, {1, 2});
    CHECK_THROWS_AS(node.release(loading, 0), std::logic_error);
    const auto attempt = node.request(loading, 0);
    CHECK_THROWS_AS(node.request(loading, 0), std::logic_error);
    CHECK_THROWS_AS(node.request(unloading, 0), std::logic_error);
    CHECK_THROWS_AS(node.release(loading, 0), std::logic_error);
    CHECK(node.isWaiting(loading));
    node.onMessage(replyFrom(2, attempt), 1);
    CHECK_THROWS_AS(node.request(unloading, 1), std::logic_error);
    CHECK_THROWS_AS(node.release(unloading, 1), std::logic_error);
    CHECK(node.hasPermission(loading));
    node.release(loading, 2);
    CHECK_THROWS_AS(node.release(loading, 2), std::logic_error);
    CHECK_NOTHROW(node.request(unloading, 2));
    CHECK(node.isWaiting(unloading));
}

TEST_CASE("foreign requests and ARM protocol messages cannot create deferred permissions")
{
    AccessProtocol node(1, {1, 2});
    auto incoming = requestFrom(2, 1, 1);
    incoming.header.id.sender = ARM_NODE_ID;
    node.onMessage(incoming, 1);
    incoming.header.id.sender = 99;
    node.onMessage(incoming, 1);
    incoming.header.id.sender = 1;
    node.onMessage(incoming, 1);
    incoming = requestFrom(2, 1, 1, ResourceId::LOADING_SERVICE);
    node.onMessage(incoming, 1);
    incoming = requestFrom(2, 1, 1);
    std::get<Request>(incoming.payload).id.requester = 1;
    node.onMessage(incoming, 1);
    CHECK(node.takeOutgoing().empty());
}

TEST_CASE("a missing peer reply keeps a request waiting regardless of elapsed ticks")
{
    AccessProtocol node(1, {1, 2, 3});
    const auto attempt = node.request(loading, 0);
    node.takeOutgoing();
    node.onMessage(replyFrom(2, attempt), 1);
    node.onMessage({{{2, 2}, std::nullopt, 1999, 30}, Heartbeat{1}}, 2000);
    CHECK(node.isWaiting(loading));
    CHECK_FALSE(node.hasPermission(loading));
    CHECK(node.takeOutgoing().empty());
}

TEST_CASE("simultaneous requests choose the smaller stable id and wait for release")
{
    ProtocolFleet fleet({9, 1, 5, 3});
    fleet.at(9).request(loading, 0);
    fleet.at(1).request(loading, 0);
    fleet.step();
    CHECK(fleet.at(1).isWaiting(loading));
    CHECK(fleet.at(9).isWaiting(loading));
    fleet.step();
    CHECK(fleet.at(1).hasPermission(loading));
    CHECK(fleet.at(9).isWaiting(loading));
    for (unsigned step = 0; step < 5; ++step) {
        fleet.step();
        CHECK(fleet.at(1).hasPermission(loading));
        CHECK(fleet.at(9).isWaiting(loading));
    }
    fleet.at(1).release(loading, fleet.tick());
    fleet.step();
    CHECK_FALSE(fleet.at(1).hasPermission(loading));
    CHECK(fleet.at(9).hasPermission(loading));
}

TEST_CASE("the two buffers allow independent owners and independent waiting requests")
{
    ProtocolFleet fleet({4, 3, 2, 1});
    fleet.at(4).request(unloading, 0);
    fleet.at(3).request(loading, 0);
    fleet.at(2).request(unloading, 0);
    fleet.at(1).request(loading, 0);
    fleet.step();
    fleet.step();
    CHECK(fleet.at(1).hasPermission(loading));
    CHECK(fleet.at(2).hasPermission(unloading));
    CHECK(fleet.at(3).isWaiting(loading));
    CHECK(fleet.at(4).isWaiting(unloading));

    fleet.at(1).release(loading, fleet.tick());
    fleet.step();
    CHECK(fleet.at(3).hasPermission(loading));
    CHECK(fleet.at(2).hasPermission(unloading));
    CHECK(fleet.at(4).isWaiting(unloading));
    fleet.at(2).release(unloading, fleet.tick());
    fleet.step();
    CHECK(fleet.at(3).hasPermission(loading));
    CHECK(fleet.at(4).hasPermission(unloading));
}

TEST_CASE("six contending machines make progress with a repeatable entry trace")
{
    const std::vector<MachineId> order{0, 3, 7, 11, 20, 99};
    const auto trace = runContended(order);
    REQUIRE(trace.size() == order.size() * 3);
    for (std::size_t index = 0; index < trace.size(); ++index) {
        CHECK(trace[index].second == order[index % order.size()]);
    }
    CHECK(runContended(order) == trace);

    auto reversed = order;
    std::reverse(reversed.begin(), reversed.end());
    CHECK(runContended(reversed) == trace);
    CHECK(runContended({11, 99, 0, 20, 7, 3}) == trace);
}

TEST_CASE("all registration and processing permutations preserve a four-machine trace")
{
    std::vector<MachineId> order{0, 3, 7, 11};
    const auto expected = runContended(order);
    do {
        CHECK(runContended(order) == expected);
    } while (std::next_permutation(order.begin(), order.end()));
}

TEST_CASE("request retries retain priority and collected replies until permission")
{
    CHECK_THROWS_AS((AccessProtocol(1, {1, 2}, 0)), std::invalid_argument);
    AccessProtocol node(1, {1, 2, 3}, 10);
    const auto attempt = node.request(loading, 5);
    const auto original = node.takeOutgoing().front();
    node.onMessage(replyFrom(2, attempt), 6);
    node.retry(14);
    CHECK(node.takeOutgoing().empty());
    node.retry(15);
    const auto outgoing = node.takeOutgoing();
    REQUIRE(outgoing.size() == 1);
    const auto& repeat = outgoing.front();
    CHECK(repeat.header.id.sequence > original.header.id.sequence);
    CHECK(repeat.header.clock > original.header.clock);
    CHECK(repeat.header.sent_tick == 15);
    CHECK(std::get<Request>(repeat.payload).id == attempt);
    CHECK(requestKey(std::get<Request>(repeat.payload)) ==
          requestKey(std::get<Request>(original.payload)));
    node.retry(15);
    CHECK(node.takeOutgoing().empty());
    node.onMessage(replyFrom(3, attempt), 16);
    CHECK(node.hasPermission(loading));
    node.retry(100);
    CHECK(node.takeOutgoing().empty());
}

TEST_CASE("retries recover both a lost request and a lost immediate reply")
{
    AccessProtocol requester(1, {1, 2}, 10);
    AccessProtocol responder(2, {1, 2}, 10);
    const auto attempt = requester.request(loading, 0);
    requester.takeOutgoing();
    requester.retry(10);
    responder.onMessage(requester.takeOutgoing().front(), 11);
    const auto lost = responder.takeOutgoing();
    REQUIRE(lost.size() == 1);
    responder.request(loading, 11);
    const auto competing = responder.takeOutgoing();
    requester.onMessage(competing.front(), 12);
    CHECK(requester.takeOutgoing().empty());
    requester.retry(20);
    responder.onMessage(requester.takeOutgoing().front(), 21);
    const auto repeated = responder.takeOutgoing();
    REQUIRE(repeated.size() == 1);
    CHECK(std::get<Reply>(repeated.front().payload).request == attempt);
    requester.onMessage(repeated.front(), 22);
    CHECK(requester.hasPermission(loading));
    CHECK(responder.isWaiting(loading));
    requester.release(loading, 23);
    for (const auto& message : requester.takeOutgoing()) {
        responder.onMessage(message, 24);
    }
    CHECK(responder.hasPermission(loading));
}

TEST_CASE("a lost deferred reply is recovered after release without receiving RELEASE")
{
    AccessProtocol holder(1, {1, 2}, 10);
    AccessProtocol waiter(2, {1, 2}, 10);
    holder.request(loading, 0);
    waiter.onMessage(holder.takeOutgoing().front(), 1);
    holder.onMessage(waiter.takeOutgoing().front(), 2);
    REQUIRE(holder.hasPermission(loading));
    waiter.request(loading, 2);
    holder.onMessage(waiter.takeOutgoing().front(), 3);
    CHECK(holder.takeOutgoing().empty());
    holder.release(loading, 4);
    holder.takeOutgoing();
    waiter.retry(12);
    holder.onMessage(waiter.takeOutgoing().front(), 13);
    const auto recovered = holder.takeOutgoing();
    REQUIRE(recovered.size() == 1);
    waiter.onMessage(recovered.front(), 14);
    CHECK(waiter.hasPermission(loading));
    CHECK_FALSE(holder.hasPermission(loading));
}

TEST_CASE("old requests cannot replace newer deferred attempts or resurrect released attempts")
{
    AccessProtocol node(1, {1, 2});
    const auto own = node.request(loading, 0);
    node.takeOutgoing();
    node.onMessage(replyFrom(2, own), 1);
    const auto current = requestFrom(2, 10, 30);
    node.onMessage(current, 2);
    node.onMessage(requestFrom(2, 5, 20), 3);
    node.release(loading, 4);
    const auto outgoing = node.takeOutgoing();
    REQUIRE(outgoing.size() == 2);
    CHECK(std::get<Reply>(outgoing.back().payload).request ==
          std::get<Request>(current.payload).id);
    node.onMessage({{{2, 11}, std::nullopt, 5, 40}, Release{{loading, 2, 10}}}, 6);
    node.onMessage(current, 7);
    CHECK(node.takeOutgoing().empty());
    node.onMessage({{{2, 21}, std::nullopt, 8, 50}, Release{{unloading, 2, 20}}}, 9);
    node.onMessage(requestFrom(2, 20, 45, unloading), 10);
    CHECK(node.takeOutgoing().empty());
    node.onMessage(requestFrom(2, 30, 60), 11);
    REQUIRE(node.takeOutgoing().size() == 1);
}

TEST_CASE("heartbeats are periodic and keep increasing in idle waiting and held states")
{
    AccessProtocol node(1, {1, 2}, 50, nullptr, {5, 200});
    node.tick(4);
    CHECK(node.takeOutgoing().empty());
    node.tick(5);
    auto outgoing = node.takeOutgoing();
    REQUIRE(outgoing.size() == 1);
    REQUIRE(messageKind(outgoing.front()) == MessageKind::HEARTBEAT);
    const auto first = std::get<Heartbeat>(outgoing.front().payload).sequence;
    CHECK_FALSE(outgoing.front().header.recipient.has_value());
    const auto attempt = node.request(loading, 6);
    node.takeOutgoing();
    node.tick(9);
    CHECK(node.takeOutgoing().empty());
    node.tick(10);
    outgoing = node.takeOutgoing();
    REQUIRE(outgoing.size() == 1);
    CHECK(std::get<Heartbeat>(outgoing.front().payload).sequence > first);
    node.onMessage(replyFrom(2, attempt), 11);
    REQUIRE(node.hasPermission(loading));
    node.tick(15);
    outgoing = node.takeOutgoing();
    REQUIRE(outgoing.size() == 1);
    CHECK(messageKind(outgoing.front()) == MessageKind::HEARTBEAT);
    CHECK(node.hasPermission(loading));
}

TEST_CASE("failure detection has a startup grace and only new peer heartbeats refresh it")
{
    CHECK_THROWS_AS((AccessProtocol(1, {1, 2}, 50, nullptr, {0, 10})), std::invalid_argument);
    CHECK_THROWS_AS((AccessProtocol(1, {1, 2}, 50, nullptr, {10, 10})), std::invalid_argument);
    AccessProtocol node(1, {1, 2}, 50, nullptr, {2, 10});
    const Message heartbeat{{{2, 10}, std::nullopt, 1, 10}, Heartbeat{10}};
    node.tick(10);
    CHECK(node.isActive(2));
    node.tick(11);
    CHECK_FALSE(node.isActive(2));
    node.onMessage(heartbeat, 12);
    CHECK(node.isActive(2));
    node.onMessage(heartbeat, 20);
    node.onMessage({{{2, 9}, std::nullopt, 1, 9}, Heartbeat{9}}, 21);
    node.onMessage({{{2, 20}, std::nullopt, 21, 20}, Request{{loading, 2, 20}, 20}}, 22);
    node.tick(22);
    CHECK(node.isActive(2));
    node.tick(23);
    CHECK_FALSE(node.isActive(2));
    node.onMessage(heartbeat, 24);
    CHECK_FALSE(node.isActive(2));
    node.onMessage({{{ARM_NODE_ID, 100}, NodeId{1}, 24, 100}, Heartbeat{100}}, 25);
    CHECK_FALSE(node.isActive(2));
    CHECK(node.isActive(1));
    CHECK_FALSE(node.isActive(99));
    node.onMessage({{{2, 11}, std::nullopt, 2, 11}, Heartbeat{11}}, 26);
    CHECK(node.isActive(2));
    node.tick(36);
    CHECK(node.isActive(2));
    node.tick(37);
    CHECK_FALSE(node.isActive(2));
}

TEST_CASE("excluding a replied peer does not substitute for a missing live peer reply")
{
    AccessProtocol node(0, {0, 1, 2, 3}, 50, nullptr, {2, 10});
    const auto attempt = node.request(loading, 0);
    node.onMessage(replyFrom(1, attempt), 1);
    node.onMessage(replyFrom(2, attempt), 1);
    for (const MachineId peer : {2, 3}) {
        node.onMessage({{{peer, 2}, std::nullopt, 5, 25}, Heartbeat{2}}, 5);
    }
    node.tick(11);
    CHECK_FALSE(node.isActive(1));
    CHECK(node.isActive(2));
    CHECK(node.isActive(3));
    CHECK(node.isWaiting(loading));
    node.onMessage(replyFrom(3, attempt), 12);
    CHECK(node.hasPermission(loading));
}

TEST_CASE("a missing unreplied peer is excluded and a waiting request can progress")
{
    AccessProtocol node(0, {0, 1, 2}, 50, nullptr, {2, 10});
    const auto attempt = node.request(loading, 0);
    node.onMessage(replyFrom(1, attempt), 1);
    node.onMessage({{{1, 2}, std::nullopt, 5, 25}, Heartbeat{2}}, 5);
    node.tick(10);
    CHECK(node.isWaiting(loading));
    node.tick(11);
    CHECK_FALSE(node.isActive(2));
    CHECK(node.hasPermission(loading));
    node.release(loading, 12);
    node.request(unloading, 12);
    CHECK(node.isWaiting(unloading));
}

TEST_CASE("a returning peer restarts a pending attempt and old replies cannot grant it")
{
    AccessProtocol node(0, {0, 1, 2}, 50, nullptr, {2, 10});
    const auto old = node.request(loading, 0);
    node.onMessage(replyFrom(1, old), 1);
    node.onMessage({{{2, 2}, std::nullopt, 5, 25}, Heartbeat{2}}, 5);
    node.tick(11);
    REQUIRE_FALSE(node.isActive(1));
    REQUIRE(node.isWaiting(loading));
    node.takeOutgoing();
    node.onMessage({{{1, 3}, std::nullopt, 12, 30}, Heartbeat{3}}, 12);
    CHECK(node.isActive(1));
    const auto outgoing = node.takeOutgoing();
    REQUIRE(outgoing.size() == 2);
    REQUIRE(messageKind(outgoing[0]) == MessageKind::CANCEL);
    CHECK(std::get<Cancel>(outgoing[0].payload).request == old);
    REQUIRE(messageKind(outgoing[1]) == MessageKind::REQUEST);
    const auto next = std::get<Request>(outgoing[1].payload).id;
    CHECK(next.sequence > old.sequence);
    node.onMessage(replyFrom(2, old), 13);
    node.onMessage(replyFrom(1, old), 13);
    CHECK(node.isWaiting(loading));
    node.onMessage(replyFrom(2, next), 14);
    CHECK(node.isWaiting(loading));
    node.onMessage(replyFrom(1, next), 14);
    CHECK(node.hasPermission(loading));
}

TEST_CASE("a returning peer does not release an already held resource")
{
    AccessProtocol node(0, {0, 1}, 50, nullptr, {2, 10});
    node.request(loading, 0);
    node.tick(11);
    REQUIRE(node.hasPermission(loading));
    node.takeOutgoing();
    node.onMessage({{{1, 3}, std::nullopt, 12, 30}, Heartbeat{3}}, 12);
    CHECK(node.isActive(1));
    CHECK(node.hasPermission(loading));
    CHECK(node.takeOutgoing().empty());
    node.onMessage(requestFrom(1, 4, 31), 13);
    CHECK(node.takeOutgoing().empty());
    node.release(loading, 14);
    REQUIRE(node.takeOutgoing().size() == 2);
}

TEST_CASE("CANCEL closes a deferred peer attempt without granting or releasing our resource")
{
    AccessProtocol node(0, {0, 1});
    const auto own = node.request(loading, 0);
    node.takeOutgoing();
    node.onMessage(replyFrom(1, own), 1);
    node.onMessage(requestFrom(1, 2, 30), 2);
    node.onMessage({{{1, 3}, std::nullopt, 3, 31}, Cancel{{loading, 1, 2}}}, 3);
    node.onMessage(requestFrom(1, 2, 30), 4);
    CHECK(node.takeOutgoing().empty());
    CHECK(node.hasPermission(loading));
    node.release(loading, 5);
    const auto outgoing = node.takeOutgoing();
    REQUIRE(outgoing.size() == 1);
    CHECK(messageKind(outgoing.front()) == MessageKind::RELEASE);
}

TEST_CASE("a returning waiter can recover a reply skipped while it was excluded")
{
    AccessProtocol node(0, {0, 1}, 50, nullptr, {2, 10});
    const auto own = node.request(loading, 0);
    node.onMessage(replyFrom(1, own), 1);
    const auto waiting = requestFrom(1, 2, 30);
    node.onMessage(waiting, 2);
    node.tick(11);
    REQUIRE_FALSE(node.isActive(1));
    node.takeOutgoing();
    node.release(loading, 12);
    const auto released = node.takeOutgoing();
    REQUIRE(released.size() == 1);
    CHECK(messageKind(released.front()) == MessageKind::RELEASE);
    node.onMessage({{{1, 3}, std::nullopt, 13, 31}, Heartbeat{3}}, 13);
    node.onMessage(waiting, 14);
    const auto recovered = node.takeOutgoing();
    REQUIRE(recovered.size() == 1);
    REQUIRE(messageKind(recovered.front()) == MessageKind::REPLY);
    CHECK(std::get<Reply>(recovered.front().payload).request ==
          std::get<Request>(waiting.payload).id);
}
