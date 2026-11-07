// Copyright (C) Microsoft Corporation. All rights reserved.
#pragma once

#include "../ComposeParser.hpp"
#include <chrono>
#include <filesystem>

namespace wsl::windows::compose::detail {

struct WorkerOptions
{
    std::filesystem::path WorkerPath;
    std::chrono::milliseconds Timeout{15000};
};

Result NormalizeWithWorker(const Request& request, const WorkerOptions& options);

} // namespace wsl::windows::compose::detail
