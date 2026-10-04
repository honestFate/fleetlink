#include "fleetlink/world.hpp"

#include <doctest/doctest.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <limits>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>
#include <tuple>
#include <vector>

using namespace fleetlink;

namespace {

using EventKey = std::tuple<Tick, MachineId, ZoneEventKind, ResourceId>;

std::vector<EventKey> eventKeys(const World& world)
{
    std::vector<EventKey> result;
    result.reserve(world.events().size());
    for (const auto& event : world.events()) {
        result.emplace_back(event.tick, event.machine, event.kind, event.resource);
    }
    return result;
}

void dispatchAll(World& world)
{
    for (const auto& machine : world.snapshot()) {
        world.dispatch(machine.id, MissionTarget::LOAD);
    }
}

void checkSnapshots(const World& left, const World& right)
{
    CHECK(left.currentTick() == right.currentTick());
    const auto lhs = left.snapshot();
    const auto rhs = right.snapshot();
    REQUIRE(lhs.size() == rhs.size());
    for (std::size_t index = 0; index < lhs.size(); ++index) {
        CHECK(lhs[index].id == rhs[index].id);
        CHECK(lhs[index].machine_class == rhs[index].machine_class);
        CHECK(lhs[index].state == rhs[index].state);
        CHECK(lhs[index].position == rhs[index].position);
        CHECK(lhs[index].load == rhs[index].load);
        CHECK(lhs[index].connected == rhs[index].connected);
        CHECK(lhs[index].occupied_buffer == rhs[index].occupied_buffer);
        CHECK(lhs[index].completed_loads == rhs[index].completed_loads);
        CHECK(lhs[index].completed_unloads == rhs[index].completed_unloads);
    }
}

} // namespace

TEST_CASE("world dispatches through transport and idle peers participate without missions")
{
    World world({99, 0, 7, 3});
    const auto initial = world.snapshot();
    REQUIRE(initial.size() == 4);
    CHECK(initial[0].id == 0);
    CHECK(initial[1].id == 3);
    CHECK(initial[2].id == 7);
    CHECK(initial[3].id == 99);
    world.dispatch(0, MissionTarget::LOAD);
    world.step(0);
    CHECK(world.currentTick() == 0);
    CHECK(world.events().empty());
    CHECK(world.snapshot()[0].state == MachineState::IDLE);

    world.step();
    CHECK(world.currentTick() == 1);
    CHECK(world.snapshot()[0].state == MachineState::WAIT_PERMIT);
    REQUIRE(world.events().size() == 1);
    CHECK(world.events().front().kind == ZoneEventKind::REQUEST);
    CHECK(world.events().front().tick == 1);
    world.step();
    CHECK(world.snapshot()[0].state == MachineState::WAIT_PERMIT);
    CHECK_FALSE(world.snapshot()[0].occupied_buffer.has_value());
    world.step();
    CHECK(world.snapshot()[0].state == MachineState::IN_ZONE);
    CHECK(world.snapshot()[0].occupied_buffer == ResourceId::LOADING_BUFFER);
    CHECK(world.snapshot()[0].completed_loads == 0);
    CHECK(initial[0].state == MachineState::IDLE);
    CHECK_FALSE(initial[0].occupied_buffer.has_value());

    world.step(2);
    CHECK(world.snapshot()[0].load == 40);
    CHECK(world.snapshot()[0].completed_loads == 0);
    world.step(3);
    const auto after = world.snapshot();
    CHECK(after[0].load == 100);
    CHECK(after[0].completed_loads == 1);
    CHECK_FALSE(after[0].occupied_buffer.has_value());
    CHECK(world.events().back().kind == ZoneEventKind::LEAVE);
    for (std::size_t index = 1; index < after.size(); ++index) {
        CHECK(after[index].state == MachineState::IDLE);
        CHECK(after[index].completed_loads == 0);
        CHECK_FALSE(after[index].occupied_buffer.has_value());
    }
}

TEST_CASE("an idle world advances model time without producing activity")
{
    World world;
    world.step(50);
    CHECK(world.currentTick() == 50);
    CHECK(world.events().empty());
    for (const auto& machine : world.snapshot()) {
        CHECK(machine.state == MachineState::IDLE);
        CHECK(machine.position == 0);
        CHECK(machine.completed_loads == 0);
        CHECK(machine.completed_unloads == 0);
    }
    world.dispatch(3, MissionTarget::LOAD);
    world.step(3);
    CHECK(world.snapshot()[3].state == MachineState::IN_ZONE);
    CHECK(world.events().front().tick == 51);
}

