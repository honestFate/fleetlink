#include <pybind11/pybind11.h>
#include <pybind11/stl.h>

#include "fleetlink/world.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <fstream>
#include <memory>
#include <mutex>
#include <numeric>
#include <sstream>
#include <stdexcept>
#include <utility>

namespace py = pybind11;
using namespace fleetlink;

namespace {

std::vector<MachineId> membersFor(unsigned machines)
{
    if (machines < 4 || machines > 10) {
        throw std::invalid_argument("World requires 4 to 10 machines");
    }
    std::vector<MachineId> members(machines);
    std::iota(members.begin(), members.end(), MachineId{0});
    return members;
}

MachineConfig capacityConfig(unsigned capacity)
{
    MachineConfig config;
    config.zone_capacity = capacity;
    return config;
}

using JournalRow = std::map<std::string, LogValue>;

// Own the stream, logger and model together, with destruction in reverse order
class PythonWorld {
public:
    PythonWorld(std::vector<MachineId> members, MachineConfig config, ChannelConfig channel,
                std::optional<std::string> log_path, LogLevel level)
        : log_path_(std::move(log_path))
    {
        std::ostream* output = &memory_;
        if (log_path_) {
            file_.open(*log_path_, std::ios::out | std::ios::trunc);
            if (!file_) {
                throw std::runtime_error("Cannot open log file: " + *log_path_);
            }
            output = &file_;
        }
        logger_ = std::make_unique<Logger>(*output, level);
        world_ = std::make_unique<World>(std::move(members), config, logger_.get(), channel);
    }

    // Bindings release the GIL before acquiring this mutex, including readers
    template <typename Action> auto access(Action action)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (closed_) {
            throw std::logic_error("World is closed");
        }
        return action(*world_);
    }

    void close()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (closed_) {
            return;
        }
        closed_ = true;
        logger_->close();
        if (log_path_) {
            file_.close();
            if (!file_) {
                throw std::runtime_error("Failed to close log file: " + *log_path_);
            }
        }
    }

    std::vector<JournalRow> events(Tick since_tick)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        logger_->flush();
        if (log_path_) {
            std::ifstream input(*log_path_);
            if (!input) {
                throw std::runtime_error("Cannot read log file: " + *log_path_);
            }
            return readEvents(input, since_tick);
        }
        std::istringstream input(memory_.str());
        return readEvents(input, since_tick);
    }

private:
    static std::vector<JournalRow> readEvents(std::istream& input, Tick since_tick)
    {
        std::vector<JournalRow> result;
        for (std::string line; std::getline(input, line);) {
            const auto json = nlohmann::json::parse(line);
            if (json.at("tick").get<Tick>() < since_tick) {
                continue;
            }
            JournalRow row;
            for (const auto& [key, value] : json.items()) {
                if (value.is_string()) {
                    row.emplace(key, value.get<std::string>());
                } else if (value.is_boolean()) {
                    row.emplace(key, value.get<bool>());
                } else if (value.is_number_unsigned()) {
                    row.emplace(key, value.get<std::uint64_t>());
                } else {
                    row.emplace(key, value.get<std::int64_t>());
                }
            }
            result.push_back(std::move(row));
        }
        if (input.bad()) {
            throw std::runtime_error("Failed to read JSONL log");
        }
        return result;
    }

    std::mutex mutex_;
    std::ostringstream memory_;
    std::ofstream file_;
    std::optional<std::string> log_path_;
    std::unique_ptr<Logger> logger_;
    std::unique_ptr<World> world_;
    bool closed_ = false;
};

