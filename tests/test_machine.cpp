#include "fleetlink/machine.hpp"
#include "fleetlink/transport.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <limits>
#include <map>
#include <stdexcept>
#include <utility>
#include <vector>

using namespace fleetlink;

namespace {

constexpr auto loading = ResourceId::LOADING_BUFFER;
constexpr auto unloading = ResourceId::UNLOADING_BUFFER;

Message missionFor(MachineId recipient, MissionTarget target, SequenceNumber sequence = 1)
{
    return {{{ARM_NODE_ID, sequence}, recipient, 0, sequence}, Mission{sequence, target}};
}

Message replyFrom(MachineId sender, const RequestId& request)
{
    return {{{sender, 1}, request.requester, 1, 10}, Reply{request}};
}

Message requestFrom(MachineId sender, ResourceId resource)
{
    return {{{sender, 2}, std::nullopt, 2, 20}, Request{{resource, sender, 1}, 20}};
}

std::vector<NodeId> transportNodes(const std::vector<MachineId>& members)
{
    std::vector<NodeId> nodes(members.begin(), members.end());
    nodes.push_back(ARM_NODE_ID);
    return nodes;
}

// Cycle tests inspect resource messages separately from acknowledgements and heartbeats
std::vector<Message> resourceMessages(std::vector<Message> messages)
{
    messages.erase(std::remove_if(messages.begin(), messages.end(),
                                  [](const auto& message) {
                                      return messageKind(message) == MessageKind::ACK ||
                                             messageKind(message) == MessageKind::HEARTBEAT;
                                  }),
                   messages.end());
    return messages;
}

std::vector<Message> tickMachine(Machine& machine, Tick tick, const std::vector<Message>& incoming)
{
    return resourceMessages(machine.tick(tick, incoming));
}

// A small test driver that exchanges all messages through the real transport
class MachineFleet {
public:
    MachineFleet(std::vector<MachineId> order, MachineConfig config)
        : order_(std::move(order)), channel_(transportNodes(order_))
    {
        for (const auto id : order_) {
            machines_.try_emplace(id, id, order_, config);
        }
    }

    Machine& at(MachineId id) { return machines_.at(id); }

    void dispatch(MachineId id, MissionTarget target)
    {
        channel_.send(missionFor(id, target, ++mission_sequence_));
    }

    std::vector<Message> step()
    {
        channel_.advanceTo(++tick_);
        std::vector<Message> outgoing;
        for (const auto id : order_) {
            const auto messages = at(id).tick(tick_, channel_.receive(id));
            outgoing.insert(outgoing.end(), messages.begin(), messages.end());

            // Check physical occupancy after every machine update
            unsigned loading_count = 0;
            unsigned unloading_count = 0;
            for (const auto& [peer, machine] : machines_) {
                INFO("machine " << peer << " at tick " << tick_);
                loading_count += machine.occupiedBuffer() == loading;
                unloading_count += machine.occupiedBuffer() == unloading;
            }
            REQUIRE(loading_count <= 1);
            REQUIRE(unloading_count <= 1);
        }
        std::sort(outgoing.begin(), outgoing.end(), messageBefore);
        for (const auto& message : outgoing) {
            channel_.send(message);
        }
        channel_.receive(ARM_NODE_ID);
        return resourceMessages(std::move(outgoing));
    }

private:
    std::vector<MachineId> order_;
    InMemoryTransport channel_;
    std::map<MachineId, Machine> machines_;
    Tick tick_ = 0;
    SequenceNumber mission_sequence_ = 0;
};

} // namespace

TEST_CASE("an idle machine stays outside the buffers and still answers peer requests")
{
    Machine machine(1, std::vector<MachineId>{1, 2});
    const auto incoming = requestFrom(2, loading);
    const auto outgoing = tickMachine(machine, 3, {incoming});
    REQUIRE(outgoing.size() == 1);
    REQUIRE(messageKind(outgoing.front()) == MessageKind::REPLY);
    CHECK(std::get<Reply>(outgoing.front().payload).request ==
          std::get<Request>(incoming.payload).id);
    CHECK(machine.state() == MachineState::IDLE);
    CHECK(machine.position() == 0);
    CHECK(machine.load() == 0);
    CHECK_FALSE(machine.occupiedBuffer().has_value());
    CHECK(tickMachine(machine, 4, {}).empty());
}

