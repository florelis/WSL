// Copyright (C) Microsoft Corporation. All rights reserved.
#include "precomp.h"
#include "Protocol.h"
#include <array>
#include <objbase.h>
#include <stdexcept>

namespace wsl::windows::compose {
namespace {

    struct TransportFailure : std::runtime_error
    {
        std::string Code;
        TransportFailure(std::string code, std::string message) : std::runtime_error(std::move(message)), Code(std::move(code))
        {
        }
    };

    class Pipe
    {
    public:
        wil::unique_handle Parent;
        wil::unique_handle Child;
        wil::unique_event Event{wil::EventOptions::ManualReset};
        OVERLAPPED Operation{};
        std::array<char, 8192> Buffer{};
        bool Pending{};
        bool Finished{};
        bool Writing{};
        size_t Offset{};
        std::string Output;

        explicit Pipe(bool writing) : Writing(writing)
        {
            GUID id;
            THROW_IF_FAILED(CoCreateGuid(&id));
            wchar_t guid[40];
            THROW_HR_IF(E_UNEXPECTED, StringFromGUID2(id, guid, ARRAYSIZE(guid)) == 0);
            const std::wstring name = std::wstring{L"\\\\.\\pipe\\wsl-compose-"} + guid;
            Parent.reset(CreateNamedPipeW(
                name.c_str(),
                (writing ? PIPE_ACCESS_OUTBOUND : PIPE_ACCESS_INBOUND) | FILE_FLAG_OVERLAPPED | FILE_FLAG_FIRST_PIPE_INSTANCE,
                PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
                1,
                static_cast<DWORD>(Buffer.size()),
                static_cast<DWORD>(Buffer.size()),
                0,
                nullptr));
            THROW_LAST_ERROR_IF(!Parent || Parent.get() == INVALID_HANDLE_VALUE);
            SECURITY_ATTRIBUTES attributes{sizeof(attributes), nullptr, TRUE};
            Child.reset(CreateFileW(name.c_str(), writing ? GENERIC_READ : GENERIC_WRITE, 0, &attributes, OPEN_EXISTING, 0, nullptr));
            THROW_LAST_ERROR_IF(!Child || Child.get() == INVALID_HANDLE_VALUE);
            Operation.hEvent = Event.get();
            if (!ConnectNamedPipe(Parent.get(), &Operation))
            {
                THROW_LAST_ERROR_IF(GetLastError() != ERROR_PIPE_CONNECTED);
            }
        }

        ~Pipe()
        {
            if (Pending)
            {
                // OVERLAPPED storage and its buffer must outlive cancellation completion.
                CancelIoEx(Parent.get(), &Operation);
                DWORD ignored{};
                GetOverlappedResult(Parent.get(), &Operation, &ignored, TRUE);
            }
        }

        void Accept(DWORD count, std::string_view input, size_t limit)
        {
            if (Writing)
            {
                Offset += count;
                if (Offset == input.size())
                {
                    Finished = true;
                    Parent.reset();
                }
            }
            else
            {
                if (Output.size() + count > limit)
                {
                    throw TransportFailure{"limit_exceeded", "Worker output exceeded its byte limit"};
                }
                Output.append(Buffer.data(), count);
            }
        }

        void Start(std::string_view input, size_t limit)
        {
            if (Finished || Pending)
            {
                return;
            }
            Event.ResetEvent();
            DWORD count{};
            BOOL success;
            if (Writing)
            {
                const auto remaining = (std::min)(input.size() - Offset, Buffer.size());
                success = WriteFile(Parent.get(), input.data() + Offset, static_cast<DWORD>(remaining), &count, &Operation);
            }
            else
            {
                success = ReadFile(Parent.get(), Buffer.data(), static_cast<DWORD>(Buffer.size()), &count, &Operation);
            }
            if (success)
            {
                Accept(count, input, limit);
                return;
            }
            const auto error = GetLastError();
            if (error == ERROR_IO_PENDING)
            {
                Pending = true;
            }
            else if (!Writing && error == ERROR_BROKEN_PIPE)
            {
                Finished = true;
            }
            else
            {
                THROW_WIN32(error);
            }
        }

        void Complete(std::string_view input, size_t limit)
        {
            if (!Pending || !Event.is_signaled())
            {
                return;
            }
            DWORD count{};
            const BOOL success = GetOverlappedResult(Parent.get(), &Operation, &count, FALSE);
            const auto error = success ? ERROR_SUCCESS : GetLastError();
            Pending = false;
            if (!success)
            {
                if (!Writing && error == ERROR_BROKEN_PIPE)
                {
                    Finished = true;
                    return;
                }
                THROW_WIN32(error);
            }
            Accept(count, input, limit);
        }
    };

