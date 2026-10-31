// Copyright (C) Microsoft Corporation. All rights reserved.
#pragma once

#include "ComposeParser.h"
#include <nlohmann/json.hpp>

namespace wsl::windows::compose {

nlohmann::json ParseJson(std::string_view text);
Request DecodeRequest(const nlohmann::json& value);
nlohmann::json EncodeRequest(const Request& request);
Result DecodeResult(const nlohmann::json& value);
nlohmann::json EncodeResult(const Result& result);
std::optional<Diagnostic> ValidateRequest(const Request& request);
std::optional<Diagnostic> ValidateDependencyGraph(const Project& project);
Result Failure(std::string code, std::string message, std::string sourceId = {}, std::string path = {});

} // namespace wsl::windows::compose