void bindValues(py::module_& m)
{
    py::enum_<MachineClass>(m, "MachineClass")
        .value("HAULER", MachineClass::HAULER)
        .value("LOADER", MachineClass::LOADER);
    py::enum_<MachineState>(m, "MachineState")
        .value("IDLE", MachineState::IDLE)
        .value("EN_ROUTE", MachineState::EN_ROUTE)
        .value("WAIT_PERMIT", MachineState::WAIT_PERMIT)
        .value("IN_ZONE", MachineState::IN_ZONE)
        .value("LOADING", MachineState::LOADING)
        .value("UNLOADING", MachineState::UNLOADING)
        .value("FAULT", MachineState::FAULT)
        .value("OFFLINE", MachineState::OFFLINE);
    py::enum_<ResourceId>(m, "ResourceId")
        .value("LOADING_BUFFER", ResourceId::LOADING_BUFFER)
        .value("UNLOADING_BUFFER", ResourceId::UNLOADING_BUFFER)
        .value("LOADING_SERVICE", ResourceId::LOADING_SERVICE)
        .value("UNLOADING_SERVICE", ResourceId::UNLOADING_SERVICE);
    py::enum_<MissionTarget>(m, "MissionTarget")
        .value("LOAD", MissionTarget::LOAD)
        .value("DUMP", MissionTarget::DUMP);
    py::enum_<ZoneEventKind>(m, "ZoneEventKind")
        .value("REQUEST", ZoneEventKind::REQUEST)
        .value("ENTER", ZoneEventKind::ENTER)
        .value("LEAVE", ZoneEventKind::LEAVE)
        .value("REMOVE", ZoneEventKind::REMOVE);
    py::enum_<LogLevel>(m, "LogLevel")
        .value("TRACE", LogLevel::TRACE)
        .value("DEBUG", LogLevel::DEBUG)
        .value("INFO", LogLevel::INFO)
        .value("WARN", LogLevel::WARN)
        .value("ERROR", LogLevel::ERROR);
    m.attr("ARM_NODE_ID") = py::int_(ARM_NODE_ID);

    py::class_<HeartbeatConfig>(m, "HeartbeatConfig")
        .def(py::init<>())
        .def_readwrite("interval", &HeartbeatConfig::interval)
        .def_readwrite("timeout", &HeartbeatConfig::timeout);
    py::class_<MachineConfig>(m, "MachineConfig")
        .def(py::init<>())
        .def_readwrite("road_length", &MachineConfig::road_length)
        .def_readwrite("speed", &MachineConfig::speed)
        .def_readwrite("loading_rate", &MachineConfig::loading_rate)
        .def_readwrite("unloading_rate", &MachineConfig::unloading_rate)
        .def_readwrite("initial_position", &MachineConfig::initial_position)
        .def_readwrite("initial_load", &MachineConfig::initial_load)
        .def_readwrite("retry_interval", &MachineConfig::retry_interval)
        .def_readwrite("heartbeat", &MachineConfig::heartbeat)
        .def_readwrite("telemetry_interval", &MachineConfig::telemetry_interval)
        .def_readwrite("zone_capacity", &MachineConfig::zone_capacity);
    py::class_<ChannelConfig>(m, "ChannelConfig")
        .def(py::init<>())
        .def_readwrite("seed", &ChannelConfig::seed)
        .def_readwrite("loss", &ChannelConfig::loss)
        .def_readwrite("delay_min", &ChannelConfig::delay_min)
        .def_readwrite("delay_max", &ChannelConfig::delay_max)
        .def_readwrite("dup", &ChannelConfig::dup);
    py::class_<ChannelCounters>(m, "ChannelCounters")
        .def_readonly("sent", &ChannelCounters::sent)
        .def_readonly("delivered", &ChannelCounters::delivered)
        .def_readonly("lost", &ChannelCounters::lost)
        .def_readonly("duplicates", &ChannelCounters::duplicates);
    py::class_<MachineSnapshot>(m, "MachineSnapshot")
        .def_readonly("id", &MachineSnapshot::id)
        .def_readonly("machine_class", &MachineSnapshot::machine_class)
        .def_readonly("state", &MachineSnapshot::state)
        .def_readonly("position", &MachineSnapshot::position)
        .def_readonly("load", &MachineSnapshot::load)
        .def_readonly("connected", &MachineSnapshot::connected)
        .def_readonly("on_route", &MachineSnapshot::on_route)
        .def_readonly("occupied_buffer", &MachineSnapshot::occupied_buffer)
        .def_readonly("completed_loads", &MachineSnapshot::completed_loads)
        .def_readonly("completed_unloads", &MachineSnapshot::completed_unloads)
        .def_readonly("halted", &MachineSnapshot::halted);
    py::class_<Telemetry>(m, "Telemetry")
        .def_readonly("sequence", &Telemetry::sequence)
        .def_readonly("position", &Telemetry::position)
        .def_readonly("state", &Telemetry::state)
        .def_readonly("load", &Telemetry::load)
        .def_readonly("connected", &Telemetry::connected)
        .def_readonly("channel", &Telemetry::channel)
        .def_readonly("zone_wait_count", &Telemetry::zone_wait_count)
        .def_readonly("zone_wait_ticks", &Telemetry::zone_wait_ticks)
        .def_readonly("halted", &Telemetry::halted);
    py::class_<ArmMachineSnapshot>(m, "ArmMachineSnapshot")
        .def_readonly("id", &ArmMachineSnapshot::id)
        .def_readonly("telemetry", &ArmMachineSnapshot::telemetry)
        .def_readonly("age", &ArmMachineSnapshot::age);
    py::class_<ZoneEvent>(m, "ZoneEvent")
        .def_readonly("tick", &ZoneEvent::tick)
        .def_readonly("machine", &ZoneEvent::machine)
        .def_readonly("kind", &ZoneEvent::kind)
        .def_readonly("resource", &ZoneEvent::resource);
}

} // namespace

