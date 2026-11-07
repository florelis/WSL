// Copyright (C) Microsoft Corporation. All rights reserved.
#pragma once

#include <cstddef>

namespace wsl::windows::compose {

inline constexpr size_t c_maxRequestBytes = 4 * 1024 * 1024;
inline constexpr size_t c_maxResponseBytes = 16 * 1024 * 1024;
inline constexpr size_t c_maxStderrBytes = 64 * 1024;
inline constexpr size_t c_maxDepth = 64;
inline constexpr size_t c_maxNodes = 100000;

} // namespace wsl::windows::compose
