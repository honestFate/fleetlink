#include <pybind11/pybind11.h>
#include <pybind11/stl.h> // std::vector / std::pair / std::string -> Python

#include "fleetlink/world.hpp"

namespace py = pybind11;

PYBIND11_MODULE(fleetlink, m)
{
    m.doc() = "fleetlink - coordination of a group of autonomous machines";

    py::class_<fleetlink::MachineSnapshot>(m, "MachineSnapshot")
        .def_readonly("id", &fleetlink::MachineSnapshot::id)
        .def_readonly("state", &fleetlink::MachineSnapshot::state)
        .def_readonly("pos", &fleetlink::MachineSnapshot::pos)
        .def_readonly("load", &fleetlink::MachineSnapshot::load)
        .def("__repr__", [](const fleetlink::MachineSnapshot &s)
             { return "<Machine " + std::to_string(s.id) + " " + s.state + ">"; });

    py::class_<fleetlink::World>(m, "World")
        .def(py::init<unsigned, int, double, std::pair<int, int>, double, int>(),
             py::arg("seed") = 42,
             py::arg("machines") = 6,
             py::arg("loss") = 0.0,
             py::arg("delay") = std::make_pair(0, 0),
             py::arg("dup") = 0.0,
             py::arg("zone_capacity") = 1)
        // GIL отпускаем на длинном прогоне: фоновый поток журнала не должен ждать
        // интерпретатор
        .def("step", &fleetlink::World::step, py::arg("ticks") = 1,
             py::call_guard<py::gil_scoped_release>())
        .def("snapshot", &fleetlink::World::snapshot)
        .def("dispatch", &fleetlink::World::dispatch,
             py::arg("machine_id"), py::arg("target"))
        .def("halt_all", &fleetlink::World::haltAll)
        .def("resume", &fleetlink::World::resume)
        .def("kill", &fleetlink::World::kill, py::arg("machine_id"))
        .def("set_arm_online", &fleetlink::World::setArmOnline, py::arg("online"))
         .def("events", &fleetlink::World::events, py::arg("since_tick") = 0);
}
