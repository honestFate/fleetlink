add_library(fleetlink_json INTERFACE)
add_library(nlohmann_json::nlohmann_json ALIAS fleetlink_json)
target_include_directories(fleetlink_json SYSTEM INTERFACE
    "${CMAKE_CURRENT_SOURCE_DIR}/json/include"
)

if(FLEETLINK_BUILD_TESTS)
    add_library(fleetlink_doctest INTERFACE)
    add_library(doctest::doctest ALIAS fleetlink_doctest)
    target_include_directories(fleetlink_doctest SYSTEM INTERFACE
        "${CMAKE_CURRENT_SOURCE_DIR}/doctest/include"
    )
endif()
