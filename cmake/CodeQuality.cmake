find_program(FLEETLINK_CLANG_FORMAT
    NAMES clang-format-18 clang-format
    NAMES_PER_DIR
    HINTS "${PROJECT_SOURCE_DIR}/.venv/bin"
)
find_program(FLEETLINK_CLANG_TIDY
    NAMES clang-tidy-18 clang-tidy
    NAMES_PER_DIR
    HINTS "${PROJECT_SOURCE_DIR}/.venv/bin"
)

file(GLOB_RECURSE fleetlink_format_files CONFIGURE_DEPENDS
    "${PROJECT_SOURCE_DIR}/include/*.hpp"
    "${PROJECT_SOURCE_DIR}/src/*.cpp"
    "${PROJECT_SOURCE_DIR}/tests/*.cpp"
    "${PROJECT_SOURCE_DIR}/bindings/*.cpp"
)

# Analyze only translation units present in this build configuration
set(fleetlink_tidy_files)
foreach(target fleetlink_core fleetlink_demo fleetlink_tests fleetlink_py)
    if(TARGET ${target})
        get_target_property(sources ${target} SOURCES)
        foreach(source IN LISTS sources)
            if(source MATCHES "\\.cpp$")
                get_filename_component(source "${source}" ABSOLUTE BASE_DIR "${PROJECT_SOURCE_DIR}")
                list(APPEND fleetlink_tidy_files "${source}")
            endif()
        endforeach()
    endif()
endforeach()
list(REMOVE_DUPLICATES fleetlink_tidy_files)

function(fleetlink_add_clang_target name tool)
    add_custom_target(${name}
        COMMAND ${CMAKE_COMMAND}
            "-DTOOL=${tool}"
            "-DMODE=${name}"
            "-DSOURCE_DIR=${PROJECT_SOURCE_DIR}"
            "-DBUILD_DIR=${PROJECT_BINARY_DIR}"
            "-DFILES=${ARGN}"
            -P "${PROJECT_SOURCE_DIR}/cmake/RunClangTool.cmake"
        WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}"
        VERBATIM
    )
endfunction()

fleetlink_add_clang_target(format "${FLEETLINK_CLANG_FORMAT}" ${fleetlink_format_files})
fleetlink_add_clang_target(format-check "${FLEETLINK_CLANG_FORMAT}" ${fleetlink_format_files})
fleetlink_add_clang_target(tidy "${FLEETLINK_CLANG_TIDY}" ${fleetlink_tidy_files})