TEST_CASE("movement clamps at either buffer boundary and requests only upon arrival")
{
    MachineConfig config;
    config.road_length = 10;
    config.speed = 4;
    config.initial_position = 10;
    MissionTarget target = MissionTarget::LOAD;
    ResourceId resource = loading;
    std::vector<std::int64_t> expected{6, 2, 0};
    SUBCASE("towards loading")
    {}
    SUBCASE("towards unloading")
    {
        config.initial_position = 0;
        config.initial_load = 100;
        target = MissionTarget::DUMP;
        resource = unloading;
        expected = {4, 8, 10};
    }

    Machine machine(1, std::vector<MachineId>{1}, config);
    for (Tick tick = 1; tick <= 3; ++tick) {
        const auto outgoing = tickMachine(machine, tick,
                                          tick == 1 ? std::vector<Message>{missionFor(1, target)}
                                                    : std::vector<Message>{});
        CHECK(machine.position() == expected.at(tick - 1));
        CHECK(machine.load() == config.initial_load);
        CHECK_FALSE(machine.occupiedBuffer().has_value());
        if (tick < 3) {
            CHECK(machine.state() == MachineState::EN_ROUTE);
            CHECK(outgoing.empty());
        } else {
            CHECK(machine.state() == MachineState::WAIT_PERMIT);
            REQUIRE(outgoing.size() == 1);
            REQUIRE(messageKind(outgoing.front()) == MessageKind::REQUEST);
            CHECK(std::get<Request>(outgoing.front().payload).id.resource == resource);
        }
    }
    CHECK(tickMachine(machine, 4, {}).empty());
    CHECK(machine.state() == MachineState::IN_ZONE);
    CHECK(machine.occupiedBuffer() == resource);
    CHECK(machine.load() == config.initial_load);
}

TEST_CASE("waiting at a buffer does not occupy it or start service before all replies")
{
    Machine machine(1, std::vector<MachineId>{1, 2, 3});
    const auto outgoing = tickMachine(machine, 1, {missionFor(1, MissionTarget::LOAD)});
    REQUIRE(outgoing.size() == 1);
    const auto attempt = std::get<Request>(outgoing.front().payload).id;
    tickMachine(machine, 2, {replyFrom(2, attempt)});
    CHECK(machine.state() == MachineState::WAIT_PERMIT);
    CHECK_FALSE(machine.occupiedBuffer().has_value());
    CHECK(machine.load() == 0);
    CHECK(machine.position() == 0);
    CHECK(tickMachine(machine, 3, {}).empty());
    CHECK(machine.state() == MachineState::WAIT_PERMIT);

    tickMachine(machine, 4, {replyFrom(3, attempt)});
    CHECK(machine.state() == MachineState::IN_ZONE);
    CHECK(machine.occupiedBuffer() == loading);
    CHECK(machine.load() == 0);
    tickMachine(machine, 5, {});
    CHECK(machine.state() == MachineState::LOADING);
    CHECK(machine.occupiedBuffer() == loading);
    CHECK(machine.load() == 20);
}

