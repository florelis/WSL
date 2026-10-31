// Copyright (C) Microsoft Corporation. All rights reserved.
#include "precomp.h"
#include "compose-go.h"
#include <atomic>
#include <cstdlib>
#include <cstring>

namespace {
std::atomic<int> g_allocations{};
}

extern "C" uint32_t ComposeAbiVersion()
{
#ifdef WRONG_ABI
    return 99;
#else
    return COMPOSE_ABI_VERSION;
#endif
}

#ifndef MISSING_EXPORTS
extern "C" uint32_t ComposeNormalize(uint8_t* input, size_t length, uint8_t** output, size_t* outputLength)
{
    using namespace wsl::windows::compose;
    const auto request = ParseJson({reinterpret_cast<char*>(input), length});
    const auto mode = request.at("project_name").get<std::string>();
    auto response = EncodeResult(Failure("invalid_compose", "Fake rejection")).dump();
    uint32_t status = COMPOSE_REJECTED;
    if (mode == "malformed")
    {
        response = "{";
    }
    else if (mode == "mismatch")
    {
        status = COMPOSE_SUCCESS;
    }
    else if (mode == "unknown")
    {
        status = 99;
    }
    else if (mode == "internal-mismatch")
    {
        status = COMPOSE_INTERNAL_ERROR;
    }
    else if (mode == "internal")
    {
        response = EncodeResult(Failure("internal_error", "Fake internal failure")).dump();
        status = COMPOSE_INTERNAL_ERROR;
    }
    else if (mode == "no-response")
    {
        *output = nullptr;
        *outputLength = 0;
        return COMPOSE_INTERNAL_ERROR;
    }
    else if (mode == "null-response")
    {
        *output = nullptr;
        *outputLength = 1;
        return COMPOSE_REJECTED;
    }
    *output = static_cast<uint8_t*>(std::malloc(response.size()));
    if (*output == nullptr)
    {
        *outputLength = 0;
        return COMPOSE_INTERNAL_ERROR;
    }
    ++g_allocations;
    std::memcpy(*output, response.data(), response.size());
    *outputLength = mode == "oversized" ? c_maxResponseBytes + 1 : response.size();
    return status;
}

extern "C" void ComposeFree(uint8_t* output)
{
    if (output != nullptr)
    {
        --g_allocations;
        std::free(output);
    }
}
#endif

extern "C" int OutstandingAllocations()
{
    return g_allocations;
}
