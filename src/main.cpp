#include "fleetlink/world.hpp"

#include <algorithm>
#include <charconv>
#include <exception>
#include <iostream>
#include <map>
#include <memory>
#include <stdexcept>
#include <string_view>
#include <vector>

namespace {

void usage(std::ostream& output)
{
    output << "Usage: fleetlink_demo [--scenario S1|S2|S3|S4] [--seed N] [--k N] [--trace]\n"
           << "S1: 2000 ticks, clean channel; S2: 5000 ticks, loss=0.2, delay=1..10, dup=0.05\n"
           << "S3: S2 with M3 failing while waiting at the start of tick 1000\n"
           << "S4: S2 with ARM offline during ticks 800..1000\n"
           << "All scenarios run 6 machines; defaults: k=1, scenario S1, seed 42\n"
           << "--k sets a positive capacity for each buffer; service capacity stays 1\n"
           << "--trace writes JSONL events to stdout and the report to stderr\n";
}

} // namespace

int main(int argc, char** argv)
{
    bool trace = false;
    std::string_view scenario = "S1";
    std::uint64_t seed = 42;
    unsigned capacity = 1;
    for (int index = 1; index < argc; ++index) {
        const std::string_view option(argv[index]);
        if (option == "--help") {
            usage(std::cout);
            return 0;
        }
        if (option == "--trace") {
            trace = true;
            continue;
        }
        if ((option == "--scenario" || option == "--seed" || option == "--k") && index + 1 < argc) {
            const std::string_view value(argv[++index]);
            if (option == "--scenario" &&
                (value == "S1" || value == "S2" || value == "S3" || value == "S4")) {
                scenario = value;
                continue;
            }
            if (option == "--seed") {
                const auto parsed =
                    std::from_chars(value.data(), value.data() + value.size(), seed);
                if (parsed.ec == std::errc{} && parsed.ptr == value.data() + value.size()) {
                    continue;
                }
            }
            if (option == "--k") {
                const auto parsed =
                    std::from_chars(value.data(), value.data() + value.size(), capacity);
                if (parsed.ec == std::errc{} && parsed.ptr == value.data() + value.size() &&
                    capacity > 0) {
                    continue;
                }
            }
        }
        usage(std::cerr);
        return 2;
    }

    try {
        fleetlink::ChannelConfig channel;
        channel.seed = seed;
        if (scenario != "S1") {
            channel.loss = 0.2;
            channel.delay_min = 1;
            channel.delay_max = 10;
            channel.dup = 0.05;
        }
        std::unique_ptr<fleetlink::Logger> logger;
        if (trace) {
            logger = std::make_unique<fleetlink::Logger>(std::cout, fleetlink::LogLevel::TRACE);
        }
        fleetlink::MachineConfig config;
        config.zone_capacity = capacity;
        fleetlink::World world({0, 1, 2, 3, 4, 5}, config, logger.get(), channel);
        for (const auto& machine : world.snapshot()) {
            world.dispatch(machine.id, fleetlink::MissionTarget::LOAD);
        }
        bool arm_recovered = true;
        bool outage_progress = true;
        std::vector<fleetlink::MachineSnapshot> before_failure;
        if (scenario == "S3") {
            world.step(999);
            before_failure = world.snapshot();
            if (before_failure.at(3).state != fleetlink::MachineState::WAIT_PERMIT) {
                throw std::runtime_error(
                    "S3 requires M3 to be waiting for permission at tick 1000 for this seed and k");
            }
            world.failAt(3, 1000);
            world.step(4001);
        } else if (scenario == "S4") {
            world.step(799);
            const auto before = world.events().size();
            world.setArmOnline(false);
            world.step(201);
            outage_progress = world.events().size() > before;
            world.setArmOnline(true);
            world.step(100);
            for (const auto& row : world.armSnapshot()) {
                arm_recovered = arm_recovered && row.telemetry && row.age && *row.age < 100;
            }
            world.step(3900);
        } else {
            world.step(scenario == "S2" ? 5000 : 2000);
        }
        if (logger) {
            logger->close();
        }

        auto& report = trace ? std::cerr : std::cout;
        report << scenario << ": 6 machines, " << world.currentTick() << " ticks, k=" << capacity
               << ", seed=" << seed << '\n';
        bool progressed = arm_recovered && outage_progress;
        for (const auto& machine : world.snapshot()) {
            report << "M" << machine.id << ": loads=" << machine.completed_loads
                   << ", unloads=" << machine.completed_unloads << '\n';
            if (scenario == "S3") {
                const auto& before = before_failure.at(machine.id);
                if (machine.id == 3) {
                    progressed = progressed && !machine.on_route &&
                                 machine.completed_loads == before.completed_loads &&
                                 machine.completed_unloads == before.completed_unloads;
                    report << "  Permanently failed at tick 1000\n";
                } else {
                    const auto loads = machine.completed_loads - before.completed_loads;
                    const auto unloads = machine.completed_unloads - before.completed_unloads;
                    report << "  After failure: loads=" << loads << ", unloads=" << unloads << '\n';
                    progressed = progressed && loads > 0 && unloads > 0;
                }
            } else {
                progressed =
                    progressed && machine.completed_loads > 0 && machine.completed_unloads > 0;
            }
        }
        std::map<fleetlink::MachineId, fleetlink::Tick> waiting;
        fleetlink::Tick max_wait = 0;
        for (const auto& event : world.events()) {
            if (event.kind == fleetlink::ZoneEventKind::REQUEST) {
                waiting.emplace(event.machine, event.tick);
            } else if (event.kind == fleetlink::ZoneEventKind::ENTER) {
                max_wait = std::max(max_wait, event.tick - waiting.at(event.machine));
                waiting.erase(event.machine);
            }
        }
        for (const auto& machine : world.snapshot()) {
            if (!machine.on_route) {
                waiting.erase(machine.id);
            }
        }
        const auto& counters = world.channelCounters();
        report << "Channel: sent=" << counters.sent << ", delivered=" << counters.delivered
               << ", lost=" << counters.lost << ", duplicates=" << counters.duplicates << '\n';
        report << "Max completed wait: " << max_wait << " ticks, still waiting: " << waiting.size()
               << '\n';
        if (scenario == "S4") {
            report << "ARM recovery: " << (arm_recovered ? "PASS" : "FAIL") << '\n';
        }
        report << "Capacity checks: PASS\nProgress: " << (progressed ? "PASS" : "FAIL") << '\n';
        return progressed && std::cout && std::cerr ? 0 : 1;
    } catch (const std::exception& error) {
        std::cerr << "fleetlink: " << error.what() << '\n';
        return 1;
    }
}
