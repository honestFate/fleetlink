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
        CHECK(lhs[index].on_route == rhs[index].on_route);
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

TEST_CASE("S2 preserves physical capacity and every machine progresses across fixed seeds")
{
    for (const std::uint64_t seed : {0, 1, 7, 42, 12345}) {
        INFO("seed " << seed);
        World world({0, 1, 2, 3, 4, 5}, {}, nullptr, {seed, 0.2, 1, 10, 0.05});
        dispatchAll(world);
        world.step(5000);
        std::map<ResourceId, std::set<MachineId>> inside;
        std::map<MachineId, Tick> waiting;
        for (const auto& event : world.events()) {
            auto& occupants = inside[event.resource];
            if (event.kind == ZoneEventKind::REQUEST) {
                CHECK(waiting.emplace(event.machine, event.tick).second);
            } else if (event.kind == ZoneEventKind::ENTER) {
                REQUIRE(waiting.count(event.machine) == 1);
                // An empirical regression budget for these finite scenarios, not a loss bound
                CHECK(event.tick - waiting.at(event.machine) < 1500);
                waiting.erase(event.machine);
                CHECK(occupants.empty());
                CHECK(occupants.insert(event.machine).second);
            } else {
                CHECK(occupants.erase(event.machine) == 1);
            }
            CHECK(occupants.size() <= 1);
        }
        for (const auto& [id, requested] : waiting) {
            INFO("still waiting " << id);
            CHECK(world.currentTick() - requested < 1500);
        }
        for (const auto& machine : world.snapshot()) {
            INFO("machine " << machine.id);
            CHECK(machine.completed_loads > 0);
            CHECK(machine.completed_unloads > 0);
            for (const auto resource : {ResourceId::LOADING_BUFFER, ResourceId::UNLOADING_BUFFER}) {
                CHECK((inside[resource].count(machine.id) != 0) ==
                      (machine.occupied_buffer == resource));
            }
        }
        CHECK(world.channelCounters().lost > 0);
        CHECK(world.channelCounters().duplicates > 0);
    }
}

TEST_CASE("dirty traces explain losses duplication and recovery independently of roster order")
{
    std::ostringstream first_output;
    std::ostringstream second_output;
    Logger first_logger(first_output, LogLevel::TRACE);
    Logger second_logger(second_output, LogLevel::TRACE);
    const ChannelConfig config{42, 0.2, 1, 10, 0.05};
    World first({0, 1, 2, 3, 4, 5}, {}, &first_logger, config);
    World second({5, 3, 1, 4, 0, 2}, {}, &second_logger, config);
    dispatchAll(first);
    dispatchAll(second);
    first.step(500);
    for (unsigned tick = 0; tick < 500; ++tick) {
        second.step();
    }
    first_logger.close();
    second_logger.close();
    CHECK(first_output.str() == second_output.str());
    checkSnapshots(first, second);
    CHECK(eventKeys(first) == eventKeys(second));

    std::map<std::string, std::uint64_t> events;
    std::istringstream input(first_output.str());
    Tick previous = 0;
    for (std::string line; std::getline(input, line);) {
        const auto row = nlohmann::json::parse(line);
        const auto event = row.at("ev").get<std::string>();
        ++events[event];
        const auto tick = row.at("tick").get<Tick>();
        CHECK(tick >= previous);
        previous = tick;
        if (event == "msg_drop" || event == "msg_duplicate") {
            CHECK(row.contains("from"));
            CHECK(row.contains("to"));
            CHECK(row.contains("seq"));
            CHECK(row.contains("sent_tick"));
        }
        if (event == "request_retry") {
            CHECK(row.at("seq") != row.at("request_seq"));
        }
    }
    const auto& counters = first.channelCounters();
    CHECK(events["msg_drop"] == counters.lost);
    CHECK(events["msg_duplicate"] == counters.duplicates);
    CHECK(events["msg_deliver"] == counters.delivered);
    CHECK(events["msg_schedule"] == counters.sent - counters.lost + counters.duplicates);
    CHECK(events["request_retry"] > 0);
    CHECK(events["request_repeat"] > 0);
    CHECK(events["reply_ignored"] > 0);
    CHECK(events["mission_retry"] > 0);
}

TEST_CASE("a completely lost mission is retried but cannot start a machine out of band")
{
    std::ostringstream output;
    Logger logger(output, LogLevel::TRACE);
    World world({0, 1, 2, 3}, {}, &logger, {42, 1, 1, 10, 0});
    world.dispatch(0, MissionTarget::LOAD);
    world.step(150);
    logger.close();
    CHECK(world.events().empty());
    CHECK(world.channelCounters().sent > 4);
    CHECK(world.channelCounters().lost == world.channelCounters().sent);
    CHECK(world.channelCounters().delivered == 0);
    for (const auto& machine : world.snapshot()) {
        CHECK(machine.state == MachineState::IDLE);
    }
    std::istringstream input(output.str());
    std::vector<Tick> retries;
    unsigned missions = 0;
    for (std::string line; std::getline(input, line);) {
        const auto row = nlohmann::json::parse(line);
        if (row.at("ev") == "msg_send" && row.at("kind") == "MISSION") {
            ++missions;
        }
        if (row.at("ev") == "mission_retry") {
            CHECK(row.at("seq") == 1);
            CHECK(row.at("mission_seq") == 1);
            retries.push_back(row.at("tick").get<Tick>());
        }
    }
    CHECK(missions == 4);
    CHECK(retries == std::vector<Tick>{50, 100, 150});
}

