#include "fleetlink/world.hpp"

#include <doctest/doctest.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>
#include <tuple>
#include <vector>

using namespace fleetlink;

namespace {

constexpr auto buffer = ResourceId::LOADING_BUFFER;
constexpr auto equipment = ResourceId::LOADING_SERVICE;

Message reply(MachineId from, RequestId request)
{
    return {{{from, 1}, request.requester, 0, 20}, Reply{request}};
}

Message request(MachineId from, SequenceNumber sequence, ResourceId resource)
{
    return {{{from, sequence}, std::nullopt, 0, 30}, Request{{resource, from, sequence}, 30}};
}

void start(World& world)
{
    for (const auto& machine : world.snapshot()) {
        world.dispatch(machine.id, MissionTarget::LOAD);
    }
}

std::map<ResourceId, unsigned> occupancy(const World& world, unsigned capacity)
{
    std::map<ResourceId, unsigned> inside;
    unsigned loaders = 0;
    unsigned unloaders = 0;
    for (const auto& machine : world.snapshot()) {
        if (machine.occupied_buffer) {
            ++inside[*machine.occupied_buffer];
        }
        loaders += machine.state == MachineState::LOADING;
        unloaders += machine.state == MachineState::UNLOADING;
        if (machine.state == MachineState::LOADING || machine.state == MachineState::UNLOADING) {
            REQUIRE(machine.occupied_buffer);
        }
    }
    REQUIRE(inside[buffer] <= capacity);
    REQUIRE(inside[ResourceId::UNLOADING_BUFFER] <= capacity);
    REQUIRE(loaders <= 1);
    REQUIRE(unloaders <= 1);
    return inside;
}

using Entry = std::pair<Tick, MachineId>;

std::vector<Entry> protocolRun(const std::vector<MachineId>& order, unsigned capacity)
{
    std::vector<NodeId> nodes(order.begin(), order.end());
    InMemoryTransport transport(nodes);
    std::map<MachineId, AccessProtocol> fleet;
    std::map<MachineId, Tick> leaving;
    std::map<MachineId, unsigned> visits;
    std::vector<Entry> entries;
    for (const auto id : order) {
        fleet.try_emplace(id, id, order, 50, nullptr, HeartbeatConfig{}, capacity);
        fleet.at(id).request(buffer, 0);
    }
    for (Tick tick = 1; tick < 100; ++tick) {
        for (const auto id : order) {
            for (const auto& message : fleet.at(id).takeOutgoing()) {
                transport.send(message);
            }
        }
        transport.advanceTo(tick);
        for (const auto id : order) {
            auto& protocol = fleet.at(id);
            for (const auto& message : transport.receive(id)) {
                protocol.onMessage(message, tick);
                unsigned permissions = 0;
                for (const auto& [peer, other] : fleet) {
                    permissions += other.hasPermission(buffer);
                }
                REQUIRE(permissions <= capacity);
            }
            protocol.tick(tick);
            if (protocol.hasPermission(buffer) && !leaving.count(id)) {
                leaving[id] = tick + 3;
                ++visits[id];
                entries.emplace_back(tick, id);
            }
            if (leaving.count(id) && leaving.at(id) == tick) {
                protocol.release(buffer, tick);
                leaving.erase(id);
                if (visits.at(id) < 3) {
                    protocol.request(buffer, tick);
                }
            }
        }
    }
    for (const auto id : order) {
        CHECK(visits[id] == 3);
    }
    std::sort(entries.begin(), entries.end());
    return entries;
}

} // namespace

TEST_CASE("buffer capacity changes the reply threshold but not equipment exclusivity")
{
    CHECK_THROWS_AS((AccessProtocol(0, {0}, 50, nullptr, {}, 0)), std::invalid_argument);
    MachineConfig invalid;
    invalid.zone_capacity = 0;
    CHECK_THROWS_AS(World({0, 1, 2, 3}, invalid), std::invalid_argument);

    AccessProtocol node(0, {0, 1, 2, 3, 4, 5}, 50, nullptr, {}, 3);
    const auto attempt = node.request(buffer, 0);
    node.onMessage(reply(1, attempt), 1);
    node.onMessage(reply(1, attempt), 1);
    node.onMessage(reply(2, attempt), 1);
    CHECK_FALSE(node.hasPermission(buffer));
    node.onMessage(reply(3, attempt), 1);
    REQUIRE(node.hasPermission(buffer));
    CHECK_THROWS_AS(node.request(ResourceId::UNLOADING_SERVICE, 1), std::logic_error);
    const auto service = node.request(equipment, 1);
    for (MachineId peer = 1; peer < 5; ++peer) {
        node.onMessage(reply(peer, service), 2);
    }
    CHECK_FALSE(node.hasPermission(equipment));
    node.onMessage(reply(5, attempt), 2);
    CHECK_FALSE(node.hasPermission(equipment));
    node.onMessage(reply(5, service), 2);
    REQUIRE(node.hasPermission(equipment));
    CHECK_THROWS_AS(node.release(buffer, 2), std::logic_error);
    CHECK_THROWS_AS(node.cancel(buffer, 2), std::logic_error);
    node.release(equipment, 3);
    CHECK(node.hasPermission(buffer));
    node.release(buffer, 3);
}

