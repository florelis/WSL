// Copyright (C) Microsoft Corporation. All rights reserved.

#pragma once

#include <string_view>

namespace wsl::windows::common::install {

// Probes throw if the result cannot be determined.
bool HasRunningWsl1Instances();
bool HasRunningWsl2Instances();
bool HasRunningWslcSessions();

// Checks if the client performing this installation is from an explicit install command,
// such as "wsl --update" or "msiexec" (from CLI, or double-clicking the MSI).
// The client is the process that called MsiInstallProduct()
bool IsExplicitInstallClient(std::wstring_view ClientProcessId) noexcept;

} // namespace wsl::windows::common::install
