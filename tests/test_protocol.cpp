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