TEST_CASE("permission traces respect capacity and remain independent of processing order")
{
    for (const auto capacity : {2U, 3U, 4U, 8U}) {
        INFO("capacity " << capacity);
        const auto first = protocolRun({7, 11, 50, 99}, capacity);
        const auto reverse = protocolRun({99, 50, 11, 7}, capacity);
        CHECK(first == reverse);
        REQUIRE_FALSE(first.empty());
        const auto first_tick = first.front().first;
        CHECK(std::count_if(first.begin(), first.end(), [first_tick](const auto& entry) {
                  return entry.first == first_tick;
              }) == std::min(capacity, 4U));
    }
}

TEST_CASE("peer buffer and equipment requests keep separate deferred replies")
{
    AccessProtocol node(0, {0, 1, 2}, 50, nullptr, {}, 2);
    const auto own = node.request(buffer, 0);
    node.onMessage(reply(2, own), 1);
    REQUIRE(node.hasPermission(buffer));
    node.takeOutgoing();
    const auto waiting = request(1, 2, buffer);
    node.onMessage(waiting, 2);
    CHECK(node.takeOutgoing().empty());
    const auto service = request(1, 3, equipment);
    node.onMessage(service, 2);
    auto messages = node.takeOutgoing();
    REQUIRE(messages.size() == 1);
    CHECK(std::get<Reply>(messages[0].payload).request == std::get<Request>(service.payload).id);
    node.release(buffer, 3);
    messages = node.takeOutgoing();
    REQUIRE(messages.size() == 2);
    CHECK(std::get<Reply>(messages[1].payload).request == std::get<Request>(waiting.payload).id);
    node.onMessage(service, 4);
    messages = node.takeOutgoing();
    REQUIRE(messages.size() == 1);
    CHECK(std::get<Reply>(messages[0].payload).request == std::get<Request>(service.payload).id);
}

TEST_CASE("k exclusion counts only unanswered live peers after an exclusion")
{
    AccessProtocol node(0, {0, 1, 2, 3}, 50, nullptr, {}, 2);
    const auto own = node.request(buffer, 0);
    node.onMessage(reply(1, own), 1);
    for (const auto peer : {2U, 3U}) {
        node.onMessage({{{peer, 2}, std::nullopt, 200, 10}, Heartbeat{2}}, 200);
    }
    node.tick(201);
    CHECK_FALSE(node.isActive(1));
    CHECK_FALSE(node.hasPermission(buffer));
    node.onMessage(reply(2, own), 202);
    CHECK(node.hasPermission(buffer));
}

TEST_CASE("larger buffers allow real simultaneous occupancy and every machine progresses")
{
    for (const auto capacity : {2U, 3U, 6U, 9U}) {
        for (const auto seed : {0U, 7U, 42U}) {
            INFO("capacity " << capacity << ", seed " << seed);
            MachineConfig config;
            config.zone_capacity = capacity;
            const ChannelConfig channel{seed, seed == 0 ? 0.0 : 0.2, 1, 10, 0.05};
            World world({0, 1, 2, 3, 4, 5}, config, nullptr, channel);
            start(world);
            unsigned peak = 0;
            for (unsigned tick = 0; tick < 2500; ++tick) {
                world.step();
                peak = std::max(peak, occupancy(world, capacity)[buffer]);
            }
            CHECK(peak >= std::min(capacity, 3U));
            for (const auto& machine : world.snapshot()) {
                CHECK(machine.completed_loads > 1);
                CHECK(machine.completed_unloads > 1);
            }
        }
    }
}

