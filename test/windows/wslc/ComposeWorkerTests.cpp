// Copyright (C) Microsoft Corporation. All rights reserved.
#include "precomp.h"
#include "Common.h"
#include "private/Worker.hpp"
#include <wil/win32_helpers.h>

namespace ComposeTests {
using namespace wsl::windows::compose;

class ComposeWorkerTests
{
    WSLC_TEST_CLASS(ComposeWorkerTests)

    TEST_METHOD(TransportFailuresAndHandleLifetime)
    {
        const auto worker = std::filesystem::path{wil::GetModuleFileNameW<std::wstring>(wil::GetModuleInstanceHandle())}.parent_path() /
                            L"compose_fake_worker.exe";
        const std::pair<const char*, const char*> cases[]{
            {"normal", "invalid_compose"},
            {"early_exit", "backend_error"},
            {"blocked_writer", "timeout"},
            {"malformed", "backend_error"},
            {"crash", "backend_error"},
            {"duplicate", "backend_error"},
            {"unknown", "backend_error"},
            {"version", "backend_error"},
            {"invalid_success", "backend_error"},
            {"identity", "backend_error"},
            {"exit_mismatch", "backend_error"},
            {"stdout_exact", "invalid_compose"},
            {"stdout_over", "limit_exceeded"},
            {"stderr_exact", "invalid_compose"},
            {"stderr_over", "limit_exceeded"},
            {"timeout", "timeout"}};
        Request warmup;
        warmup.ProjectName = "normal";
        warmup.ProjectDirectory = "C:\\captured\\project";
        warmup.Sources.push_back({"base", "", "services: {}"});
        const auto warmed = detail::NormalizeWithWorker(warmup, {worker, std::chrono::milliseconds{5000}});
        VERIFY_IS_FALSE(warmed.AcceptedProject.has_value());
        VERIFY_IS_TRUE(!warmed.Diagnostics.empty() && warmed.Diagnostics[0].Code == "invalid_compose");
        for (int pass = 0; pass < 2; ++pass)
        {
            DWORD before{};
            VERIFY_WIN32_BOOL_SUCCEEDED(GetProcessHandleCount(GetCurrentProcess(), &before));
            for (const auto& [mode, code] : cases)
            {
                auto request = warmup;
                request.ProjectName = mode;
                request.Sources[0].Content += "\n#" + std::string(512 * 1024, 'x');
                const bool timeout = mode == std::string_view{"timeout"} || mode == std::string_view{"blocked_writer"};
                const detail::WorkerOptions options{worker, std::chrono::milliseconds{timeout ? 250 : 5000}};
                const auto start = std::chrono::steady_clock::now();
                const auto result = detail::NormalizeWithWorker(request, options);
                VERIFY_IS_TRUE(std::chrono::steady_clock::now() - start < std::chrono::seconds{10});
                LogInfo("Worker case: %s", mode);
                VERIFY_IS_FALSE(result.AcceptedProject.has_value());
                VERIFY_IS_TRUE(!result.Diagnostics.empty() && result.Diagnostics[0].Code == code);
            }
            DWORD after{};
            VERIFY_WIN32_BOOL_SUCCEEDED(GetProcessHandleCount(GetCurrentProcess(), &after));
            if (pass != 0)
            {
                VERIFY_IS_TRUE(after <= before);
            }
        }
    }
};
} // namespace ComposeTests
