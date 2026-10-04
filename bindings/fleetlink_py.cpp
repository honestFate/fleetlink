#include <pybind11/pybind11.h>

#include "fleetlink/machine.hpp"

namespace py = pybind11;

PYBIND11_MODULE(fleetlink, m)
{
    m.doc() = "fleetlink - coordination of a group of autonomous machines";

    py::enum_<fleetlink::MachineClass>(m, "MachineClass")
        .value("HAULER", fleetlink::MachineClass::HAULER)
        .value("LOADER", fleetlink::MachineClass::LOADER);

    py::enum_<fleetlink::MachineState>(m, "MachineState")
        .value("IDLE", fleetlink::MachineState::IDLE)
        .value("EN_ROUTE", fleetlink::MachineState::EN_ROUTE)
        .value("WAIT_PERMIT", fleetlink::MachineState::WAIT_PERMIT)
        .value("IN_ZONE", fleetlink::MachineState::IN_ZONE)
        .value("LOADING", fleetlink::MachineState::LOADING)
        .value("UNLOADING", fleetlink::MachineState::UNLOADING)
        .value("FAULT", fleetlink::MachineState::FAULT)
        .value("OFFLINE", fleetlink::MachineState::OFFLINE);

    py::class_<fleetlink::Machine>(m, "Machine")
        .def(py::init<fleetlink::MachineId, fleetlink::MachineClass>(),
             py::arg("id"), py::arg("machine_class") = fleetlink::MachineClass::HAULER)
        .def_property_readonly("id", &fleetlink::Machine::id)
        .def_property_readonly("machine_class", &fleetlink::Machine::machineClass)
        .def_property_readonly("state", &fleetlink::Machine::state)
        .def_property_readonly("position", &fleetlink::Machine::position)
        .def_property_readonly("load", &fleetlink::Machine::load)
        .def_property_readonly("connected", &fleetlink::Machine::connected);
}