TEST_CASE("the 2000 tick demo trace proves buffer capacity and progress for every machine")
{
    World world;
    dispatchAll(world);
    world.step(2000);
    CHECK(world.currentTick() == 2000);
    REQUIRE_FALSE(world.events().empty());

    std::map<ResourceId, std::set<MachineId>> inside;
    std::map<MachineId, ResourceId> requested;
    std::map<MachineId, std::uint64_t> loads;
    std::map<MachineId, std::uint64_t> unloads;
    Tick previous_tick = 0;
    for (const auto& event : world.events()) {
        INFO("machine " << event.machine << " at tick " << event.tick);
        CHECK(event.tick >= previous_tick);
        CHECK(event.tick <= world.currentTick());
        previous_tick = event.tick;
        auto& occupants = inside[event.resource];
        if (event.kind == ZoneEventKind::REQUEST) {
            CHECK(requested.emplace(event.machine, event.resource).second);
        } else if (event.kind == ZoneEventKind::ENTER) {
            REQUIRE(requested.count(event.machine) == 1);
            CHECK(requested.at(event.machine) == event.resource);
            CHECK(occupants.empty());
            CHECK(occupants.insert(event.machine).second);
        } else {
            CHECK(occupants.erase(event.machine) == 1);
            CHECK(requested.erase(event.machine) == 1);
            if (event.resource == ResourceId::LOADING_BUFFER) {
                ++loads[event.machine];
            } else {
                ++unloads[event.machine];
            }
        }
        CHECK(occupants.size() <= 1);
    }

    const auto snapshot = world.snapshot();
    REQUIRE(snapshot.size() == 6);
    for (const auto& machine : snapshot) {
        INFO("machine " << machine.id);
        CHECK(machine.completed_loads > 0);
        CHECK(machine.completed_unloads > 0);
        CHECK(machine.completed_loads == loads[machine.id]);
        CHECK(machine.completed_unloads == unloads[machine.id]);
        for (const auto resource : {ResourceId::LOADING_BUFFER, ResourceId::UNLOADING_BUFFER}) {
            CHECK((inside[resource].count(machine.id) != 0) ==
                  (machine.occupied_buffer == resource));
        }
    }
}

TEST_CASE("batch and single tick execution have identical snapshots and traces")
{
    World batch;
    World single;
    dispatchAll(batch);
    dispatchAll(single);
    batch.step(2000);
    for (unsigned tick = 0; tick < 2000; ++tick) {
        single.step();
    }
    checkSnapshots(batch, single);
    CHECK(eventKeys(batch) == eventKeys(single));
}

TEST_CASE("world registration order preserves stable ids and the complete event trace")
{
    const std::vector<MachineId> members{0, 3, 7, 11, 20, 99};
    auto reversed = members;
    std::reverse(reversed.begin(), reversed.end());
    World first(members);
    World second(reversed);
    dispatchAll(first);
    dispatchAll(second);
    first.step(2000);
    second.step(2000);
    checkSnapshots(first, second);
    CHECK(eventKeys(first) == eventKeys(second));
}

TEST_CASE("world rejects invalid membership configuration and mission addresses")
{
    CHECK_THROWS_AS(World(std::vector<MachineId>{}), std::invalid_argument);
    CHECK_THROWS_AS(World({0, 1, 2}), std::invalid_argument);
    CHECK_THROWS_AS(World({0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10}), std::invalid_argument);
    CHECK_THROWS_AS(World({0, 1, 2, 2}), std::invalid_argument);
    MachineConfig config;
    config.speed = 0;
    CHECK_THROWS_AS((World({0, 1, 2, 3}, config)), std::invalid_argument);

    World world({0, 3, 7, std::numeric_limits<MachineId>::max()});
    CHECK_THROWS_AS(world.dispatch(1, MissionTarget::LOAD), std::invalid_argument);
    world.step();
    CHECK(world.events().empty());
    CHECK_NOTHROW(world.dispatch(std::numeric_limits<MachineId>::max(), MissionTarget::DUMP));
    world.step();
    CHECK(world.snapshot().back().state == MachineState::EN_ROUTE);
}

TEST_CASE("world detects tick overflow before advancing or consuming queued missions")
{
    World world;
    world.step();
    world.dispatch(0, MissionTarget::LOAD);
    CHECK_THROWS_AS(world.step(std::numeric_limits<Tick>::max()), std::overflow_error);
    CHECK(world.currentTick() == 1);
    CHECK(world.events().empty());
    CHECK(world.snapshot()[0].state == MachineState::IDLE);
    world.step();
    CHECK(world.snapshot()[0].state == MachineState::WAIT_PERMIT);
    CHECK(world.events().front().tick == 2);
}

