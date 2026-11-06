// Copyright (C) Microsoft Corporation. All rights reserved.
#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace wsl::windows::compose {

struct Source
{
    std::string Id;
    std::string Filename;
    std::string Content;
};

struct Request
{
    std::vector<Source> Sources;
    std::string ProjectName;
    std::string ProjectDirectory;
    std::map<std::string, std::string> Environment;
    std::vector<std::string> Profiles;
};

struct Diagnostic
{
    std::string Code;
    std::string Severity;
    std::string Message;
    std::string SourceId;
    std::string Path;
};

enum class CommandState
{
    Absent,
    Null,
    Value
};

struct Command
{
    CommandState State = CommandState::Absent;
    std::vector<std::string> Arguments;
};

struct Port
{
    uint16_t Target{};
    std::optional<uint16_t> Published;
    std::string HostIp;
    std::string Protocol = "tcp";
};

struct Mount
{
    std::string Type;
    std::string Source;
    std::string Target;
    bool ReadOnly{};
    bool CreateHostPath{};
};

struct Service
{
    std::string Image;
    Command CommandLine;
    Command Entrypoint;
    std::map<std::string, std::string> Environment;
    std::map<std::string, std::string> Labels;
    std::vector<Port> Ports;
    std::vector<Mount> Mounts;
    std::vector<std::string> Dependencies;
    std::vector<std::string> Profiles;
    bool Enabled{};
};

struct Project
{
    std::string Name;
    std::string Directory;
    std::map<std::string, Service> Services;
    std::map<std::string, std::string> Volumes;
    std::string DefaultNetworkName;
};

struct Result
{
    std::vector<Diagnostic> Diagnostics;
    std::optional<Project> AcceptedProject;
};

} // namespace wsl::windows::compose
