// Copyright (C) Microsoft Corporation. All rights reserved.
#include "precomp.h"
#include "Common.h"
#include "private/Library.hpp"
#include "private/Protocol.hpp"
#include "compose-go.h"
#include <future>
#include <limits>
#include <wil/win32_helpers.h>

namespace ComposeTests {
using namespace wsl::windows::compose;
using detail::LibraryOptions;
using detail::NormalizeWithLibrary;

namespace {

    void Check(bool condition, const char* message)
    {
        if (!condition)
        {
            LogError("%s", message);
        }
        VERIFY_IS_TRUE(condition);
    }

    Request Input(std::string name = "demo")
    {
        Request request;
        request.ProjectName = std::move(name);
        request.ProjectDirectory = R"(C:\captured\project)";
        request.Sources.push_back({"base", "compose.yaml", "services: {app: {image: alpine}}"});
        return request;
    }

    void Error(const Request& request, const LibraryOptions& options, std::string_view code)
    {
        const auto result = NormalizeWithLibrary(request, options);
        Check(
            !result.AcceptedProject && !result.Diagnostics.empty() && result.Diagnostics[0].Code == code, "Wrong failure result");
    }

    void Abi(const LibraryOptions& options)
    {
        // Normalize pins the runtime before this test takes a temporary reference.
        Check(NormalizeWithLibrary(Input(), options).AcceptedProject.has_value(), "Initial C++ to Go roundtrip failed");
        wil::unique_hmodule module{
            LoadLibraryExW(options.LibraryPath.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32)};
        THROW_LAST_ERROR_IF(!module);
        const auto normalize = reinterpret_cast<decltype(&ComposeNormalize)>(GetProcAddress(module.get(), "ComposeNormalize"));
        const auto release = reinterpret_cast<decltype(&ComposeFree)>(GetProcAddress(module.get(), "ComposeFree"));
        Check(normalize != nullptr && release != nullptr, "Missing ABI exports");
        release(nullptr);

        uint8_t* output{};
        size_t length = 99;
        Check(
            normalize(nullptr, 1, &output, &length) == COMPOSE_INVALID_ARGUMENT && output == nullptr && length == 0,
            "Null input with nonzero length accepted");
        Check(normalize(nullptr, 0, nullptr, &length) == COMPOSE_INVALID_ARGUMENT && length == 0, "Null output accepted");
        Check(normalize(nullptr, 0, &output, nullptr) == COMPOSE_INVALID_ARGUMENT && output == nullptr, "Null length accepted");

        auto call = [&](uint8_t* input, size_t size, std::string_view expected) {
            output = nullptr;
            length = 0;
            auto cleanup = wil::scope_exit([&] { release(output); });
            const auto status = normalize(input, size, &output, &length);
            Check(
                status == COMPOSE_REJECTED && output != nullptr && length != 0 && length <= c_maxResponseBytes,
                "Bad ABI rejection");
            const auto result = DecodeResult(ParseJson({reinterpret_cast<char*>(output), length}));
            Check(!result.AcceptedProject && result.Diagnostics.at(0).Code == expected, "Bad ABI error envelope");
        };
        call(nullptr, 0, "invalid_request");
        uint8_t byte = '{';
        call(&byte, 1, "invalid_request");
        call(&byte, std::numeric_limits<size_t>::max(), "limit_exceeded");
        std::string invalidUtf8(1, static_cast<char>(0xff));
        call(reinterpret_cast<uint8_t*>(invalidUtf8.data()), invalidUtf8.size(), "invalid_request");

        auto encoded = EncodeRequest(Input()).dump();
        std::vector<std::pair<uint8_t*, size_t>> buffers;
        auto cleanup = wil::scope_exit([&] {
            for (const auto& buffer : buffers)
            {
                release(buffer.first);
            }
        });
        for (int i = 0; i < 64; ++i)
        {
            output = nullptr;
            length = 0;
            const auto status = normalize(reinterpret_cast<uint8_t*>(encoded.data()), encoded.size(), &output, &length);
            buffers.emplace_back(output, length);
            Check(status == COMPOSE_SUCCESS && output != nullptr, "ABI success failed");
        }
        encoded.assign(encoded.size(), 'x');
        for (const auto& buffer : buffers)
        {
            Check(
                DecodeResult(ParseJson({reinterpret_cast<char*>(buffer.first), buffer.second})).AcceptedProject.has_value(),
                "Response storage did not survive subsequent calls or input mutation");
        }
    }

