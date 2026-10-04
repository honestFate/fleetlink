cmake_minimum_required(VERSION 3.20)
project(fleetlink LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17) # 20/23 — если хотите
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

option(FLEETLINK_BUILD_PYTHON "Build the Python module" OFF)
option(FLEETLINK_BUILD_TESTS "Build tests" ON)
set(FLEETLINK_SANITIZER "none" CACHE STRING "none | address | thread | undefined")
set_property(CACHE FLEETLINK_SANITIZER PROPERTY STRINGS none address thread undefined)
if(NOT FLEETLINK_SANITIZER MATCHES "^(none|address|thread|undefined)$")
    message(FATAL_ERROR "Unsupported FLEETLINK_SANITIZER: ${FLEETLINK_SANITIZER}")
endif()

add_subdirectory(third_party)

# --- ядро ---------------------------------------------------------------------
add_library(fleetlink_core SHARED
    src/machine.cpp
    src/messages.cpp
    src/transport.cpp
    src/logger.cpp
    src/world.cpp
)
target_include_directories(fleetlink_core PUBLIC include)
find_package(Threads REQUIRED)
target_link_libraries(fleetlink_core
    PUBLIC Threads::Threads
    PRIVATE nlohmann_json::nlohmann_json
)

if(NOT FLEETLINK_SANITIZER STREQUAL "none")
    if(NOT CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        message(FATAL_ERROR "Sanitizer builds require GCC or Clang")
    endif()
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
        tests/test_messages.cpp
        tests/test_transport.cpp
        tests/test_protocol.cpp
        tests/test_world.cpp
    )
    target_link_libraries(fleetlink_tests PRIVATE fleetlink_core doctest::doctest)
    add_test(NAME fleetlink_tests COMMAND fleetlink_tests)
endif()

# --- Python-модуль ------------------------------------------------------------
if(FLEETLINK_BUILD_PYTHON)
    find_package(Python 3.8 COMPONENTS Interpreter Development.Module REQUIRED)
    set(PYBIND11_FINDPYTHON ON)
    add_subdirectory(third_party/pybind11 EXCLUDE_FROM_ALL)
    # Имя цели НЕ должно совпадать с именем модуля — иначе коллизия имён в CMake.
    pybind11_add_module(fleetlink_py bindings/fleetlink_py.cpp)
    set_target_properties(fleetlink_py PROPERTIES OUTPUT_NAME fleetlink)
    target_link_libraries(fleetlink_py PRIVATE fleetlink_core)

    if(FLEETLINK_BUILD_TESTS)
        add_test(NAME fleetlink_python_import
            COMMAND ${CMAKE_COMMAND} -E env
                "PYTHONPATH=$<TARGET_FILE_DIR:fleetlink_py>"
                "${Python_EXECUTABLE}" -c "import fleetlink; fleetlink.Machine(1)"
        )
    endif()
endif()

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    foreach(target fleetlink_core fleetlink_demo fleetlink_tests fleetlink_py)
        if(TARGET ${target})
            target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
        endif()
    endforeach()
endif()