TEST_CASE("asynchronous logs preserve every zone event and do not affect the model")
{
    std::ostringstream output;
    Logger logger(output);
    World logged({0, 1, 2, 3, 4, 5}, {}, &logger);
    World plain;
    dispatchAll(logged);
    dispatchAll(plain);
    logged.step(2000);
    plain.step(2000);
    logger.close();
    checkSnapshots(logged, plain);
    CHECK(eventKeys(logged) == eventKeys(plain));

    std::vector<EventKey> actual;
    std::istringstream input(output.str());
    for (std::string line; std::getline(input, line);) {
        const auto row = nlohmann::json::parse(line);
        CHECK(row.at("lvl") == "INFO");
        const auto node = row.at("node").get<std::string>();
        REQUIRE(node.front() == 'M');
        const auto id = static_cast<MachineId>(std::stoul(node.substr(1)));
        const auto resource = row.at("zone") == "LOADING_BUFFER" ? ResourceId::LOADING_BUFFER
                                                                 : ResourceId::UNLOADING_BUFFER;
        auto kind = ZoneEventKind::LEAVE;
        if (row.at("ev") == "zone_request") {
            kind = ZoneEventKind::REQUEST;
        } else if (row.at("ev") == "zone_enter") {
            kind = ZoneEventKind::ENTER;
        } else {
            CHECK(row.at("ev") == "zone_leave");
        }
        actual.emplace_back(row.at("tick").get<Tick>(), id, kind, resource);
    }
    CHECK(actual == eventKeys(logged));
}

TEST_CASE("full asynchronous traces retain message identities and are reproducible")
{
    std::ostringstream first_output;
    std::ostringstream second_output;
    Logger first_log(first_output, LogLevel::TRACE);
    Logger second_log(second_output, LogLevel::TRACE);
    World first({0, 3, 7, 11}, {}, &first_log);
    World second({11, 7, 3, 0}, {}, &second_log);
    dispatchAll(first);
    dispatchAll(second);
    first.step(100);
    for (unsigned tick = 0; tick < 100; ++tick) {
        second.step();
    }
    first_log.close();
    second_log.close();
    CHECK(first_output.str() == second_output.str());
    checkSnapshots(first, second);

    using MessageKey = std::pair<std::string, SequenceNumber>;
    std::map<MessageKey, nlohmann::json> sent;
    std::set<std::tuple<std::string, SequenceNumber, std::string>> received;
    std::istringstream input(first_output.str());
    Tick previous_tick = 0;
    unsigned state_changes = 0;
    unsigned releases = 0;
    for (std::string line; std::getline(input, line);) {
        const auto row = nlohmann::json::parse(line);
        const auto tick = row.at("tick").get<Tick>();
        CHECK(tick >= previous_tick);
        previous_tick = tick;
        if (row.at("ev") == "state_change") {
            CHECK(row.at("lvl") == "DEBUG");
            CHECK(row.at("from") != row.at("to"));
            CHECK(row.contains("pos"));
            CHECK(row.contains("load"));
            ++state_changes;
        }
        if (row.at("ev") == "msg_send") {
            CHECK(row.at("lvl") == "TRACE");
            const MessageKey key{row.at("from").get<std::string>(), row.at("seq")};
            CHECK(sent.emplace(key, row).second);
            CHECK(row.at("node") == row.at("from"));
            if (row.at("kind") == "MISSION") {
                CHECK(row.at("target") == "load");
                CHECK(row.contains("mission_seq"));
            }
            if (row.at("kind") == "RELEASE") {
                CHECK(row.contains("request_seq"));
                ++releases;
            }
        }
        if (row.at("ev") == "msg_receive") {
            const MessageKey key{row.at("from").get<std::string>(), row.at("seq")};
            REQUIRE(sent.count(key) == 1);
            const auto to = row.at("to").get<std::string>();
            CHECK(received.emplace(key.first, key.second, to).second);
            CHECK(row.at("node") == to);
            CHECK(row.at("sent_tick") < tick);
            CHECK(row.at("clock") == sent.at(key).at("clock"));
            CHECK(row.at("kind") == sent.at(key).at("kind"));
            CHECK(row.at("sent_tick") == sent.at(key).at("tick"));
        }
    }
    CHECK_FALSE(sent.empty());
    CHECK_FALSE(received.empty());
    CHECK(state_changes > 0);
    CHECK(releases > 0);
}