    void RepeatedConcurrentCalls(const LibraryOptions& options)
    {
        std::vector<std::future<bool>> calls;
        for (int i = 0; i < 8; ++i)
        {
            calls.emplace_back(std::async(std::launch::async, [&, i] {
                for (int j = 0; j < 20; ++j)
                {
                    auto request = Input();
                    const auto name = "MISSING_" + std::to_string(i);
                    request.Environment["IMAGE"] = "image" + std::to_string(i);
                    request.Sources[0].Content =
                        "services: {app: {image: '$IMAGE', labels: {value: '${OUTER:-${" + name + "}}'}}}";
                    const auto result = NormalizeWithLibrary(request, options);
                    if (!result.AcceptedProject || result.AcceptedProject->Services.at("app").Image != request.Environment["IMAGE"] ||
                        result.Diagnostics.size() != 1 || result.Diagnostics[0].Message.find('"' + name + '"') == std::string::npos)
                    {
                        return false;
                    }
                    request.Sources[0].Content = "services: {app: {build: .}}";
                    const auto rejected = NormalizeWithLibrary(request, options);
                    const auto clean = NormalizeWithLibrary(Input(), options);
                    if (rejected.AcceptedProject || rejected.Diagnostics.empty() ||
                        rejected.Diagnostics[0].Code != "unsupported_feature" || !clean.AcceptedProject || !clean.Diagnostics.empty())
                    {
                        return false;
                    }
                }
                return true;
            }));
        }
        for (auto& call : calls)
        {
            VERIFY_IS_TRUE(call.get());
        }
    }

    void BindingFailures(const LibraryOptions& fake, const LibraryOptions& wrongVersion, const LibraryOptions& missingExports)
    {
        Error(Input(), {}, "backend_error");
        Error(Input(), {L"relative.dll"}, "backend_error");
        Error(Input(), {fake.LibraryPath.parent_path() / L"missing.dll"}, "backend_error");
        Error(Input(), wrongVersion, "backend_error");
        Error(Input(), missingExports, "backend_error");
        for (const auto* name :
             {"malformed", "mismatch", "unknown", "internal-mismatch", "null-response", "oversized", "identity"})
        {
            Error(Input(name), fake, "backend_error");
        }
        Error(Input("internal"), fake, "internal_error");
        Error(Input("no-response"), fake, "internal_error");
        Error(Input(), fake, "invalid_compose");
        const auto module = GetModuleHandleW(fake.LibraryPath.c_str());
        Check(module != nullptr, "Library was not retained");
        const auto allocations = reinterpret_cast<int (*)()>(GetProcAddress(module, "OutstandingAllocations"));
        Check(allocations != nullptr && allocations() == 0, "Rejected responses were not freed by the allocating DLL");
    }

} // namespace

class ComposeLibraryTests
{
    WSLC_TEST_CLASS(ComposeLibraryTests)

    static std::filesystem::path Directory()
    {
        return std::filesystem::path{wil::GetModuleFileNameW<std::wstring>(wil::GetModuleInstanceHandle())}.parent_path();
    }

    TEST_METHOD(AbiInputsAndBufferOwnership)
    {
        Abi({Directory() / L"compose-go.dll"});
    }

    TEST_METHOD(ConcurrentRequestIsolation)
    {
        RepeatedConcurrentCalls({Directory() / L"compose-go.dll"});
    }

    TEST_METHOD(BindingFailuresAndCleanup)
    {
        const auto directory = Directory();
        BindingFailures({directory / L"compose_fake.dll"}, {directory / L"compose_wrong_abi.dll"}, {directory / L"compose_missing_exports.dll"});
    }
};
} // namespace ComposeTests
