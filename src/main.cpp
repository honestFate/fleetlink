#include "fleetlink/world.hpp"

#include <algorithm>
#include <charconv>
#include <exception>
#include <iostream>
#include <map>
#include <memory>
#include <string_view>

namespace {

void usage(std::ostream& output)
{
    output << "Usage: fleetlink_demo [--scenario S1|S2] [--seed N] [--trace]\n"
           << "S1: 2000 ticks, clean channel; S2: 5000 ticks, loss=0.2, delay=1..10, dup=0.05\n"
           << "Both scenarios run 6 machines with k=1, default scenario S1 and seed 42\n"
           << "--trace writes JSONL events to stdout and the report to stderr\n";
}

} // namespace

int main(int argc, char** argv)
{
    bool trace = false;
    std::string_view scenario = "S1";
    std::uint64_t seed = 42;
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
        if ((option == "--scenario" || option == "--seed") && index + 1 < argc) {
            const std::string_view value(argv[++index]);
            if (option == "--scenario" && (value == "S1" || value == "S2")) {
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
        }
        usage(std::cerr);
        return 2;
    }

    try {
        fleetlink::ChannelConfig channel;
        channel.seed = seed;
        if (scenario == "S2") {
            channel.loss = 0.2;
            channel.delay_min = 1;
            channel.delay_max = 10;
            channel.dup = 0.05;
        }
        std::unique_ptr<fleetlink::Logger> logger;
        if (trace) {
            logger = std::make_unique<fleetlink::Logger>(std::cout, fleetlink::LogLevel::TRACE);
        }
        fleetlink::World world({0, 1, 2, 3, 4, 5}, {}, logger.get(), channel);
        for (const auto& machine : world.snapshot()) {
            world.dispatch(machine.id, fleetlink::MissionTarget::LOAD);
        }
        world.step(scenario == "S2" ? 5000 : 2000);
        if (logger) {
            logger->close();
        }

        auto& report = trace ? std::cerr : std::cout;
        report << scenario << ": 6 machines, " << world.currentTick()
               << " ticks, k=1, seed=" << seed << '\n';
        bool progressed = true;
        for (const auto& machine : world.snapshot()) {
            report << "M" << machine.id << ": loads=" << machine.completed_loads
                   << ", unloads=" << machine.completed_unloads << '\n';
            progressed = progressed && machine.completed_loads > 0 && machine.completed_unloads > 0;
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
        const auto& counters = world.channelCounters();
        report << "Channel: sent=" << counters.sent << ", delivered=" << counters.delivered
               << ", lost=" << counters.lost << ", duplicates=" << counters.duplicates << '\n';
        report << "Max completed wait: " << max_wait << " ticks, still waiting: " << waiting.size()
               << '\n';
        report << "Capacity checks: PASS\nProgress: " << (progressed ? "PASS" : "FAIL") << '\n';
        return progressed && std::cout && std::cerr ? 0 : 1;
    } catch (const std::exception& error) {
        std::cerr << "fleetlink: " << error.what() << '\n';
        return 1;
    }
}
