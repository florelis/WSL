// Copyright (C) Microsoft Corporation. All rights reserved.

#include "precomp.h"
#include "InstallGuard.h"
#include "hcs.hpp"
#include <TlHelp32.h>
#include <charconv>

namespace {

bool IsServiceStopped(LPCWSTR name)
{
    const wil::unique_schandle manager{OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT)};
    THROW_LAST_ERROR_IF(!manager);

    const wil::unique_schandle service{OpenServiceW(manager.get(), name, SERVICE_QUERY_STATUS)};
    if (!service)
    {
        THROW_LAST_ERROR_IF(GetLastError() != ERROR_SERVICE_DOES_NOT_EXIST);
        return true;
    }

    SERVICE_STATUS status{};
    THROW_IF_WIN32_BOOL_FALSE(QueryServiceStatus(service.get(), &status));
    return status.dwCurrentState == SERVICE_STOPPED;
}

wil::unique_handle OpenProcessIfPresent(DWORD pid)
{
    wil::unique_handle process{OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid)};
    if (!process)
    {
        const auto error = GetLastError();
        THROW_WIN32_IF(error, error != ERROR_INVALID_PARAMETER && error != ERROR_ACCESS_DENIED);
        return {};
    }

    return process;
}

bool AnyProcess(std::function<bool(const PROCESSENTRY32&)> filter)
{
    const wil::unique_tool_help_snapshot snapshot{CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0)};
    THROW_LAST_ERROR_IF(!snapshot);

    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    HRESULT queryError = S_OK;
    if (Process32FirstW(snapshot.get(), &entry))
    {
        do
        {
            try
            {
                if (filter(entry))
                {
                    return true;
                }
            }
            catch (...)
            {
                queryError = wil::ResultFromCaughtException();
                WSL_LOG("AnyProcessQueryFailed", TraceLoggingValue(entry.th32ProcessID, "Pid"), TraceLoggingHResult(queryError));
            }
        } while (Process32NextW(snapshot.get(), &entry));
    }

    THROW_LAST_ERROR_IF(GetLastError() != ERROR_NO_MORE_FILES);
    THROW_IF_FAILED(queryError);
    return false;
}

// Checks if a given process is using the WSL subsystem.
bool IsWsl1Process(DWORD pid)
{
    const auto process = OpenProcessIfPresent(pid);
    if (!process)
    {
        return false;
    }

    // These are documented but not declared in the header.
    constexpr auto ProcessInfoClass_ProcessSubsystemInformation = static_cast<PROCESSINFOCLASS>(75);
    constexpr ULONG SubsystemInformationTypeWsl = 1;

    ULONG subsystem{};
    THROW_IF_NTSTATUS_FAILED(
        NtQueryInformationProcess(process.get(), ProcessInfoClass_ProcessSubsystemInformation, &subsystem, sizeof(subsystem), nullptr));

    return subsystem == SubsystemInformationTypeWsl;
}
} // namespace

bool wsl::windows::common::install::HasRunningWsl1Instances()
{
    return AnyProcess([](const PROCESSENTRY32& entry) { return IsWsl1Process(entry.th32ProcessID); });
}

bool wsl::windows::common::install::HasRunningWsl2Instances()
{
    if (IsServiceStopped(L"vmcompute"))
    {
        return false;
    }

    // Look for any compute systems owned by WSL
    const auto operation = hcs::CreateOperation();
    THROW_IF_FAILED(HcsEnumerateComputeSystems(LR"({"Owners":["WSL"]})", operation.get()));

    wil::unique_cotaskmem_string result;
    THROW_IF_FAILED(HcsWaitForOperationResult(operation.get(), 10000, &result));
    THROW_HR_IF_NULL(E_UNEXPECTED, result.get());
    const auto systems = nlohmann::json::parse(wsl::shared::string::WideToMultiByte(result.get()));
    THROW_HR_IF(E_UNEXPECTED, !systems.is_array());
    return !systems.empty();
}

bool wsl::windows::common::install::HasRunningWslcSessions()
{
    return AnyProcess([](const PROCESSENTRY32& entry) {
        // TODO: This can be improved - Session hosts can remain alive without a VM.
        return _wcsicmp(entry.szExeFile, L"wslcsession.exe") == 0;
    });
}

bool wsl::windows::common::install::IsExplicitInstallClient(std::wstring_view ClientProcessId) noexcept
try
{
    // Parse the PID and find the process
    const auto pidString = wsl::windows::common::string::WideToMultiByte(ClientProcessId);
    DWORD pid{};
    const auto result = std::from_chars(pidString.data(), pidString.data() + pidString.size(), pid);
    if (result.ec != std::errc{} || result.ptr != pidString.data() + pidString.size() || pid == 0)
    {
        return false;
    }

    const auto process = OpenProcessIfPresent(pid);
    if (!process)
    {
        return false;
    }

    // Check if the process is msiexec or wsl.
    // TODO: Consider if we need additional validation. For example, checking for a MS signature,
    // a specific location, or the original file name.
    std::wstring path;
    THROW_IF_FAILED(wil::QueryFullProcessImageNameW(process.get(), 0, path));
    const auto name = std::filesystem::path(path).filename().wstring();
    return _wcsicmp(name.c_str(), L"msiexec.exe") == 0 || _wcsicmp(name.c_str(), L"wsl.exe") == 0;
}
catch (...)
{
    LOG_CAUGHT_EXCEPTION();
    return false;
}