TEST_CASE("one mission through transport starts repeated loading and unloading cycles")
{
    MachineConfig config;
    config.road_length = 10;
    config.speed = 4;
    config.loading_rate = 40;
    config.unloading_rate = 60;
    MachineFleet fleet({7}, config);
    fleet.dispatch(7, MissionTarget::LOAD);

    struct Expected {
        MachineState state;
        std::int64_t position;
        int load;
        std::optional<ResourceId> buffer;
        std::optional<MessageKind> sent;
    };
    const std::vector<Expected> expected{
        {MachineState::WAIT_PERMIT, 0, 0, {}, MessageKind::REQUEST},
        {MachineState::IN_ZONE, 0, 0, loading, {}},
        {MachineState::LOADING, 0, 40, loading, {}},
        {MachineState::LOADING, 0, 80, loading, {}},
        {MachineState::EN_ROUTE, 0, 100, {}, MessageKind::RELEASE},
        {MachineState::EN_ROUTE, 4, 100, {}, {}},
        {MachineState::EN_ROUTE, 8, 100, {}, {}},
        {MachineState::WAIT_PERMIT, 10, 100, {}, MessageKind::REQUEST},
        {MachineState::IN_ZONE, 10, 100, unloading, {}},
        {MachineState::UNLOADING, 10, 40, unloading, {}},
        {MachineState::EN_ROUTE, 10, 0, {}, MessageKind::RELEASE},
        {MachineState::EN_ROUTE, 6, 0, {}, {}},
        {MachineState::EN_ROUTE, 2, 0, {}, {}},
        {MachineState::WAIT_PERMIT, 0, 0, {}, MessageKind::REQUEST},
        {MachineState::IN_ZONE, 0, 0, loading, {}},
        {MachineState::LOADING, 0, 40, loading, {}},
    };
    for (const auto& step : expected) {
        const auto outgoing = fleet.step();
        const auto& machine = fleet.at(7);
        CHECK(machine.state() == step.state);
        CHECK(machine.position() == step.position);
        CHECK(machine.load() == step.load);
        CHECK(machine.occupiedBuffer() == step.buffer);
        if (step.sent) {
            REQUIRE(outgoing.size() == 1);
            CHECK(messageKind(outgoing.front()) == *step.sent);
        } else {
            CHECK(outgoing.empty());
        }
    }
}

TEST_CASE("service processes requests and sends deferred replies after physically leaving")
{
    MachineConfig config;
    config.loading_rate = 40;
    Machine machine(1, std::vector<MachineId>{1, 2, 3}, config);
    const auto first = tickMachine(machine, 1, {missionFor(1, MissionTarget::LOAD)});
    REQUIRE(first.size() == 1);
    const auto attempt = std::get<Request>(first.front().payload).id;
    tickMachine(machine, 2, {replyFrom(2, attempt), replyFrom(3, attempt)});
    REQUIRE(machine.occupiedBuffer() == loading);

    const auto waiting = requestFrom(2, loading);
    const auto other_buffer = requestFrom(3, unloading);
    const auto replies = tickMachine(machine, 3, {waiting, other_buffer});
    REQUIRE(replies.size() == 1);
    REQUIRE(messageKind(replies.front()) == MessageKind::REPLY);
    CHECK(replies.front().header.recipient == NodeId{3});
    CHECK(machine.state() == MachineState::LOADING);
    CHECK(machine.load() == 40);
    CHECK(machine.occupiedBuffer() == loading);
    CHECK(tickMachine(machine, 4, {}).empty());

    const auto released = tickMachine(machine, 5, {});
    REQUIRE(released.size() == 2);
    REQUIRE(messageKind(released[0]) == MessageKind::RELEASE);
    CHECK(std::get<Release>(released[0].payload).request == attempt);
    REQUIRE(messageKind(released[1]) == MessageKind::REPLY);
    CHECK(std::get<Reply>(released[1].payload).request == std::get<Request>(waiting.payload).id);
    CHECK(released[1].header.recipient == NodeId{2});
    CHECK_FALSE(machine.occupiedBuffer().has_value());
    CHECK(machine.state() == MachineState::EN_ROUTE);
    CHECK(machine.load() == 100);
    CHECK(tickMachine(machine, 6, {}).empty());
    CHECK(machine.position() > 0);
}