TEST_CASE("slower unloading fills the second buffer and its equipment remains exclusive in trace")
{
    MachineConfig config;
    config.zone_capacity = 3;
    config.loading_rate = 50;
    config.unloading_rate = 5;
    std::ostringstream output;
    Logger logger(output, LogLevel::INFO);
    World world({0, 1, 2, 3, 4, 5}, config, &logger);
    start(world);
    world.step(1500);
    logger.close();
    std::map<std::string, std::set<std::string>> inside;
    std::map<std::string, unsigned> peak;
    std::map<std::string, std::string> serving;
    unsigned services = 0;
    std::istringstream input(output.str());
    for (std::string line; std::getline(input, line);) {
        const auto row = nlohmann::json::parse(line);
        const auto event = row.at("ev").get<std::string>();
        const auto machine = row.at("node").get<std::string>();
        if (event == "zone_enter") {
            const auto zone = row.at("zone").get<std::string>();
            CHECK(inside[zone].insert(machine).second);
            REQUIRE(inside[zone].size() <= config.zone_capacity);
            peak[zone] = std::max(peak[zone], static_cast<unsigned>(inside[zone].size()));
        } else if (event == "zone_leave") {
            CHECK(inside[row.at("zone").get<std::string>()].erase(machine) == 1);
        } else if (event == "service_start") {
            const auto operation = row.at("operation").get<std::string>();
            CHECK(serving[operation].empty());
            const auto zone = operation == "load" ? "LOADING_BUFFER" : "UNLOADING_BUFFER";
            CHECK(inside[zone].count(machine) == 1);
            serving[operation] = machine;
        } else if (event == "service_finish") {
            const auto operation = row.at("operation").get<std::string>();
            CHECK(serving[operation] == machine);
            serving[operation].clear();
            ++services;
        }
    }
    CHECK(peak["LOADING_BUFFER"] == 3);
    CHECK(peak["UNLOADING_BUFFER"] == 3);
    CHECK(services > 20);
    for (const auto& machine : world.snapshot()) {
        CHECK(machine.completed_unloads > 1);
    }
}

TEST_CASE("HALT retains occupied buffer slots and equipment until every stop is cleared")
{
    MachineConfig config;
    config.zone_capacity = 3;
    World world({0, 1, 2, 3, 4, 5}, config);
    start(world);
    bool reached = false;
    for (unsigned tick = 0; tick < 50 && !reached; ++tick) {
        world.step();
        unsigned waiting = 0;
        unsigned working = 0;
        for (const auto& machine : world.snapshot()) {
            waiting += machine.state == MachineState::IN_ZONE;
            working += machine.state == MachineState::LOADING && machine.load == 40;
        }
        reached = waiting >= 2 && working == 1;
    }
    REQUIRE(reached);
    world.haltAll();
    world.step();
    const auto stopped = world.snapshot();
    world.haltAll(2);
    world.step(250);
    world.resume();
    world.step(250);
    for (const auto& machine : world.snapshot()) {
        CHECK(machine.halted);
        CHECK(machine.state == stopped.at(machine.id).state);
        CHECK(machine.load == stopped.at(machine.id).load);
        CHECK(machine.position == stopped.at(machine.id).position);
        CHECK(machine.occupied_buffer == stopped.at(machine.id).occupied_buffer);
    }
    world.resume(2);
    world.step(1000);
    for (const auto& machine : world.snapshot()) {
        CHECK_FALSE(machine.halted);
        CHECK(machine.completed_unloads > stopped.at(machine.id).completed_unloads);
    }
}

TEST_CASE("failure while owning equipment frees physical occupancy and survivors progress")
{
    MachineConfig config;
    config.zone_capacity = 3;
    World world({0, 1, 2, 3, 4, 5}, config);
    start(world);
    std::optional<MachineId> owner;
    for (unsigned tick = 0; tick < 50 && !owner; ++tick) {
        world.step();
        for (const auto& machine : world.snapshot()) {
            if (machine.state == MachineState::LOADING) {
                owner = machine.id;
            }
        }
    }
    REQUIRE(owner);
    world.fail(*owner);
    REQUIRE_FALSE(world.snapshot().at(*owner).occupied_buffer);
    world.step(250);
    const auto detected = world.snapshot();
    world.step(1000);
    for (const auto& machine : world.snapshot()) {
        if (machine.id != *owner) {
            CHECK(machine.completed_unloads > detected.at(machine.id).completed_unloads);
        }
    }
}

