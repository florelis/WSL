// Copyright (C) Microsoft Corporation. All rights reserved.
#include "precomp.h"
#include "private/Protocol.hpp"
#include <cctype>
#include <functional>
#include <regex>
#include <set>
#include <stdexcept>

namespace wsl::windows::compose {
namespace {

    using Json = nlohmann::json;

    void Keys(const Json& value, std::initializer_list<std::string_view> names)
    {
        if (!value.is_object() || value.size() != names.size())
        {
            throw std::invalid_argument("Unexpected object shape");
        }
        for (const auto name : names)
        {
            if (!value.contains(name))
            {
                throw std::invalid_argument("Missing field: " + std::string{name});
            }
        }
    }

    uint16_t PortNumber(const Json& value)
    {
        if (!value.is_number_integer())
        {
            throw std::invalid_argument("Port must be an integer");
        }
        const auto port = value.get<int64_t>();
        if (port < 1 || port > 65535)
        {
            throw std::invalid_argument("Port outside 1..65535");
        }
        return static_cast<uint16_t>(port);
    }

    Json EncodeCommand(const Command& command)
    {
        const auto state = command.State == CommandState::Absent ? "absent"
                           : command.State == CommandState::Null ? "null"
                                                                 : "value";
        return Json{{"state", state}, {"arguments", command.Arguments}};
    }