TEST_CASE("a new mission is applied only after the current service and exit")
{
    Tick replacement_tick = 2;
    SUBCASE("while waiting")
    {}
    SUBCASE("after entering")
    {
        replacement_tick = 3;
    }
    SUBCASE("during service")
    {
        replacement_tick = 4;
    }
    SUBCASE("on the final service tick")
    {
        replacement_tick = 5;
    }
    MachineConfig config;
    config.loading_rate = 40;
    Machine machine(1, std::vector<MachineId>{1}, config);
    const auto first = tickMachine(machine, 1, {missionFor(1, MissionTarget::LOAD)});
    REQUIRE(first.size() == 1);
    const auto original = std::get<Request>(first.front().payload).id;
    for (Tick tick = 2; tick <= 5; ++tick) {
        const auto outgoing = tickMachine(
            machine, tick,
            tick == replacement_tick ? std::vector<Message>{missionFor(1, MissionTarget::LOAD, 2)}
                                     : std::vector<Message>{});
        if (tick < 5) {
            CHECK(machine.occupiedBuffer() == loading);
            CHECK(outgoing.empty());
        } else {
            REQUIRE(outgoing.size() == 1);
            REQUIRE(messageKind(outgoing.front()) == MessageKind::RELEASE);
            CHECK(std::get<Release>(outgoing.front().payload).request == original);
            CHECK_FALSE(machine.occupiedBuffer().has_value());
        }
    }
    const auto next = tickMachine(machine, 6, {missionFor(1, MissionTarget::DUMP, 1)});
    REQUIRE(next.size() == 1);
    REQUIRE(messageKind(next.front()) == MessageKind::REQUEST);
    const auto request = std::get<Request>(next.front().payload);
    CHECK(request.id.resource == loading);
    CHECK(request.id.sequence > original.sequence);
    CHECK(machine.state() == MachineState::WAIT_PERMIT);

    tickMachine(machine, 7, {missionFor(1, MissionTarget::LOAD, 2)});
    CHECK(machine.occupiedBuffer() == loading);
    tickMachine(machine, 8, {});
    CHECK_FALSE(machine.occupiedBuffer().has_value());
    CHECK(tickMachine(machine, 9, {}).empty());
    CHECK(machine.position() > 0);
}

TEST_CASE("a replacement mission does not turn a moving machine before its current service")
{
    MachineConfig config;
    config.road_length = 10;
    config.speed = 4;
    config.loading_rate = 50;
    config.initial_position = 10;
    Machine machine(1, std::vector<MachineId>{1}, config);
    tickMachine(machine, 1, {missionFor(1, MissionTarget::LOAD)});
    CHECK(machine.position() == 6);
    CHECK(tickMachine(machine, 2, {missionFor(1, MissionTarget::DUMP, 2)}).empty());
    CHECK(machine.position() == 2);
    const auto outgoing = tickMachine(machine, 3, {});
    REQUIRE(outgoing.size() == 1);
    REQUIRE(messageKind(outgoing.front()) == MessageKind::REQUEST);
    CHECK(std::get<Request>(outgoing.front().payload).id.resource == loading);
    tickMachine(machine, 4, {});
    CHECK(machine.occupiedBuffer() == loading);
    tickMachine(machine, 5, {});
    CHECK(machine.load() == 50);
    tickMachine(machine, 6, {});
    CHECK(machine.load() == 100);
    CHECK_FALSE(machine.occupiedBuffer().has_value());
    tickMachine(machine, 7, {});
    CHECK(machine.position() == 4);
}

TEST_CASE("only the latest addressed ARM mission starts an idle machine")
{
    Machine machine(1, std::vector<MachineId>{1, 2});
    auto spoofed = missionFor(1, MissionTarget::LOAD);
    spoofed.header.id.sender = 2;
    auto broadcast = missionFor(1, MissionTarget::LOAD);
    broadcast.header.recipient.reset();
    // Deliberately malformed target to test rejection of an invalid mission
    // NOLINTNEXTLINE(clang-analyzer-optin.core.EnumCastOutOfRange)
    const auto invalid = missionFor(1, static_cast<MissionTarget>(99));
    CHECK(tickMachine(machine, 1, {spoofed, broadcast, invalid, missionFor(2, MissionTarget::LOAD)})
              .empty());
    CHECK(machine.state() == MachineState::IDLE);
    CHECK(
        tickMachine(machine, 2,
                    {missionFor(1, MissionTarget::DUMP, 2), missionFor(1, MissionTarget::LOAD, 1)})
            .empty());
    CHECK(machine.state() == MachineState::EN_ROUTE);
    CHECK(machine.position() == 10);
}