PYBIND11_MODULE(fleetlink, m)
{
    m.doc() = "fleetlink - coordination of a group of autonomous machines";
    bindValues(m);
    py::class_<Machine>(m, "Machine")
        .def(py::init<MachineId, MachineClass>(), py::arg("id"),
             py::arg("machine_class") = MachineClass::HAULER)
        .def_property_readonly("id", &Machine::id)
        .def_property_readonly("machine_class", &Machine::machineClass)
        .def_property_readonly("state", &Machine::state)
        .def_property_readonly("position", &Machine::position)
        .def_property_readonly("load", &Machine::load)
        .def_property_readonly("connected", &Machine::connected);

    py::class_<PythonWorld>(m, "World")
        .def(py::init([](std::uint64_t seed, unsigned machines, double loss,
                         std::pair<Tick, Tick> delay, double dup, unsigned capacity,
                         std::optional<std::string> log_path, LogLevel level) {
                 return std::make_unique<PythonWorld>(
                     membersFor(machines), capacityConfig(capacity),
                     ChannelConfig{seed, loss, delay.first, delay.second, dup}, std::move(log_path),
                     level);
             }),
             py::arg("seed") = 42, py::arg("machines") = 6, py::arg("loss") = 0,
             py::arg("delay") = std::make_pair(Tick{0}, Tick{0}), py::arg("dup") = 0,
             py::arg("zone_capacity") = 1, py::kw_only(), py::arg("log_path") = py::none(),
             py::arg("log_level") = LogLevel::INFO)
        .def(py::init<std::vector<MachineId>, MachineConfig, ChannelConfig,
                      std::optional<std::string>, LogLevel>(),
             py::arg("members"), py::arg("config") = MachineConfig{},
             py::arg("channel") = ChannelConfig{}, py::kw_only(), py::arg("log_path") = py::none(),
             py::arg("log_level") = LogLevel::INFO)
        .def(
            "step",
            [](PythonWorld& self, Tick ticks) {
                self.access([ticks](World& world) { world.step(ticks); });
            },
            py::arg("ticks") = 1, py::call_guard<py::gil_scoped_release>())
        .def(
            "snapshot",
            [](PythonWorld& self) {
                return self.access([](const World& world) { return world.snapshot(); });
            },
            py::call_guard<py::gil_scoped_release>())
        .def(
            "arm_snapshot",
            [](PythonWorld& self) {
                return self.access([](const World& world) { return world.armSnapshot(); });
            },
            py::call_guard<py::gil_scoped_release>())
        .def(
            "channel_counters",
            [](PythonWorld& self) {
                return self.access([](const World& world) { return world.channelCounters(); });
            },
            py::call_guard<py::gil_scoped_release>())
        .def_property_readonly("current_tick",
                               [](PythonWorld& self) {
                                   py::gil_scoped_release release;
                                   return self.access(
                                       [](const World& world) { return world.currentTick(); });
                               })
        .def_property_readonly("arm_online",
                               [](PythonWorld& self) {
                                   py::gil_scoped_release release;
                                   return self.access(
                                       [](const World& world) { return world.armOnline(); });
                               })
        .def(
            "dispatch",
            [](PythonWorld& self, MachineId id, MissionTarget target) {
                self.access([id, target](World& world) { world.dispatch(id, target); });
            },
            py::arg("machine_id"), py::arg("target"), py::call_guard<py::gil_scoped_release>())
        .def(
            "dispatch",
            [](PythonWorld& self, MachineId id, const std::string& target) {
                if (target != "load" && target != "dump") {
                    throw std::invalid_argument("Mission target must be load or dump");
                }
                self.access([id, &target](World& world) {
                    world.dispatch(id,
                                   target == "load" ? MissionTarget::LOAD : MissionTarget::DUMP);
                });
            },
            py::arg("machine_id"), py::arg("target"), py::call_guard<py::gil_scoped_release>())
        .def(
            "halt_all",
            [](PythonWorld& self, NodeId initiator) {
                self.access([initiator](World& world) { world.haltAll(initiator); });
            },
            py::arg("initiator") = ARM_NODE_ID, py::call_guard<py::gil_scoped_release>())
        .def(
            "resume",
            [](PythonWorld& self, NodeId initiator) {
                self.access([initiator](World& world) { world.resume(initiator); });
            },
            py::arg("initiator") = ARM_NODE_ID, py::call_guard<py::gil_scoped_release>())
        .def(
            "kill",
            [](PythonWorld& self, MachineId id) {
                self.access([id](World& world) { world.fail(id); });
            },
            py::arg("machine_id"), py::call_guard<py::gil_scoped_release>())
        .def(
            "kill_at",
            [](PythonWorld& self, MachineId id, Tick tick) {
                self.access([id, tick](World& world) { world.failAt(id, tick); });
            },
            py::arg("machine_id"), py::arg("tick"), py::call_guard<py::gil_scoped_release>())
        .def(
            "set_arm_online",
            [](PythonWorld& self, bool online) {
                self.access([online](World& world) { world.setArmOnline(online); });
            },
            py::arg("online"), py::call_guard<py::gil_scoped_release>())
        .def("events", &PythonWorld::events, py::arg("since_tick") = 0,
             py::call_guard<py::gil_scoped_release>())
        .def(
            "zone_events",
            [](PythonWorld& self, Tick since_tick) {
                return self.access([since_tick](const World& world) {
                    std::vector<ZoneEvent> result;
                    for (const auto& event : world.events()) {
                        if (event.tick >= since_tick) {
                            result.push_back(event);
                        }
                    }
                    return result;
                });
            },
            py::arg("since_tick") = 0, py::call_guard<py::gil_scoped_release>())
        .def("close", &PythonWorld::close, py::call_guard<py::gil_scoped_release>())
        .def(
            "__enter__",
            [](PythonWorld& self) -> PythonWorld& {
                self.access([](const World&) {});
                return self;
            },
            py::return_value_policy::reference_internal, py::call_guard<py::gil_scoped_release>())
        .def("__exit__",
             [](PythonWorld& self, const py::object&, const py::object&, const py::object&) {
                 py::gil_scoped_release release;
                 self.close();
                 return false;
             });
}
