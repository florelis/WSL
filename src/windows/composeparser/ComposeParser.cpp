// Copyright (C) Microsoft Corporation. All rights reserved.
#include "precomp.h"
#include "private/Protocol.hpp"
#include "private/Library.hpp"
#include "compose-go.h"

namespace wsl::windows::compose {
namespace {

    template <typename T>
    T Resolve(HMODULE module, const char* name)
    {
        const auto address = GetProcAddress(module, name);
        THROW_LAST_ERROR_IF(!address);
        return reinterpret_cast<T>(address);
    }

} // namespace

Result detail::NormalizeWithLibrary(const Request& request, const LibraryOptions& options)
{
    try
    {
        if (const auto diagnostic = ValidateRequest(request))
        {
            return {{*diagnostic}, std::nullopt};
        }
        std::string input;
        try
        {
            input = EncodeRequest(request).dump();
        }
        catch (const nlohmann::json::exception& error)
        {
            return Failure("invalid_request", error.what());
        }
        if (input.size() > c_maxRequestBytes)
        {
            return Failure("limit_exceeded", "Request exceeds 4 MiB");
        }
        if (!options.LibraryPath.is_absolute() || options.LibraryPath.native().find(L'\0') != std::wstring::npos)
        {
            return Failure("backend_error", "Supply a trusted absolute DLL path");
        }
        wil::unique_hmodule module{
            LoadLibraryExW(options.LibraryPath.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32)};
        THROW_LAST_ERROR_IF(!module);

        // Even a rejected ABI may have started Go runtime threads. Never unload it.
        HMODULE pinned{};
        if (!GetModuleHandleExW(
                GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN, reinterpret_cast<LPCWSTR>(module.get()), &pinned))
        {
            const auto error = GetLastError();
            module.release();
            THROW_WIN32(error);
        }
        const auto version = Resolve<decltype(&ComposeAbiVersion)>(module.get(), "ComposeAbiVersion");
        const auto normalize = Resolve<decltype(&ComposeNormalize)>(module.get(), "ComposeNormalize");
        const auto freeResponse = Resolve<decltype(&ComposeFree)>(module.get(), "ComposeFree");
        if (version() != COMPOSE_ABI_VERSION)
        {
            return Failure("backend_error", "Unsupported parser ABI version");
        }

        uint8_t* output{};
        size_t length{};
        auto cleanup = wil::scope_exit([&] { freeResponse(output); });
        const auto status = normalize(reinterpret_cast<uint8_t*>(input.data()), input.size(), &output, &length);
        if (status == COMPOSE_INTERNAL_ERROR && output == nullptr && length == 0)
        {
            return Failure("internal_error", "Parser failed without a response");
        }
        if (output == nullptr || length == 0 || length > c_maxResponseBytes)
        {
            return Failure("backend_error", "Invalid parser response buffer or length");
        }
        auto result = DecodeResult(ParseJson({reinterpret_cast<const char*>(output), length}));
        if (status != COMPOSE_SUCCESS && status != COMPOSE_REJECTED && status != COMPOSE_INTERNAL_ERROR)
        {
            return Failure("backend_error", "Unexpected parser status");
        }
        if ((status == COMPOSE_SUCCESS) != result.AcceptedProject.has_value())
        {
            return Failure("backend_error", "Parser status disagrees with response");
        }
        const auto internal = std::any_of(result.Diagnostics.begin(), result.Diagnostics.end(), [](const auto& diagnostic) {
            return diagnostic.Code == "internal_error" && diagnostic.Severity == "error";
        });
        if ((status == COMPOSE_INTERNAL_ERROR) != internal)
        {
            return Failure("backend_error", "Parser internal-error status disagrees with diagnostics");
        }
        const auto path = [](const std::string& value) {
            return std::filesystem::path{std::u8string{value.begin(), value.end()}}.lexically_normal();
        };
        if (result.AcceptedProject && (result.AcceptedProject->Name != request.ProjectName ||
                                       path(result.AcceptedProject->Directory) != path(request.ProjectDirectory)))
        {
            return Failure("backend_error", "Parser changed explicit project identity");
        }
        return result;
    }
    catch (const std::exception& error)
    {
        return Failure("backend_error", error.what());
    }
}

Result Normalize(const Request& request)
{
    try
    {
        const std::filesystem::path module{wil::GetModuleFileNameW<std::wstring>(wil::GetModuleInstanceHandle())};
        return detail::NormalizeWithLibrary(request, {module.parent_path() / L"compose-go.dll"});
    }
    catch (const std::exception& error)
    {
        return Failure("backend_error", error.what());
    }
}

} // namespace wsl::windows::compose