    Result Run(const std::string& input, const Request& request, const ParserOptions& options)
    {
        const auto deadline = std::chrono::steady_clock::now() + options.Timeout;
        Pipe in{true};
        Pipe out{false};
        Pipe err{false};
        wil::unique_handle job{CreateJobObjectW(nullptr, nullptr)};
        THROW_LAST_ERROR_IF(!job);
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
        limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        THROW_IF_WIN32_BOOL_FALSE(SetInformationJobObject(job.get(), JobObjectExtendedLimitInformation, &limits, sizeof(limits)));

        SIZE_T bytes{};
        InitializeProcThreadAttributeList(nullptr, 2, 0, &bytes);
        THROW_LAST_ERROR_IF(bytes == 0);
        std::vector<unsigned char> storage(bytes);
        auto attributes = reinterpret_cast<PPROC_THREAD_ATTRIBUTE_LIST>(storage.data());
        THROW_IF_WIN32_BOOL_FALSE(InitializeProcThreadAttributeList(attributes, 2, 0, &bytes));
        const auto deleteAttributes = wil::scope_exit([&] { DeleteProcThreadAttributeList(attributes); });
        HANDLE inherited[]{in.Child.get(), out.Child.get(), err.Child.get()};
        HANDLE jobs[]{job.get()};
        THROW_IF_WIN32_BOOL_FALSE(UpdateProcThreadAttribute(
            attributes, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherited, sizeof(inherited), nullptr, nullptr));
        THROW_IF_WIN32_BOOL_FALSE(UpdateProcThreadAttribute(attributes, 0, PROC_THREAD_ATTRIBUTE_JOB_LIST, jobs, sizeof(jobs), nullptr, nullptr));

        STARTUPINFOEXW startup{};
        startup.StartupInfo.cb = sizeof(startup);
        startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
        startup.StartupInfo.hStdInput = in.Child.get();
        startup.StartupInfo.hStdOutput = out.Child.get();
        startup.StartupInfo.hStdError = err.Child.get();
        startup.lpAttributeList = attributes;
        PROCESS_INFORMATION info{};
        const auto executable = options.WorkerPath.native();
        std::wstring command = L"\"" + executable + L"\"";
        // Windows loader/runtime support only; the Compose environment travels in JSON.
        wchar_t systemDirectory[MAX_PATH];
        const UINT length = GetWindowsDirectoryW(systemDirectory, ARRAYSIZE(systemDirectory));
        THROW_LAST_ERROR_IF(length == 0 || length >= ARRAYSIZE(systemDirectory));
        std::wstring environment = std::wstring{L"SystemRoot="} + systemDirectory;
        environment.push_back(L'\0');
        environment.push_back(L'\0');
        const auto workingDirectory = options.WorkerPath.parent_path().native();
        THROW_IF_WIN32_BOOL_FALSE(CreateProcessW(
            executable.c_str(),
            command.data(),
            nullptr,
            nullptr,
            TRUE,
            EXTENDED_STARTUPINFO_PRESENT | CREATE_UNICODE_ENVIRONMENT | CREATE_NO_WINDOW,
            environment.data(),
            workingDirectory.c_str(),
            &startup.StartupInfo,
            &info));
        wil::unique_handle process{info.hProcess};
        wil::unique_handle thread{info.hThread};
        const auto reap = wil::scope_exit([&] {
            LOG_IF_WIN32_BOOL_FALSE(TerminateJobObject(job.get(), 3));
            LOG_HR_IF(E_UNEXPECTED, WaitForSingleObject(process.get(), INFINITE) != WAIT_OBJECT_0);
        });
        in.Child.reset();
        out.Child.reset();
        err.Child.reset();
        bool exited = false;
        while (!exited || !in.Finished || !out.Finished || !err.Finished)
        {
            const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());
            if (remaining.count() <= 0)
            {
                throw TransportFailure{"timeout", "Worker exceeded the end-to-end deadline"};
            }
            in.Complete(input, 0);
            out.Complete({}, c_maxResponseBytes);
            err.Complete({}, c_maxStderrBytes);
            in.Start(input, 0);
            out.Start({}, c_maxResponseBytes);
            err.Start({}, c_maxStderrBytes);
            exited = WaitForSingleObject(process.get(), 0) == WAIT_OBJECT_0;
            if ((!in.Finished && !in.Pending) || (!out.Finished && !out.Pending) || (!err.Finished && !err.Pending))
            {
                continue;
            }
            std::vector<HANDLE> waits;
            if (!exited)
            {
                waits.push_back(process.get());
            }
            for (auto pipe : {&in, &out, &err})
            {
                if (pipe->Pending)
                {
                    waits.push_back(pipe->Event.get());
                }
            }
            if (!waits.empty())
            {
                const auto waited = WaitForMultipleObjects(
                    static_cast<DWORD>(waits.size()), waits.data(), FALSE, static_cast<DWORD>((std::min<int64_t>)(remaining.count(), MAXDWORD - 1)));
                THROW_LAST_ERROR_IF(waited == WAIT_FAILED);
            }
        }
        DWORD exitCode{};
        THROW_IF_WIN32_BOOL_FALSE(GetExitCodeProcess(process.get(), &exitCode));
        auto result = DecodeResult(ParseJson(out.Output));
        if ((result.AcceptedProject && exitCode != 0) || (!result.AcceptedProject && exitCode != 2))
        {
            throw TransportFailure{"transport_error", "Worker exit code disagrees with its response"};
        }
        const auto path = [](const std::string& value) {
            return std::filesystem::path{std::u8string{value.begin(), value.end()}}.lexically_normal();
        };
        if (result.AcceptedProject && (result.AcceptedProject->Name != request.ProjectName ||
                                       path(result.AcceptedProject->Directory) != path(request.ProjectDirectory)))
        {
            throw TransportFailure{"transport_error", "Worker changed explicit project identity"};
        }
        return result;
    }

} // namespace

Result Normalize(const Request& request, const ParserOptions& options)
{
    if (const auto error = ValidateRequest(request))
    {
        return {{*error}, std::nullopt};
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
    if (!options.WorkerPath.is_absolute() || options.WorkerPath.native().find(L'"') != std::wstring::npos || options.Timeout.count() <= 0)
    {
        return Failure("invalid_request", "A trusted absolute worker path and positive timeout are required");
    }
    try
    {
        return Run(input, request, options);
    }
    catch (const TransportFailure& error)
    {
        return Failure(error.Code, error.what());
    }
    catch (const std::exception& error)
    {
        return Failure("transport_error", error.what());
    }
}

} // namespace wsl::windows::compose