TEST_CASE("S3 detects M3 locally and all survivors progress after detection")
{
    std::ostringstream output;
    Logger logger(output, LogLevel::TRACE);
    const ChannelConfig channel{42, 0.2, 1, 10, 0.05};
    World first({0, 1, 2, 3, 4, 5}, {}, &logger, channel);
    World permuted({5, 2, 4, 1, 3, 0}, {}, nullptr, channel);
    dispatchAll(first);
    dispatchAll(permuted);
    first.step(1000);
    permuted.step(1000);
    REQUIRE(first.snapshot()[3].state == MachineState::WAIT_PERMIT);
    const auto failed_before = first.snapshot()[3];
    first.fail(3);
    permuted.fail(3);
    first.step(211);
    permuted.step(211);
    const auto after_detection = first.snapshot();
    first.step(3789);
    permuted.step(3789);
    logger.close();
    checkSnapshots(first, permuted);
    CHECK(eventKeys(first) == eventKeys(permuted));

    for (const auto& machine : first.snapshot()) {
        if (machine.id == 3) {
            CHECK(machine.state == MachineState::FAULT);
            CHECK_FALSE(machine.connected);
            CHECK_FALSE(machine.on_route);
            CHECK_FALSE(machine.occupied_buffer.has_value());
            CHECK(machine.completed_loads == failed_before.completed_loads);
            CHECK(machine.completed_unloads == failed_before.completed_unloads);
        } else {
            CHECK(machine.completed_loads > after_detection.at(machine.id).completed_loads);
            CHECK(machine.completed_unloads > after_detection.at(machine.id).completed_unloads);
        }
    }
    std::map<ResourceId, std::set<MachineId>> inside;
    ZoneEventKind last_m3 = ZoneEventKind::LEAVE;
    for (const auto& event : first.events()) {
        if (event.machine == 3) {
            CHECK(event.tick <= 1000);
            last_m3 = event.kind;
        }
        if (event.kind == ZoneEventKind::ENTER) {
            CHECK(inside[event.resource].empty());
            CHECK(inside[event.resource].insert(event.machine).second);
        } else if (event.kind == ZoneEventKind::LEAVE) {
            CHECK(inside[event.resource].erase(event.machine) == 1);
        }
        CHECK(inside[event.resource].size() <= 1);
    }
    CHECK(last_m3 == ZoneEventKind::REQUEST);

    std::set<std::string> suspected;
    std::set<std::string> excluded;
    unsigned failures = 0;
    std::istringstream input(output.str());
    for (std::string line; std::getline(input, line);) {
        const auto row = nlohmann::json::parse(line);
        const auto event = row.at("ev").get<std::string>();
        if (event == "peer_suspected" || event == "peer_excluded") {
            CHECK(row.at("peer") == "M3");
            CHECK(row.at("tick").get<Tick>() > 1000);
            CHECK(row.at("tick").get<Tick>() <= 1211);
            CHECK(row.at("tick").get<Tick>() - row.at("last_seen").get<Tick>() == 201);
            auto& observers = event == "peer_suspected" ? suspected : excluded;
            CHECK(observers.insert(row.at("node").get<std::string>()).second);
        }
        if (event == "machine_failed") {
            ++failures;
            CHECK(row.at("node") == "M3");
            CHECK(row.at("tick") == 1000);
            CHECK(row.at("from") == "WAIT_PERMIT");
        }
        CHECK(event != "peer_returned");
        if (row.at("node") == "M3" && row.at("tick").get<Tick>() > 1000) {
            FAIL("A permanently failed machine produced a new event");
        }
    }
    CHECK(failures == 1);
    CHECK(suspected == std::set<std::string>{"M0", "M1", "M2", "M4", "M5"});
    CHECK(excluded == suspected);
}

TEST_CASE("a real failure inside a buffer removes occupancy without completing service")
{
    World world({0, 1, 2, 3});
    dispatchAll(world);
    world.step(4);
    const auto before = world.snapshot()[0];
    REQUIRE(before.occupied_buffer == ResourceId::LOADING_BUFFER);
    REQUIRE(before.load == 20);
    CHECK_THROWS_AS(world.fail(99), std::invalid_argument);
    world.fail(0);
    const auto removed = world.snapshot()[0];
    CHECK_FALSE(removed.occupied_buffer.has_value());
    CHECK_FALSE(removed.on_route);
    CHECK(removed.completed_loads == 0);
    CHECK(removed.load == before.load);
    REQUIRE(world.events().back().kind == ZoneEventKind::REMOVE);
    CHECK(world.events().back().tick == 4);
    const auto event_count = world.events().size();
    world.fail(0);
    CHECK(world.events().size() == event_count);
    world.step(400);
    for (const auto& machine : world.snapshot()) {
        if (machine.id == 0) {
            CHECK(machine.completed_loads == 0);
            CHECK(machine.completed_unloads == 0);
            CHECK(machine.load == before.load);
        } else {
            CHECK(machine.completed_loads > 0);
            CHECK(machine.completed_unloads > 0);
        }
    }
}
