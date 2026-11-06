// Copyright (C) Microsoft Corporation. All rights reserved.
#pragma once

#include "ComposeModel.hpp"

namespace wsl::windows::compose {

// Normalizes captured UTF-8 inputs without file discovery or container operations.
// The contract is defined in contract.md; an implementation is supplied separately.
Result Normalize(const Request& request);

} // namespace wsl::windows::compose
