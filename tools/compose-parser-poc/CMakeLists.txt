# Copyright (C) Microsoft Corporation. All rights reserved.
cmake_minimum_required(VERSION 3.25)
project(ComposeParserPoc LANGUAGES CXX)

if(NOT WIN32 OR NOT MSVC)
    message(FATAL_ERROR "The focused prototype requires Windows and MSVC.")
endif()

include(FetchContent)
include(CTest)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")

FetchContent_Declare(nlohmannjson
    URL https://github.com/nlohmann/json/releases/download/v3.12.0/json.tar.xz
    URL_HASH SHA256=42f6e95cad6ec532fd372391373363b62a14af6d771056dbfc86160e6dfff7aa)
FetchContent_MakeAvailable(nlohmannjson)

set(WIL_INCLUDE_DIR "${CMAKE_BINARY_DIR}/packages/Microsoft.Windows.ImplementationLibrary/include"
    CACHE PATH "WIL 1.0.251108.1 include directory")
if(NOT EXISTS "${WIL_INCLUDE_DIR}/wil/resource.h")
    message(FATAL_ERROR
        "Missing WIL 1.0.251108.1. Run: nuget install Microsoft.Windows.ImplementationLibrary -Version 1.0.251108.1 -OutputDirectory \"${CMAKE_BINARY_DIR}/packages\" -ExcludeVersion -NonInteractive")
endif()
add_library(compose_poc_wil INTERFACE)
target_include_directories(compose_poc_wil INTERFACE "${WIL_INCLUDE_DIR}")

add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../../src/windows/composeparser" composeparser)
add_executable(compose-parser-poc main.cpp)
target_link_libraries(compose-parser-poc PRIVATE compose_parser)
target_precompile_headers(compose-parser-poc PRIVATE precomp.h)
target_compile_options(compose-parser-poc PRIVATE /W4 /utf-8)

if(BUILD_TESTING)
    add_executable(compose-parser-tests tests/ParserTests.cpp)
    target_link_libraries(compose-parser-tests PRIVATE compose_parser)
    target_precompile_headers(compose-parser-tests PRIVATE precomp.h)
    target_compile_options(compose-parser-tests PRIVATE /W4 /utf-8)
    target_compile_definitions(compose-parser-tests PRIVATE
        COMPOSE_FIXTURES="${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures")
    if(TARGET compose_worker)
        add_executable(compose-fake-worker tests/FakeWorker.cpp)
        target_link_libraries(compose-fake-worker PRIVATE compose_parser)
        target_precompile_headers(compose-fake-worker PRIVATE precomp.h)
        add_executable(compose-transport-tests tests/TransportTests.cpp)
        target_link_libraries(compose-transport-tests PRIVATE compose_parser)
        target_precompile_headers(compose-transport-tests PRIVATE precomp.h)
        add_dependencies(compose-transport-tests compose-fake-worker)
        add_test(NAME compose-transport COMMAND compose-transport-tests "$<TARGET_FILE:compose-fake-worker>")
        set_tests_properties(compose-transport PROPERTIES TIMEOUT 60)
        add_dependencies(compose-parser-tests compose_worker)
        add_test(NAME compose-contract COMMAND compose-parser-tests "${COMPOSE_WORKER_PATH}")
    else()
        add_test(NAME compose-contract COMMAND compose-parser-tests)
    endif()
    set_tests_properties(compose-contract PROPERTIES TIMEOUT 120)
endif()
