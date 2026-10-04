if(NOT EXISTS "${TOOL}")
    message(FATAL_ERROR
        "Install the tools from requirements-dev.txt and rerun CMake configuration"
    )
endif()

execute_process(
    COMMAND "${TOOL}" --version
    OUTPUT_VARIABLE version
    ERROR_VARIABLE version_error
    RESULT_VARIABLE result
)
if(NOT "${result}" STREQUAL "0")
    message(FATAL_ERROR "Cannot run ${TOOL}: ${version_error}")
endif()
if(NOT version MATCHES "version 18\\.1\\.8([^0-9.]|$)")
    message(FATAL_ERROR "Expected Clang tool version 18.1.8, got: ${version}")
endif()

if(MODE STREQUAL "format")
    set(arguments --style=file -i)
elseif(MODE STREQUAL "format-check")
    set(arguments --style=file --dry-run --Werror)
elseif(MODE STREQUAL "tidy")
    if(NOT EXISTS "${BUILD_DIR}/compile_commands.json")
        message(FATAL_ERROR "tidy requires compile_commands.json from a Makefiles or Ninja build")
    endif()
    set(arguments "-p=${BUILD_DIR}" "--config-file=${SOURCE_DIR}/.clang-tidy" --quiet)
else()
    message(FATAL_ERROR "Unknown tool mode: ${MODE}")
endif()

execute_process(
    COMMAND "${TOOL}" ${arguments} ${FILES}
    WORKING_DIRECTORY "${SOURCE_DIR}"
    RESULT_VARIABLE result
)
if(NOT "${result}" STREQUAL "0")
    message(FATAL_ERROR "${MODE} failed with exit code ${result}")
endif()
