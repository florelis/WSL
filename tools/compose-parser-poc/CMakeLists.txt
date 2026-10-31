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
    add_test(NAME compose-contract COMMAND compose-parser-tests "${COMPOSE_LIBRARY_PATH}")
    set_tests_properties(compose-contract PROPERTIES TIMEOUT 120)

    foreach(variant IN ITEMS fake wrong-abi missing-exports)
        add_library(compose-${variant} SHARED tests/FakeLibrary.cpp)
        target_link_libraries(compose-${variant} PRIVATE compose_parser)
        target_precompile_headers(compose-${variant} PRIVATE precomp.h)
        target_compile_options(compose-${variant} PRIVATE /W4 /utf-8)
        target_link_options(compose-${variant} PRIVATE /EXPORT:ComposeAbiVersion /EXPORT:OutstandingAllocations)
        if(NOT variant STREQUAL "missing-exports")
            target_link_options(compose-${variant} PRIVATE /EXPORT:ComposeNormalize /EXPORT:ComposeFree)
        endif()
    endforeach()
    target_compile_definitions(compose-wrong-abi PRIVATE WRONG_ABI)
    target_compile_definitions(compose-missing-exports PRIVATE MISSING_EXPORTS)
    add_executable(compose-library-tests tests/LibraryTests.cpp)
    target_link_libraries(compose-library-tests PRIVATE compose_parser)
    target_precompile_headers(compose-library-tests PRIVATE precomp.h)
    target_compile_options(compose-library-tests PRIVATE /W4 /utf-8)
    add_dependencies(compose-library-tests compose-fake compose-wrong-abi compose-missing-exports)
    add_test(NAME compose-library COMMAND compose-library-tests "${COMPOSE_LIBRARY_PATH}"
        "$<TARGET_FILE:compose-fake>" "$<TARGET_FILE:compose-wrong-abi>" "$<TARGET_FILE:compose-missing-exports>")
    set_tests_properties(compose-library PROPERTIES TIMEOUT 120)
endif()