    Command DecodeCommand(const Json& value)
    {
        Keys(value, {"state", "arguments"});
        Command command;
        const auto state = value.at("state").get<std::string>();
        if (state == "absent")
        {
            command.State = CommandState::Absent;
        }
        else if (state == "null")
        {
            command.State = CommandState::Null;
        }
        else if (state == "value")
        {
            command.State = CommandState::Value;
        }
        else
        {
            throw std::invalid_argument("Invalid command state");
        }
        command.Arguments = value.at("arguments").get<std::vector<std::string>>();
        if (command.State != CommandState::Value && !command.Arguments.empty())
        {
            throw std::invalid_argument("Arguments on absent/null command");
        }
        return command;
    }

} // namespace

Json ParseJson(std::string_view text)
{
    std::vector<std::set<std::string>> keys;
    return Json::parse(text, [&](int depth, Json::parse_event_t event, Json& value) {
        if (depth > 128)
        {
            throw std::invalid_argument("JSON nesting limit exceeded");
        }
        if (event == Json::parse_event_t::object_start)
        {
            keys.emplace_back();
        }
        else if (event == Json::parse_event_t::object_end)
        {
            keys.pop_back();
        }
        else if (event == Json::parse_event_t::key && !keys.back().insert(value.get<std::string>()).second)
        {
            throw std::invalid_argument("Duplicate JSON key");
        }
        return true;
    });
}

Request DecodeRequest(const Json& value)
{
    Keys(value, {"version", "sources", "project_name", "project_directory", "environment", "profiles"});
    if (!value.at("version").is_number_integer() || value.at("version") != 1)
    {
        throw std::invalid_argument("Unsupported request version");
    }
    Request request;
    request.ProjectName = value.at("project_name").get<std::string>();
    request.ProjectDirectory = value.at("project_directory").get<std::string>();
    request.Environment = value.at("environment").get<std::map<std::string, std::string>>();
    request.Profiles = value.at("profiles").get<std::vector<std::string>>();
    if (!value.at("sources").is_array())
    {
        throw std::invalid_argument("Sources must be an array");
    }
    for (const auto& source : value.at("sources"))
    {
        Keys(source, {"id", "filename", "content"});
        request.Sources.push_back(
            {source.at("id").get<std::string>(), source.at("filename").get<std::string>(), source.at("content").get<std::string>()});
    }
    return request;
}

Json EncodeRequest(const Request& request)
{
    auto sources = Json::array();
    for (const auto& source : request.Sources)
    {
        sources.push_back({{"id", source.Id}, {"filename", source.Filename}, {"content", source.Content}});
    }
    return Json{
        {"version", 1},
        {"sources", sources},
        {"project_name", request.ProjectName},
        {"project_directory", request.ProjectDirectory},
        {"environment", request.Environment},
        {"profiles", request.Profiles}};
}

std::optional<Diagnostic> ValidateRequest(const Request& request)
{
    const auto invalid = [](std::string message) {
        return std::optional<Diagnostic>{{"invalid_request", "error", std::move(message), {}, {}}};
    };
    if (request.Sources.empty())
    {
        return invalid("At least one source is required");
    }
    if (!std::regex_match(request.ProjectName, std::regex{"[a-z0-9][a-z0-9_-]*"}))
    {
        return invalid("Invalid explicit project name");
    }
    const auto& directory = request.ProjectDirectory;
    const bool drive = directory.size() >= 3 && std::isalpha(static_cast<unsigned char>(directory[0])) && directory[1] == ':' &&
                       (directory[2] == '\\' || directory[2] == '/');
    const auto share = directory.find('\\', 2);
    const bool unc = directory.size() > 4 && directory.starts_with("\\\\") && directory[2] != '?' && directory[2] != '.' &&
                     share != std::string::npos && share > 2 && share + 1 < directory.size() && directory[share + 1] != '\\';
    if ((!drive && !unc) || directory.find('\0') != std::string::npos)
    {
        return invalid("A fully qualified Windows project directory is required");
    }
    std::set<std::string> ids;
    for (const auto& source : request.Sources)
    {
        if (source.Id.empty() || source.Content.empty() || !ids.insert(source.Id).second)
        {
            return invalid("Sources require unique nonempty IDs and nonempty captured contents");
        }
    }
    for (const auto& profile : request.Profiles)
    {
        if (profile != "*" && !std::regex_match(profile, std::regex{"[a-zA-Z0-9][a-zA-Z0-9_.-]*"}))
        {
            return invalid("Invalid selected profile");
        }
    }
    return std::nullopt;
}

Result Failure(std::string code, std::string message, std::string sourceId, std::string path)
{
    return {{{std::move(code), "error", std::move(message), std::move(sourceId), std::move(path)}}, std::nullopt};
}

Json EncodeResult(const Result& result)
{
    auto diagnostics = Json::array();
    for (const auto& diagnostic : result.Diagnostics)
    {
        diagnostics.push_back(
            {{"code", diagnostic.Code},
             {"severity", diagnostic.Severity},
             {"message", diagnostic.Message},
             {"source_id", diagnostic.SourceId},
             {"path", diagnostic.Path}});
    }
    Json value{{"version", 1}, {"status", result.AcceptedProject ? "success" : "error"}, {"diagnostics", diagnostics}};
    if (result.AcceptedProject)
    {
        const auto& project = *result.AcceptedProject;
        auto services = Json::object();
        for (const auto& [name, service] : project.Services)
        {
            auto ports = Json::array();
            for (const auto& port : service.Ports)
            {
                ports.push_back(
                    {{"target", port.Target},
                     {"published", port.Published ? Json(*port.Published) : Json(nullptr)},
                     {"host_ip", port.HostIp},
                     {"protocol", port.Protocol}});
            }
            auto mounts = Json::array();
            for (const auto& mount : service.Mounts)
            {
                mounts.push_back(
                    {{"type", mount.Type},
                     {"source", mount.Source},
                     {"target", mount.Target},
                     {"read_only", mount.ReadOnly},
                     {"create_host_path", mount.CreateHostPath}});
            }
            services[name] = {
                {"image", service.Image},
                {"command", EncodeCommand(service.CommandLine)},
                {"entrypoint", EncodeCommand(service.Entrypoint)},
                {"environment", service.Environment},
                {"labels", service.Labels},
                {"ports", ports},
                {"mounts", mounts},
                {"dependencies", service.Dependencies},
                {"profiles", service.Profiles},
                {"enabled", service.Enabled}};
        }
        value["project"] = {
            {"name", project.Name},
            {"directory", project.Directory},
            {"services", services},
            {"volumes", project.Volumes},
            {"default_network_name", project.DefaultNetworkName}};
    }
    return value;
}

Result DecodeResult(const Json& value)
{
    const auto status = value.at("status").get<std::string>();
    if (status == "success")
    {
        Keys(value, {"version", "status", "diagnostics", "project"});
    }
    else if (status == "error")
    {
        Keys(value, {"version", "status", "diagnostics"});
    }
    else
    {
        throw std::invalid_argument("Invalid result status");
    }
    if (!value.at("version").is_number_integer() || value.at("version") != 1 || !value.at("diagnostics").is_array())
    {
        throw std::invalid_argument("Invalid result version/diagnostics");
    }
    Result result;
    bool hasError = false;
    for (const auto& diagnostic : value.at("diagnostics"))
    {
        Keys(diagnostic, {"code", "severity", "message", "source_id", "path"});
        Diagnostic item{
            diagnostic.at("code").get<std::string>(),
            diagnostic.at("severity").get<std::string>(),
            diagnostic.at("message").get<std::string>(),
            diagnostic.at("source_id").get<std::string>(),
            diagnostic.at("path").get<std::string>()};
        if (item.Code.empty() || item.Message.empty() || (item.Severity != "error" && item.Severity != "warning"))
        {
            throw std::invalid_argument("Invalid diagnostic");
        }
        hasError |= item.Severity == "error";
        result.Diagnostics.push_back(std::move(item));
    }
    if (hasError == (status == "success"))
    {
        throw std::invalid_argument("Diagnostics disagree with status");
    }
    if (status == "error")
    {
        return result;
    }
    const auto& project = value.at("project");
    Keys(project, {"name", "directory", "services", "volumes", "default_network_name"});
    Project accepted;
    accepted.Name = project.at("name").get<std::string>();
    accepted.Directory = project.at("directory").get<std::string>();
    accepted.Volumes = project.at("volumes").get<std::map<std::string, std::string>>();
    accepted.DefaultNetworkName = project.at("default_network_name").get<std::string>();
    if (!project.at("services").is_object())
    {
        throw std::invalid_argument("Services must be an object");
    }
    for (const auto& [name, service] : project.at("services").items())
    {
        Keys(
            service,
            {"image", "command", "entrypoint", "environment", "labels", "ports", "mounts", "dependencies", "profiles", "enabled"});
        Service item;
        item.Image = service.at("image").get<std::string>();
        item.CommandLine = DecodeCommand(service.at("command"));
        item.Entrypoint = DecodeCommand(service.at("entrypoint"));
        item.Environment = service.at("environment").get<std::map<std::string, std::string>>();
        item.Labels = service.at("labels").get<std::map<std::string, std::string>>();
        item.Dependencies = service.at("dependencies").get<std::vector<std::string>>();
        item.Profiles = service.at("profiles").get<std::vector<std::string>>();
        item.Enabled = service.at("enabled").get<bool>();
        if (name.empty() || item.Image.empty())
        {
            throw std::invalid_argument("Empty service name/image");
        }
        if (!service.at("ports").is_array() || !service.at("mounts").is_array())
        {
            throw std::invalid_argument("Ports and mounts must be arrays");
        }
        for (const auto& port : service.at("ports"))
        {
            Keys(port, {"target", "published", "host_ip", "protocol"});
            if (port.at("protocol") != "tcp" && port.at("protocol") != "udp")
            {
                throw std::invalid_argument("Unsupported port protocol");
            }
            item.Ports.push_back(
                {PortNumber(port.at("target")),
                 port.at("published").is_null() ? std::nullopt : std::optional{PortNumber(port.at("published"))},
                 port.at("host_ip").get<std::string>(),
                 port.at("protocol").get<std::string>()});
        }
        for (const auto& mount : service.at("mounts"))
        {
            Keys(mount, {"type", "source", "target", "read_only", "create_host_path"});
            const auto type = mount.at("type").get<std::string>();
            const auto source = mount.at("source").get<std::string>();
            const auto target = mount.at("target").get<std::string>();
            if ((type != "bind" && type != "volume") || target.empty() || target[0] != '/' ||
                (type == "bind" && source.empty()) || (type == "volume" && mount.at("create_host_path") != false) ||
                (type == "volume" && !source.empty() && !accepted.Volumes.contains(source)))
            {
                throw std::invalid_argument("Invalid mount");
            }
            item.Mounts.push_back(
                {mount.at("type").get<std::string>(),
                 mount.at("source").get<std::string>(),
                 mount.at("target").get<std::string>(),
                 mount.at("read_only").get<bool>(),
                 mount.at("create_host_path").get<bool>()});
        }
        accepted.Services.emplace(name, std::move(item));
    }
    if (const auto error = ValidateDependencyGraph(accepted))
    {
        throw std::invalid_argument(error->Message);
    }
    result.AcceptedProject = std::move(accepted);
    return result;
}

std::optional<Diagnostic> ValidateDependencyGraph(const Project& project)
{
    std::map<std::string, size_t> incoming;
    std::map<std::string, std::vector<std::string>> dependents;
    for (const auto& [name, service] : project.Services)
    {
        incoming[name] = service.Dependencies.size();
        for (const auto& dependency : service.Dependencies)
        {
            const auto found = project.Services.find(dependency);
            if (found == project.Services.end() || (service.Enabled && !found->second.Enabled))
            {
                return Diagnostic{"invalid_compose", "error", "Missing or disabled dependency: " + dependency, {}, "services." + name};
            }
            dependents[dependency].push_back(name);
        }
    }
    std::vector<std::string> ready;
    for (const auto& [name, count] : incoming)
    {
        if (count == 0)
        {
            ready.push_back(name);
        }
    }
    for (size_t index = 0; index < ready.size(); ++index)
    {
        for (const auto& name : dependents[ready[index]])
        {
            if (--incoming[name] == 0)
            {
                ready.push_back(name);
            }
        }
    }
    if (ready.size() != project.Services.size())
    {
        return Diagnostic{"invalid_compose", "error", "Dependency cycle", {}, "services"};
    }
    return std::nullopt;
}

} // namespace wsl::windows::compose
