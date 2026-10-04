#include "fleetlink/machine.hpp"

#include <iostream>

int main(int argc, char**)
{
    if (argc != 1) {
        std::cerr << "Scenario execution is not implemented yet\n";
        return 2;
    }

    const fleetlink::Machine machine(1);
    std::cout << "fleetlink: machine " << machine.id()
              << " created (simulation is not implemented yet)\n";
}
