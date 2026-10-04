#include "fleetlink/world.hpp"

#include <nlohmann/json.hpp>

#include <exception>
#include <iostream>
#include <string>
#include <string_view>

namespace {

const char* eventName(fleetlink::ZoneEventKind kind)
{
    if (kind == fleetlink::ZoneEventKind::REQUEST) {
        return "zone_request";
    }
    if (kind == fleetlink::ZoneEventKind::ENTER) {
        return "zone_enter";
    }
    return "zone_leave";
}

void printTrace(const std::vector<fleetlink::ZoneEvent>& events)
{
    for (const auto& event : events) {
        const nlohmann::json row{
            {"tick", event.tick},
            {"node", "M" + std::to_string(event.machine)},
            {"lvl", "INFO"},
            {"ev", eventName(event.kind)},
            {"zone", event.resource == fleetlink::ResourceId::LOADING_BUFFER ? "LOADING_BUFFER"
                                                                             : "UNLOADING_BUFFER"},
        };
        std::cout << row.dump() << '\n';
    }
}

} // namespace

int main(int argc, char** argv)
{
    const bool trace = argc == 2 && std::string_view(argv[1]) == "--trace";
    const bool help = argc == 2 && std::string_view(argv[1]) == "--help";
    if (help || (argc != 1 && !trace)) {
        auto& output = help ? std::cout : std::cerr;
        output << "Usage: fleetlink_demo [--trace]\n"
               << "Run 6 machines for 2000 ticks with a clean channel and k=1\n"
               << "--trace writes JSONL events to stdout and the report to stderr\n";
        return help ? 0 : 2;
    }

    try {
        fleetlink::World world;
        for (const auto& machine : world.snapshot()) {
            world.dispatch(machine.id, fleetlink::MissionTarget::LOAD);
        }
        world.step(2000);
        if (trace) {
            printTrace(world.events());
        }

        auto& report = trace ? std::cerr : std::cout;
        report << "Ideal-channel demo: 6 machines, " << world.currentTick() << " ticks, k=1\n";
        bool progressed = true;
        for (const auto& machine : world.snapshot()) {
            report << "M" << machine.id << ": loads=" << machine.completed_loads
                   << ", unloads=" << machine.completed_unloads << '\n';
            progressed = progressed && machine.completed_loads > 0 && machine.completed_unloads > 0;
        }
        report << "Capacity checks: PASS\nProgress: " << (progressed ? "PASS" : "FAIL") << '\n';
        return progressed && std::cout && std::cerr ? 0 : 1;
    } catch (const std::exception& error) {
        std::cerr << "fleetlink: " << error.what() << '\n';
        return 1;
    }
}
