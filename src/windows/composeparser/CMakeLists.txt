# Copyright (C) Microsoft Corporation. All rights reserved.
add_library(compose_parser STATIC ComposeParser.cpp Protocol.cpp)
target_compile_features(compose_parser PUBLIC cxx_std_20)
target_link_libraries(compose_parser PRIVATE nlohmann_json::nlohmann_json)
target_precompile_headers(compose_parser PRIVATE precomp.h)
target_compile_options(compose_parser PRIVATE /utf-8)
target_include_directories(compose_parser PRIVATE "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/$<CONFIG>")
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
set(COMPOSE_CGO_COMPILER "" CACHE FILEPATH "C compiler for the selected Windows Go architecture")
set(COMPOSE_CGO_FLAGS "" CACHE STRING "Target/sysroot flags for cgo compilation and linking")
if(NOT EXISTS "${COMPOSE_CGO_COMPILER}")
    message(FATAL_ERROR "Set COMPOSE_CGO_COMPILER to the verified MSYS2 Clang executable.")
endif()
if(TARGET_PLATFORM STREQUAL "arm64")
    set(compose_go_arch arm64)
else()
    set(compose_go_arch amd64)
endif()
set(compose_library_path "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/$<CONFIG>/compose-go.dll")
set(compose_header_path "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/$<CONFIG>/compose-go.h")
set(compose_go_environment
    "GOTOOLCHAIN=local" "CGO_ENABLED=1" "GOOS=windows" "GOARCH=${compose_go_arch}"
    "MS_GOTOOLCHAIN_TELEMETRY_ENABLED=0" "CC=\"${COMPOSE_CGO_COMPILER}\" ${COMPOSE_CGO_FLAGS}")
file(GLOB compose_go_sources CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/go/*.go")
add_custom_command(OUTPUT "${compose_library_path}" "${compose_header_path}"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/$<CONFIG>"
    COMMAND "${CMAKE_COMMAND}" -E env ${compose_go_environment}
        "${COMPOSE_GO_EXECUTABLE}" build -mod=readonly -trimpath -buildmode=c-shared -o "${compose_library_path}" .
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/go"
    DEPENDS ${compose_go_sources} go/go.mod go/go.sum
    VERBATIM)
add_custom_target(compose_go_dll DEPENDS "${compose_library_path}" "${compose_header_path}")
add_dependencies(compose_parser compose_go_dll)
set_target_properties(compose_go_dll PROPERTIES FOLDER windows)
add_custom_target(compose_go_check
    COMMAND "${CMAKE_COMMAND}" -E env ${compose_go_environment}
        "${COMPOSE_GO_EXECUTABLE}" test -mod=readonly ./...
    COMMAND "${CMAKE_COMMAND}" -E env ${compose_go_environment}
        "${COMPOSE_GO_EXECUTABLE}" vet -mod=readonly ./...
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/go"
    VERBATIM)
