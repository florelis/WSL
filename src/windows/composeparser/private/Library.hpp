// Copyright (C) Microsoft Corporation. All rights reserved.
#pragma once

#include "../ComposeParser.hpp"
#include <filesystem>

namespace wsl::windows::compose::detail {

struct LibraryOptions
{
    std::filesystem::path LibraryPath;
};

Result NormalizeWithLibrary(const Request& request, const LibraryOptions& options);

} // namespace wsl::windows::compose::detail
