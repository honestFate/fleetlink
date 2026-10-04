cmake_minimum_required(VERSION 3.20)
project(fleetlink LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17) # 20/23 — если хотите
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

option(FLEETLINK_BUILD_PYTHON "Build the Python module" ON)
option(FLEETLINK_BUILD_TESTS "Build tests" ON)
set(FLEETLINK_SANITIZER "none" CACHE STRING "none | address | thread | undefined")

# --- ядро ---------------------------------------------------------------------
add_library(fleetlink_core
    src/machine.cpp
    src/transport.cpp
    src/logger.cpp
    src/world.cpp
)
target_include_directories(fleetlink_core PUBLIC include)
find_package(Threads REQUIRED)
target_link_libraries(fleetlink_core PUBLIC Threads::Threads)

if(NOT FLEETLINK_SANITIZER STREQUAL "none")
    target_compile_options(fleetlink_core PUBLIC
        -fsanitize=${FLEETLINK_SANITIZER} -fno-omit-frame-pointer -g)
    target_link_options(fleetlink_core PUBLIC
        -fsanitize=${FLEETLINK_SANITIZER})
endif()

# --- нативное демо: именно его гоняем под санитайзерами -----------------------
add_executable(fleetlink_demo src/main.cpp)
target_link_libraries(fleetlink_demo PRIVATE fleetlink_core)

# --- нативные тесты -----------------------------------------------------------
if(FLEETLINK_BUILD_TESTS)
    enable_testing()
    add_executable(fleetlink_tests
        tests/main.cpp
        tests/test_transport.cpp
        tests/test_protocol.cpp
        tests/test_world.cpp
    )
    target_link_libraries(fleetlink_tests PRIVATE fleetlink_core)
    add_test(NAME fleetlink_tests COMMAND fleetlink_tests)
endif()

# --- Python-модуль ------------------------------------------------------------
if(FLEETLINK_BUILD_PYTHON)
    find_package(Python COMPONENTS Interpreter Development.Module REQUIRED)
    find_package(pybind11 CONFIG REQUIRED) # pip install pybind11
    # Имя цели НЕ должно совпадать с именем модуля — иначе коллизия имён в CMake.
    pybind11_add_module(fleetlink_py bindings/fleetlink_py.cpp)
    set_target_properties(fleetlink_py PROPERTIES OUTPUT_NAME fleetlink)
    target_link_libraries(fleetlink_py PRIVATE fleetlink_core)
endif()