TEST_CASE("six machines share both buffers while waiting at the same boundary")
{
    const std::vector<MachineId> ids{0, 3, 7, 11, 20, 99};
    MachineConfig config;
    config.road_length = 8;
    config.speed = 2;
    config.loading_rate = 25;
    config.unloading_rate = 25;
    MachineFleet fleet(ids, config);
    std::map<MachineId, unsigned> loaded;
    std::map<MachineId, unsigned> unloaded;
    for (const auto id : ids) {
        fleet.dispatch(id, MissionTarget::LOAD);
        loaded.emplace(id, 0);
        unloaded.emplace(id, 0);
    }
    fleet.step();
    for (const auto id : ids) {
        CHECK(fleet.at(id).position() == 0);
        CHECK(fleet.at(id).state() == MachineState::WAIT_PERMIT);
        CHECK_FALSE(fleet.at(id).occupiedBuffer().has_value());
    }

    for (unsigned tick = 0; tick < 120; ++tick) {
        const auto outgoing = fleet.step();
        for (const auto& message : outgoing) {
            if (const auto* release = std::get_if<Release>(&message.payload)) {
                const auto id = release->request.requester;
                CHECK_FALSE(fleet.at(id).occupiedBuffer().has_value());
                if (release->request.resource == loading) {
                    ++loaded.at(id);
                    CHECK(fleet.at(id).load() == 100);
                } else {
                    ++unloaded.at(id);
                    CHECK(fleet.at(id).load() == 0);
                }
            }
        }
        for (const auto id : ids) {
            const auto& machine = fleet.at(id);
            CHECK(machine.position() >= 0);
            CHECK(machine.position() <= config.road_length);
            CHECK(machine.load() >= 0);
            CHECK(machine.load() <= 100);
        }
    }
    for (const auto id : ids) {
        INFO("machine " << id);
        CHECK(loaded.at(id) > 0);
        CHECK(unloaded.at(id) > 0);
    }
}

TEST_CASE("invalid configuration rejects motion or service that could never complete")
{
    MachineConfig config;
    SUBCASE("empty road")
    {
        config.road_length = 0;
    }
    SUBCASE("zero speed")
    {
        config.speed = 0;
    }
    SUBCASE("negative speed")
    {
        config.speed = -1;
    }
    SUBCASE("zero loading rate")
    {
        config.loading_rate = 0;
    }
    SUBCASE("excessive loading rate")
    {
        config.loading_rate = 101;
    }
    SUBCASE("zero unloading rate")
    {
        config.unloading_rate = 0;
    }
    SUBCASE("excessive unloading rate")
    {
        config.unloading_rate = 101;
    }
    SUBCASE("position before the road")
    {
        config.initial_position = -1;
    }
    SUBCASE("position after the road")
    {
        config.initial_position = config.road_length + 1;
    }
    SUBCASE("negative load")
    {
        config.initial_load = -1;
    }
    SUBCASE("excessive load")
    {
        config.initial_load = 101;
    }
    CHECK_THROWS_AS((Machine(1, std::vector<MachineId>{1}, config)), std::invalid_argument);
}

TEST_CASE("duplicate backwards and skipped ticks cannot perform extra movement")
{
    Machine machine(1);
    tickMachine(machine, 1, {missionFor(1, MissionTarget::DUMP)});
    CHECK(machine.position() == 10);
    CHECK_THROWS_AS(tickMachine(machine, 1, {}), std::invalid_argument);
    CHECK_THROWS_AS(tickMachine(machine, 0, {}), std::invalid_argument);
    CHECK_THROWS_AS(tickMachine(machine, 3, {}), std::invalid_argument);
    CHECK(machine.position() == 10);
    CHECK_NOTHROW(tickMachine(machine, 2, {}));
    CHECK(machine.position() == 20);
}

TEST_CASE("large coordinates and speed do not overflow when clamping movement")
{
    MachineConfig config;
    config.road_length = std::numeric_limits<std::int64_t>::max();
    config.speed = config.road_length;
    config.initial_position = config.road_length - 1;
    MissionTarget target = MissionTarget::DUMP;
    auto expected = config.road_length;
    SUBCASE("towards unloading")
    {}
    SUBCASE("towards loading")
    {
        config.initial_position = 1;
        target = MissionTarget::LOAD;
        expected = 0;
    }
    Machine machine(1, std::vector<MachineId>{1}, config);
    tickMachine(machine, 1, {missionFor(1, target)});
    CHECK(machine.position() == expected);
    CHECK(machine.state() == MachineState::WAIT_PERMIT);
}

