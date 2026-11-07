# Copyright (C) Microsoft Corporation. All rights reserved.
add_library(compose_parser STATIC ComposeParser.cpp Protocol.cpp)
target_compile_features(compose_parser PUBLIC cxx_std_20)
target_link_libraries(compose_parser PRIVATE nlohmann_json::nlohmann_json yaml-cpp::yaml-cpp)
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