TEST_CASE("capacity three produces identical dirty traces after a roster permutation")
{
    MachineConfig config;
    config.zone_capacity = 3;
    const ChannelConfig channel{42, 0.2, 1, 10, 0.05};
    std::ostringstream first_output;
    std::ostringstream second_output;
    Logger first_log(first_output, LogLevel::TRACE);
    Logger second_log(second_output, LogLevel::TRACE);
    World first({0, 1, 2, 3, 4, 5}, config, &first_log, channel);
    World second({5, 3, 1, 4, 2, 0}, config, &second_log, channel);
    start(first);
    start(second);
    first.step(500);
    second.step(500);
    first_log.close();
    second_log.close();
    CHECK(first_output.str() == second_output.str());
}

TEST_CASE(
    "HALT on the final equipment reply cancels only service and rejects old replies on resume")
{
    for (const bool halt_first : {false, true}) {
        MachineConfig config;
        config.zone_capacity = 3;
        config.loading_rate = 25;
        Machine machine(0, {0, 1, 2}, config);
        machine.tick(1, {{{{ARM_NODE_ID, 1}, 0, 0, 1}, Mission{1, MissionTarget::LOAD}}});
        machine.tick(2, {});
        REQUIRE(machine.occupiedBuffer() == buffer);
        const auto requests = machine.tick(3, {});
        REQUIRE(requests.size() == 1);
        const auto old = std::get<Request>(requests[0].payload).id;
        REQUIRE(old.resource == equipment);
        const Message halt{{{ARM_NODE_ID, 2}, std::nullopt, 3, 40}, Halt{1}};
        std::vector<Message> incoming{reply(1, old), reply(2, old)};
        incoming.insert(halt_first ? incoming.begin() : incoming.end(), halt);
        const auto cancelled = machine.tick(4, incoming);
        REQUIRE(machine.halted());
        CHECK(machine.occupiedBuffer() == buffer);
        CHECK(machine.state() == MachineState::IN_ZONE);
        CHECK(machine.load() == 0);
        unsigned cancels = 0;
        for (const auto& message : cancelled) {
            if (const auto* cancel = std::get_if<Cancel>(&message.payload)) {
                ++cancels;
                CHECK(cancel->request == old);
            }
        }
        CHECK(cancels == 1);
        const auto resumed =
            machine.tick(5, {{{{ARM_NODE_ID, 3}, std::nullopt, 4, 50}, Resume{2}}});
        std::optional<RequestId> next;
        for (const auto& message : resumed) {
            if (const auto* attempt = std::get_if<Request>(&message.payload)) {
                next = attempt->id;
            }
        }
        REQUIRE(next);
        CHECK(next->resource == equipment);
        CHECK(next->sequence > old.sequence);
        machine.tick(6, {reply(1, old), reply(2, old)});
        CHECK(machine.load() == 0);
        machine.tick(7, {reply(1, *next), reply(2, *next)});
        CHECK(machine.state() == MachineState::LOADING);
        CHECK(machine.load() == 25);
        CHECK(machine.occupiedBuffer() == buffer);
    }
}

TEST_CASE("a returning peer restarts equipment acquisition while retaining the occupied buffer")
{
    AccessProtocol node(0, {0, 1, 2}, 50, nullptr, {}, 3);
    node.request(buffer, 0);
    REQUIRE(node.hasPermission(buffer));
    node.onMessage({{{1, 1}, std::nullopt, 200, 10}, Heartbeat{1}}, 200);
    node.tick(201);
    REQUIRE_FALSE(node.isActive(2));
    const auto old = node.request(equipment, 202);
    node.takeOutgoing();
    node.onMessage({{{2, 1}, std::nullopt, 203, 10}, Heartbeat{1}}, 203);
    const auto messages = node.takeOutgoing();
    REQUIRE(messages.size() == 2);
    CHECK(std::get<Cancel>(messages[0].payload).request == old);
    const auto next = std::get<Request>(messages[1].payload).id;
    CHECK(next.resource == equipment);
    CHECK(next.sequence > old.sequence);
    node.onMessage(reply(1, old), 204);
    node.onMessage(reply(2, old), 204);
    CHECK_FALSE(node.hasPermission(equipment));
    CHECK(node.hasPermission(buffer));
    node.onMessage(reply(1, next), 205);
    node.onMessage(reply(2, next), 205);
    CHECK(node.hasPermission(equipment));
    CHECK(node.hasPermission(buffer));
}
