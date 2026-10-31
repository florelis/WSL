// Copyright (C) Microsoft Corporation. All rights reserved.
#include "precomp.h"

using namespace wsl::windows::compose;

int wmain(int argc, wchar_t** argv)
{
    if (argc != 2)
    {
        return 1;
    }
    size_t failures{};
    const std::pair<const char*, const char*> cases[]{
        {"normal", "invalid_compose"},
        {"early_exit", "transport_error"},
        {"blocked_writer", "timeout"},
        {"malformed", "transport_error"},
        {"crash", "transport_error"},
        {"duplicate", "transport_error"},
        {"unknown", "transport_error"},
        {"version", "transport_error"},
        {"invalid_success", "transport_error"},
        {"exit_mismatch", "transport_error"},
        {"stdout_exact", "invalid_compose"},
        {"stdout_over", "limit_exceeded"},
        {"stderr_exact", "invalid_compose"},
        {"stderr_over", "limit_exceeded"},
        {"timeout", "timeout"}};
    DWORD before{};
    // Compare steady-state calls after lazy Win32/RPC initialization.
    Request warmup;
    warmup.ProjectName = "normal";
    warmup.ProjectDirectory = "C:\\captured\\project";
    warmup.Sources.push_back({"base", "", "services: {}"});
    const auto warmed = Normalize(warmup, {argv[1], std::chrono::milliseconds{5000}});
    if (warmed.AcceptedProject || warmed.Diagnostics.empty() || warmed.Diagnostics[0].Code != "invalid_compose")
    {
        std::cerr << "FAIL warmup: " << EncodeResult(warmed).dump() << '\n';
        return 1;
    }
    for (int pass = 0; pass < 2; ++pass)
    {
        THROW_IF_WIN32_BOOL_FALSE(GetProcessHandleCount(GetCurrentProcess(), &before));
        for (const auto& [mode, code] : cases)
        {
            Request request;
            request.ProjectName = mode;
            request.ProjectDirectory = "C:\\captured\\project";
            request.Sources.push_back({"base", "", "services: {}\n#" + std::string(512 * 1024, 'x')});
            const bool timeout = mode == std::string_view{"timeout"} || mode == std::string_view{"blocked_writer"};
            ParserOptions options{argv[1], std::chrono::milliseconds{timeout ? 250 : 5000}};
            const auto start = std::chrono::steady_clock::now();
            const auto result = Normalize(request, options);
            const auto elapsed = std::chrono::steady_clock::now() - start;
            if (result.AcceptedProject || result.Diagnostics.empty() || result.Diagnostics[0].Code != code ||
                elapsed > std::chrono::seconds{10})
            {
                ++failures;
                std::cerr << "FAIL " << mode << ": " << EncodeResult(result).dump() << '\n';
            }
            else
            {
                std::cout << "PASS " << mode << '\n';
            }
        }
        DWORD after{};
        THROW_IF_WIN32_BOOL_FALSE(GetProcessHandleCount(GetCurrentProcess(), &after));
        std::cout << "Steady-state handles: " << before << " -> " << after << '\n';
        if (pass != 0 && after > before)
        {
            std::cerr << "FAIL handle leak: " << before << " -> " << after << '\n';
            ++failures;
        }
    }
    return failures == 0 ? 0 : 1;
}
