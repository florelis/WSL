// Copyright (C) Microsoft Corporation. All rights reserved.
#include "precomp.h"
#include <iterator>

using namespace wsl::windows::compose;

int wmain()
{
    try
    {
        // Fill both outputs before reading stdin to exercise full-duplex I/O.
        constexpr size_t prefix = 128 * 1024;
        std::cout << std::string(prefix, ' ') << std::flush;
        std::cerr << std::string(32 * 1024, 'x') << std::flush;
        char start[4096];
        std::cin.read(start, sizeof(start));
        std::string input{start, static_cast<size_t>(std::cin.gcount())};
        if (input.find("\"project_name\":\"early_exit\"") != std::string::npos)
        {
            return 42;
        }
        if (input.find("\"project_name\":\"blocked_writer\"") != std::string::npos)
        {
            wil::unique_event never;
            never.create();
            WaitForSingleObject(never.get(), INFINITE);
            return 3;
        }
        std::cin.clear();
        input.append(std::istreambuf_iterator<char>{std::cin}, {});
        const auto request = DecodeRequest(ParseJson(input));
        const auto& mode = request.ProjectName;
        if (mode == "timeout")
        {
            wil::unique_event never;
            never.create();
            WaitForSingleObject(never.get(), INFINITE);
            return 3;
        }
        if (mode == "crash")
        {
            return 42;
        }
        if (mode == "malformed")
        {
            std::cout << "{";
            return 0;
        }
        auto result = Failure("invalid_compose", "Synthetic failure");
        auto encoded = EncodeResult(result);
        if (mode == "duplicate")
        {
            std::cout << R"({"version":1,"version":1,"status":"error","diagnostics":[]})";
            return 2;
        }
        if (mode == "unknown")
        {
            encoded["unexpected"] = true;
        }
        if (mode == "version")
        {
            encoded["version"] = 2;
        }
        if (mode == "invalid_success" || mode == "identity")
        {
            Project project;
            project.Name = mode;
            project.Directory = request.ProjectDirectory;
            project.DefaultNetworkName = mode + "_default";
            Service service;
            service.Image = mode == "identity" ? "alpine" : "";
            service.Enabled = true;
            project.Services["app"] = service;
            result.AcceptedProject = std::move(project);
            result.Diagnostics.clear();
            if (mode == "identity")
            {
                result.AcceptedProject->Name = "changed";
                result.AcceptedProject->DefaultNetworkName = "changed_default";
            }
            encoded = EncodeResult(result);
        }
        if (mode == "stderr_exact" || mode == "stderr_over")
        {
            std::cerr << std::string(32 * 1024 + (mode == "stderr_over" ? 1 : 0), 'x') << std::flush;
        }
        const auto text = encoded.dump();
        if (mode == "stdout_exact" || mode == "stdout_over")
        {
            std::cout << std::string(c_maxResponseBytes - prefix - text.size() + (mode == "stdout_over" ? 1 : 0), ' ');
        }
        std::cout << text << std::flush;
        if (mode == "exit_mismatch" || mode == "invalid_success" || mode == "identity")
        {
            return 0;
        }
        return 2;
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what();
        return 3;
    }
}