TEST_CASE("missions are acknowledged again after ACK loss but executed only once")
{
    MachineConfig config;
    config.loading_rate = 100;
    Machine machine(1, std::vector<MachineId>{1}, config);
    const auto mission = missionFor(1, MissionTarget::LOAD);
    auto outgoing = machine.tick(1, {mission});
    REQUIRE(outgoing.size() == 2);
    REQUIRE(messageKind(outgoing.front()) == MessageKind::ACK);
    CHECK(std::get<Ack>(outgoing.front().payload).message == mission.header.id);
    CHECK(outgoing.front().header.recipient == ARM_NODE_ID);
    CHECK(messageKind(outgoing.back()) == MessageKind::REQUEST);
    machine.tick(2, {});
    machine.tick(3, {});
    REQUIRE_FALSE(machine.occupiedBuffer().has_value());
    outgoing = machine.tick(4, {mission, mission});
    REQUIRE(outgoing.size() == 2);
    for (const auto& message : outgoing) {
        REQUIRE(messageKind(message) == MessageKind::ACK);
        CHECK(std::get<Ack>(message.payload).message == mission.header.id);
    }
    CHECK(outgoing[0].header.id.sequence != outgoing[1].header.id.sequence);
    CHECK(machine.state() == MachineState::EN_ROUTE);
    CHECK(machine.position() > 0);
}

TEST_CASE("permanent failure removes physical occupancy and stops every machine activity")
{
    Machine machine(1);
    Tick now = 0;
    SUBCASE("idle")
    {}
    SUBCASE("moving")
    {
        machine.tick(++now, {missionFor(1, MissionTarget::DUMP)});
    }
    SUBCASE("permission obtained before physical entry")
    {
        machine.tick(++now, {missionFor(1, MissionTarget::LOAD)});
        REQUIRE(machine.state() == MachineState::WAIT_PERMIT);
    }
    SUBCASE("inside buffer")
    {
        machine.tick(++now, {missionFor(1, MissionTarget::LOAD)});
        machine.tick(++now, {});
        REQUIRE(machine.occupiedBuffer() == loading);
    }
    SUBCASE("during service")
    {
        machine.tick(++now, {missionFor(1, MissionTarget::LOAD)});
        machine.tick(++now, {});
        machine.tick(++now, {});
        REQUIRE(machine.load() == 20);
    }
    const auto position = machine.position();
    const auto load = machine.load();
    machine.fail();
    machine.fail();
    CHECK(machine.state() == MachineState::FAULT);
    CHECK_FALSE(machine.connected());
    CHECK_FALSE(machine.onRoute());
    CHECK_FALSE(machine.occupiedBuffer().has_value());
    for (unsigned step = 0; step < 20; ++step) {
        CHECK(machine.tick(++now, {missionFor(1, MissionTarget::LOAD, 2), requestFrom(2, loading)})
                  .empty());
        CHECK(machine.position() == position);
        CHECK(machine.load() == load);
        CHECK(machine.state() == MachineState::FAULT);
    }
}

TEST_CASE("known limitation: falsely excluding a live holder can exceed physical capacity")
{
    MachineConfig config;
    config.loading_rate = 1;
    config.heartbeat = {2, 10};
    Machine holder(1, std::vector<MachineId>{1, 2}, config);
    Machine waiter(2, std::vector<MachineId>{1, 2}, config);
    const auto requested = tickMachine(holder, 1, {missionFor(1, MissionTarget::LOAD)});
    const auto permission = tickMachine(waiter, 1, requested);
    tickMachine(holder, 2, permission);
    REQUIRE(holder.occupiedBuffer() == loading);
    const auto waiting = tickMachine(waiter, 2, {missionFor(2, MissionTarget::LOAD)});
    tickMachine(holder, 3, waiting);
    tickMachine(waiter, 3, {});
    // Simulate a link that loses all subsequent traffic, without physically failing either node
    for (Tick now = 4; now <= 11; ++now) {
        holder.tick(now, {});
        waiter.tick(now, {});
    }
    CHECK(holder.onRoute());
    CHECK(waiter.onRoute());
    CHECK(holder.occupiedBuffer() == loading);
    CHECK(waiter.occupiedBuffer() == loading);
    CHECK(holder.load() < 100);
}
