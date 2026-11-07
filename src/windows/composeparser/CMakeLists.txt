# Copyright (C) Microsoft Corporation. All rights reserved.
add_library(compose_parser STATIC ComposeParser.cpp Protocol.cpp)
target_compile_features(compose_parser PUBLIC cxx_std_20)
target_link_libraries(compose_parser PRIVATE nlohmann_json::nlohmann_json ole32)
target_precompile_headers(compose_parser PRIVATE precomp.h)
target_compile_options(compose_parser PRIVATE /utf-8)
target_sources(compose_parser PUBLIC
    FILE_SET HEADERS
    BASE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}"
    FILES ComposeModel.hpp ComposeParser.hpp)
set_target_properties(compose_parser PROPERTIES
    FOLDER windows
    VERIFY_INTERFACE_HEADER_SETS ON)

add_custom_target(compose_contract_check ALL
    DEPENDS compose_parser_verify_interface_header_sets)
set_target_properties(compose_contract_check PROPERTIES FOLDER tests)

find_program(COMPOSE_GO_EXECUTABLE NAMES go REQUIRED)
if(TARGET_PLATFORM STREQUAL "arm64")
    set(compose_go_arch arm64)
else()
    set(compose_go_arch amd64)
endif()
set(compose_worker_path "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/$<CONFIG>/compose-worker.exe")
set(compose_go_environment
    "GOTOOLCHAIN=local" "CGO_ENABLED=0" "GOOS=windows" "GOARCH=${compose_go_arch}"
    "MS_GOTOOLCHAIN_TELEMETRY_ENABLED=0")
file(GLOB compose_go_sources CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/go/*.go")
add_custom_command(OUTPUT "${compose_worker_path}"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/$<CONFIG>"
    COMMAND "${CMAKE_COMMAND}" -E env ${compose_go_environment}
        "${COMPOSE_GO_EXECUTABLE}" build -mod=readonly -trimpath -o "${compose_worker_path}" .
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/go"
    DEPENDS ${compose_go_sources} go/go.mod go/go.sum
    VERBATIM)
add_custom_target(compose_worker DEPENDS "${compose_worker_path}")
add_dependencies(compose_parser compose_worker)
set_target_properties(compose_worker PROPERTIES FOLDER windows)
add_custom_target(compose_go_check
    COMMAND "${CMAKE_COMMAND}" -E env ${compose_go_environment}
        "${COMPOSE_GO_EXECUTABLE}" test -mod=readonly ./...
    COMMAND "${CMAKE_COMMAND}" -E env ${compose_go_environment}
        "${COMPOSE_GO_EXECUTABLE}" vet -mod=readonly ./...
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/go"
    VERBATIM)
