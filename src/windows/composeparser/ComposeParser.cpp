// Copyright (C) Microsoft Corporation. All rights reserved.
#include "precomp.h"
#include "private/Protocol.hpp"
#include <yaml-cpp/yaml.h>
#include <algorithm>
#include <charconv>
#include <cmath>
#include <functional>
#include <regex>
#include <set>

namespace wsl::windows::compose {
namespace {

    using Json = nlohmann::json;

    struct ParseError : std::runtime_error
    {
        std::string Code;
        std::string Path;
        ParseError(std::string code, std::string message, std::string path = {}) :
            std::runtime_error(std::move(message)), Code(std::move(code)), Path(std::move(path))
        {
        }
    };

    [[noreturn]] void Invalid(std::string_view path, std::string message)
    {
        throw ParseError{"invalid_compose", std::move(message), std::string{path}};
    }

    [[noreturn]] void Unsupported(std::string_view path)
    {
        throw ParseError{"unsupported_feature", "Outside the supported POC slice: " + std::string{path}, std::string{path}};
    }

    void Fields(const Json& value, std::string_view path, std::initializer_list<std::string_view> names)
    {
        if (!value.is_object())
        {
            Invalid(path, "Expected mapping");
        }
        for (const auto& [key, unused] : value.items())
        {
            if (std::find(names.begin(), names.end(), key) == names.end())
            {
                Unsupported(std::string{path} + "." + key);
            }
        }
    }

    std::string String(const Json& value, std::string_view path)
    {
        if (!value.is_string())
        {
            Invalid(path, "Expected string");
        }
        return value.get<std::string>();
    }

    std::vector<std::string> Strings(const Json& value, std::string_view path)
    {
        if (!value.is_array())
        {
            Invalid(path, "Expected string array");
        }
        std::vector<std::string> values;
        for (const auto& item : value)
        {
            values.push_back(String(item, path));
        }
        return values;
    }

    Json Scalar(const YAML::Node& node)
    {
        const auto& tag = node.Tag();
        const auto& text = node.Scalar();
        if (tag == "!" || tag == "tag:yaml.org,2002:str")
        {
            return text;
        }
        if (tag != "?" && tag != "tag:yaml.org,2002:int" && tag != "tag:yaml.org,2002:float" && tag != "tag:yaml.org,2002:bool")
        {
            Unsupported("YAML tag " + tag);
        }
        const bool booleanTag = tag == "?" || tag == "tag:yaml.org,2002:bool";
        if (booleanTag && (text == "true" || text == "True" || text == "TRUE"))
        {
            return true;
        }
        if (booleanTag && (text == "false" || text == "False" || text == "FALSE"))
        {
            return false;
        }
        if (text.empty() || (text[0] != '+' && text[0] != '-' && text[0] != '.' && (text[0] < '0' || text[0] > '9')))
        {
            if (tag != "?")
            {
                throw ParseError{"invalid_yaml", "Invalid tagged scalar"};
            }
            return text;
        }
        std::string numeric = text;
        numeric.erase(std::remove(numeric.begin(), numeric.end(), '_'), numeric.end());
        YAML::Node numericNode{numeric};
        int64_t integer{};
        const bool integerTag = tag == "?" || tag == "tag:yaml.org,2002:int";
        if (integerTag && YAML::convert<int64_t>::decode(numericNode, integer))
        {
            return integer;
        }
        if (integerTag)
        {
            uint64_t unsignedInteger{};
            if (numeric[0] != '-' && YAML::convert<uint64_t>::decode(numericNode, unsignedInteger))
            {
                return unsignedInteger;
            }
            const auto sign = !numeric.empty() && (numeric[0] == '-' || numeric[0] == '+') ? size_t{1} : size_t{0};
            if (numeric.size() > sign + 2 && numeric[sign] == '0' && (numeric[sign + 1] == 'o' || numeric[sign + 1] == 'b'))
            {
                uint64_t number{};
                const auto parsed =
                    std::from_chars(numeric.data() + sign + 2, numeric.data() + numeric.size(), number, numeric[sign + 1] == 'o' ? 8 : 2);
                if (parsed.ec == std::errc{} && parsed.ptr == numeric.data() + numeric.size())
                {
                    if (numeric[0] != '-')
                    {
                        return number;
                    }
                    if (number <= INT64_MAX)
                    {
                        return -static_cast<int64_t>(number);
                    }
                    if (number == uint64_t{INT64_MAX} + 1)
                    {
                        return INT64_MIN;
                    }
                }
            }
        }
        double number{};
        if ((tag == "?" || tag == "tag:yaml.org,2002:float") && YAML::convert<double>::decode(numericNode, number))
        {
            if (!std::isfinite(number))
            {
                Invalid("", "Nonfinite YAML number");
            }
            return number;
        }
        if (tag != "?")
        {
            throw ParseError{"invalid_yaml", "Invalid tagged scalar"};
        }
        return text;
    }

    class YamlReader
    {
        size_t m_visits{};
        std::vector<YAML::Node> m_active;

    public:
        Json Expand(const YAML::Node& node, size_t depth = 1)
        {
            if (depth > c_maxDepth || ++m_visits > c_maxNodes)
            {
                throw ParseError{"limit_exceeded", "YAML structural limit exceeded"};
            }
            for (const auto& active : m_active)
            {
                if (node.is(active))
                {
                    throw ParseError{"invalid_yaml", "Cyclic YAML alias"};
                }
            }
            m_active.push_back(node);
            const auto pop = wil::scope_exit([&] { m_active.pop_back(); });
            if (node.IsNull())
            {
                return nullptr;
            }
            if (node.IsScalar())
            {
                return Scalar(node);
            }
            const auto tag = node.Tag();
            if (tag != "?" && tag != (node.IsMap() ? "tag:yaml.org,2002:map" : "tag:yaml.org,2002:seq"))
            {
                Unsupported("YAML tag " + tag);
            }
            if (node.IsSequence())
            {
                auto array = Json::array();
                for (const auto& child : node)
                {
                    array.push_back(Expand(child, depth + 1));
                }
                return array;
            }
            if (!node.IsMap())
            {
                throw ParseError{"invalid_yaml", "Unexpected YAML node"};
            }
            auto object = Json::object();
            std::vector<Json> merges;
            std::set<std::string> keys;
            for (const auto& entry : node)
            {
                const auto key = Expand(entry.first, depth + 1);
                if (!key.is_string())
                {
                    throw ParseError{"invalid_yaml", "Mapping keys must be strings"};
                }
                const auto name = key.get<std::string>();
                if (!keys.insert(name).second)
                {
                    throw ParseError{"invalid_yaml", "Duplicate YAML key: " + name};
                }
                auto value = Expand(entry.second, depth + 1);
                if (name == "<<" && entry.first.Tag() == "?")
                {
                    if (value.is_object())
                    {
                        merges.push_back(std::move(value));
                    }
                    else if (value.is_array())
                    {
                        for (auto& item : value)
                        {
                            if (!item.is_object())
                            {
                                throw ParseError{"invalid_yaml", "Merge sequence requires mappings"};
                            }
                            merges.push_back(std::move(item));
                        }
                    }
                    else
                    {
                        throw ParseError{"invalid_yaml", "Merge requires mapping or mapping sequence"};
                    }
                }
                else
                {
                    object[name] = std::move(value);
                }
            }
            for (const auto& merge : merges)
            {
                for (const auto& [key, value] : merge.items())
                {
                    if (!object.contains(key))
                    {
                        object[key] = value;
                    }
                }
            }
            return object;
        }
    };

    bool VariableStart(char c)
    {
        return c == '_' || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
    }

    bool VariableChar(char c)
    {
        return VariableStart(c) || (c >= '0' && c <= '9');
    }

    class Interpreter
    {
        const Request& m_request;
        std::map<std::string, std::string> m_environment;
        std::vector<Diagnostic>& m_diagnostics;

        std::string Substitute(std::string_view text, std::string_view path, size_t depth = 0)
        {
            if (depth > c_maxDepth)
            {
                throw ParseError{"limit_exceeded", "Interpolation depth exceeded", std::string{path}};
            }
            std::string result;
            for (size_t i = 0; i < text.size();)
            {
                if (text[i] != '$')
                {
                    result.push_back(text[i++]);
                    continue;
                }
                ++i;
                if (i < text.size() && text[i] == '$')
                {
                    result.push_back('$');
                    ++i;
                    continue;
                }
                const bool braced = i < text.size() && text[i] == '{';
                i += braced ? 1 : 0;
                const auto begin = i;
                if (i == text.size() || !VariableStart(text[i]))
                {
                    if (braced)
                    {
                        Invalid(path, "Invalid interpolation");
                    }
                    result.push_back('$');
                    continue;
                }
                while (i < text.size() && VariableChar(text[i]))
                {
                    ++i;
                }
                const std::string name{text.substr(begin, i - begin)};
                const auto found = m_environment.find(name);
                const bool set = found != m_environment.end();
                const bool nonempty = set && !found->second.empty();
                std::string value = set ? found->second : "";
                bool direct = true;
                if (braced)
                {
                    if (i == text.size())
                    {
                        Invalid(path, "Unclosed interpolation");
                    }
                    if (text[i] != '}')
                    {
                        direct = false;
                        const bool colon = text[i] == ':';
                        i += colon ? 1 : 0;
                        if (i == text.size() || (text[i] != '-' && text[i] != '+' && text[i] != '?'))
                        {
                            Invalid(path, "Unknown interpolation operator");
                        }
                        const char operation = text[i++];
                        const auto argumentStart = i;
                        size_t nesting = 0;
                        while (i < text.size())
                        {
                            if (text[i] == '{')
                            {
                                ++nesting;
                            }
                            if (text[i] == '}')
                            {
                                if (nesting == 0)
                                {
                                    break;
                                }
                                --nesting;
                            }
                            ++i;
                        }
                        if (i == text.size())
                        {
                            Invalid(path, "Unclosed interpolation");
                        }
                        const bool present = colon ? nonempty : set;
                        const auto argument = text.substr(argumentStart, i - argumentStart);
                        if (operation == '-' && !present)
                        {
                            value = Substitute(argument, path, depth + 1);
                        }
                        else if (operation == '+')
                        {
                            value = present ? Substitute(argument, path, depth + 1) : "";
                        }
                        else if (operation == '?' && !present)
                        {
                            Invalid(path, "Required variable " + name + ": " + Substitute(argument, path, depth + 1));
                        }
                    }
                    ++i;
                }
                if (direct && !set)
                {
                    m_diagnostics.push_back({"invalid_compose", "warning", "Undefined variable: " + name, {}, std::string{path}});
                }
                if (result.size() + value.size() > c_maxResponseBytes)
                {
                    throw ParseError{"limit_exceeded", "Expanded scalar exceeds response limit", std::string{path}};
                }
                result += value;
            }
            return result;
        }

        void Interpolate(Json& value, const std::string& path)
        {
            if (value.is_string())
            {
                value = Substitute(value.get_ref<const std::string&>(), path);
            }
            else if (value.is_object())
            {
                for (auto& [key, child] : value.items())
                {
                    Interpolate(child, path + "." + key);
                }
            }
            else if (value.is_array())
            {
                for (auto& child : value)
                {
                    Interpolate(child, path + "[]");
                }
            }
        }

        static bool Boolean(const Json& value, std::string_view path)
        {
            if (value.is_boolean())
            {
                return value.get<bool>();
            }
            if (value == "true" || value == "false")
            {
                return value == "true";
            }
            Invalid(path, "Expected boolean");
        }

        static std::string ScalarString(const Json& value, std::string_view path)
        {
            if (value.is_string())
            {
                return value.get<std::string>();
            }
            if (value.is_number() || value.is_boolean())
            {
                if (value.is_number_float())
                {
                    char buffer[64];
                    const auto converted =
                        std::to_chars(std::begin(buffer), std::end(buffer), value.get<double>(), std::chars_format::general);
                    if (converted.ec != std::errc{})
                    {
                        Invalid(path, "Failed to format numeric scalar");
                    }
                    return {buffer, converted.ptr};
                }
                return value.dump();
            }
            Invalid(path, "Expected scalar");
        }

        static Json KeyValues(const Json& value, std::string_view path)
        {
            auto result = Json::object();
            if (value.is_null())
            {
                return result;
            }
            if (value.is_array())
            {
                for (const auto& entry : Strings(value, path))
                {
                    const auto equal = entry.find('=');
                    result[entry.substr(0, equal)] = equal == std::string::npos ? Json(nullptr) : Json(entry.substr(equal + 1));
                }
            }
            else if (value.is_object())
            {
                for (const auto& [key, item] : value.items())
                {
                    result[key] = item.is_null() ? Json(nullptr) : Json(ScalarString(item, path));
                }
            }
            else
            {
                Invalid(path, "Expected mapping or string array");
            }
            return result;
        }

        static uint16_t Number(const Json& value, std::string_view path)
        {
            if (!value.is_string() && !value.is_number_integer())
            {
                Invalid(path, "Expected port number");
            }
            const auto text = ScalarString(value, path);
            unsigned int number{};
            const auto parsed = std::from_chars(text.data(), text.data() + text.size(), number);
            if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size())
            {
                Unsupported(path);
            }
            if (number < 1 || number > 65535)
            {
                Invalid(path, "Port outside 1..65535");
            }
            return static_cast<uint16_t>(number);
        }

        static std::vector<std::string> Split(const std::string& text, char delimiter)
        {
            std::vector<std::string> parts;
            size_t start = 0;
            for (;;)
            {
                const auto end = text.find(delimiter, start);
                parts.push_back(text.substr(start, end == std::string::npos ? end : end - start));
                if (end == std::string::npos)
                {
                    return parts;
                }
                start = end + 1;
            }
        }

        static bool Ipv4(const std::string& value)
        {
            const auto parts = Split(value, '.');
            if (parts.size() != 4)
            {
                return false;
            }
            for (const auto& part : parts)
            {
                unsigned int number{};
                const auto parsed = std::from_chars(part.data(), part.data() + part.size(), number);
                if (part.empty() || (part.size() > 1 && part[0] == '0') || parsed.ec != std::errc{} ||
                    parsed.ptr != part.data() + part.size() || number > 255)
                {
                    return false;
                }
            }
            return true;
        }

        static Json PortValue(const Json& input, const std::string& path)
        {
            Json value = input;
            if (!value.is_object())
            {
                const auto text = ScalarString(value, path);
                if (text.find_first_of("-[]") != std::string::npos)
                {
                    Unsupported(path);
                }
                const auto protocol = Split(text, '/');
                if (protocol.size() > 2)
                {
                    Unsupported(path);
                }
                const auto ports = Split(protocol[0], ':');
                if (ports.size() > 3)
                {
                    Unsupported(path);
                }
                value = {{"target", ports.back()}};
                if (ports.size() >= 2)
                {
                    value["published"] = ports[ports.size() - 2];
                }
                if (ports.size() == 3)
                {
                    value["host_ip"] = ports[0];
                }
                if (protocol.size() == 2)
                {
                    value["protocol"] = protocol[1];
                }
            }
            Fields(value, path, {"target", "published", "host_ip", "protocol"});
            const auto host = String(value.value("host_ip", Json("")), path);
            const auto protocol = String(value.value("protocol", Json("tcp")), path);
            if ((!host.empty() && !Ipv4(host)) || (protocol != "tcp" && protocol != "udp"))
            {
                Unsupported(path);
            }
            return {
                {"target", Number(value.value("target", Json(nullptr)), path)},
                {"published", value.contains("published") ? Json(Number(value["published"], path)) : Json(nullptr)},
                {"host_ip", host},
                {"protocol", protocol}};
        }

        static std::filesystem::path WindowsPath(const std::string& value)
        {
            return std::filesystem::path{std::u8string{value.begin(), value.end()}};
        }

        static std::string PathString(const std::filesystem::path& path)
        {
            const auto value = path.lexically_normal().u8string();
            return {value.begin(), value.end()};
        }

        static std::string ContainerPath(const std::string& value)
        {
            if (value.empty() || value.front() != '/' || value.find_first_of("\\\0", 0, 2) != std::string::npos)
            {
                Invalid("volumes.target", "Expected absolute Linux mount target");
            }
            std::vector<std::string> parts;
            for (const auto& part : Split(value, '/'))
            {
                if (part == "..")
                {
                    if (!parts.empty())
                    {
                        parts.pop_back();
                    }
                }
                else if (!part.empty() && part != ".")
                {
                    parts.push_back(part);
                }
            }
            std::string result;
            for (const auto& part : parts)
            {
                result += "/" + part;
            }
            return result.empty() ? "/" : result;
        }

        Json MountValue(const Json& input, const std::string& path)
        {
            Json value = input;
            bool shortForm = value.is_string();
            if (shortForm)
            {
                const auto text = value.get<std::string>();
                const auto parts = Split(text, ':');
                if (parts.size() > 3 || (text.size() > 1 && text[1] == ':'))
                {
                    Unsupported(path);
                }
                if (parts.size() == 3 && parts[2] != "ro" && parts[2] != "rw")
                {
                    Unsupported(path);
                }
                const auto source = parts.size() >= 2 ? parts[0] : "";
                if (parts.size() >= 2 && source.empty())
                {
                    Invalid(path, "Empty mount source");
                }
                const bool bind =
                    !source.empty() && (source[0] == '.' || source[0] == '/' || source[0] == '~' || source.starts_with("\\\\"));
                value = {
                    {"type", bind ? "bind" : "volume"},
                    {"source", source},
                    {"target", parts.size() >= 2 ? parts[1] : parts[0]},
                    {"read_only", parts.size() == 3 && parts[2] == "ro"}};
            }
            Fields(value, path, {"type", "source", "target", "read_only", "bind"});
            const auto type = String(value.value("type", Json(nullptr)), path);
            auto source = String(value.value("source", Json("")), path);
            const auto target = ContainerPath(String(value.value("target", Json(nullptr)), path));
            const auto readOnly = Boolean(value.value("read_only", Json(false)), path);
            bool createHostPath = type == "bind" && shortForm;
            if (value.contains("bind"))
            {
                if (type != "bind")
                {
                    Unsupported(path + ".bind");
                }
                Fields(value["bind"], path + ".bind", {"create_host_path"});
                createHostPath = Boolean(value["bind"].value("create_host_path", Json(false)), path);
            }
            if (type == "bind")
            {
                if (source.empty() || source.find('\0') != std::string::npos)
                {
                    Invalid(path, "Empty/invalid bind source");
                }
                const auto sourcePath = WindowsPath(source);
                if (!sourcePath.is_absolute() &&
                    (source.front() == '~' || source.front() == '/' || source.front() == '\\' || source.find(':') != std::string::npos))
                {
                    Unsupported(path + ".source");
                }
                if (source.starts_with("\\\\?") || source.starts_with("\\\\."))
                {
                    Unsupported(path + ".source");
                }
                source = PathString(sourcePath.is_absolute() ? sourcePath : WindowsPath(m_request.ProjectDirectory) / sourcePath);
            }
            else if (type != "volume")
            {
                Unsupported(path + ".type");
            }
            else if (!source.empty() && !std::regex_match(source, std::regex{"[a-zA-Z0-9][a-zA-Z0-9_.-]*"}))
            {
                Invalid(path, "Invalid named volume");
            }
            return {{"type", type}, {"source", source}, {"target", target}, {"read_only", readOnly}, {"create_host_path", createHostPath}};
        }

    public:
        Interpreter(const Request& request, std::vector<Diagnostic>& diagnostics) :
            m_request(request), m_environment(request.Environment), m_diagnostics(diagnostics)
        {
            m_environment["COMPOSE_PROJECT_NAME"] = request.ProjectName;
        }

        Json Prepare(Json root)
        {
            Fields(root, "", {"name", "services", "volumes"});
            if (root.contains("name"))
            {
                String(root["name"], "name");
            }
            if (root.contains("volumes") && !root["volumes"].is_null())
            {
                if (!root["volumes"].is_object())
                {
                    Invalid("volumes", "Expected named-volume mapping");
                }
                for (const auto& [name, value] : root["volumes"].items())
                {
                    if (!std::regex_match(name, std::regex{"[a-zA-Z0-9][a-zA-Z0-9_.-]*"}))
                    {
                        Invalid("volumes." + name, "Invalid volume name");
                    }
                    if (!value.is_null())
                    {
                        Fields(value, "volumes." + name, {});
                    }
                }
            }
            if (!root.contains("services"))
            {
                root["services"] = Json::object();
            }
            if (!root["services"].is_object())
            {
                Invalid("services", "Expected services mapping");
            }
            for (const auto& [name, service] : root["services"].items())
            {
                if (!std::regex_match(name, std::regex{"[a-zA-Z0-9._-]+"}))
                {
                    Invalid("services." + name, "Invalid service name");
                }
                Fields(
                    service,
                    "services." + name,
                    {"image", "command", "entrypoint", "environment", "labels", "ports", "volumes", "depends_on", "profiles"});
            }
            Interpolate(root, "");
            for (auto& [name, service] : root["services"].items())
            {
                for (auto& [key, value] : service.items())
                {
                    const auto path = "services." + name + "." + key;
                    if (key == "image")
                    {
                        String(value, path);
                    }
                    else if (key == "command" || key == "entrypoint")
                    {
                        if (value.is_string())
                        {
                            Unsupported(path);
                        }
                        if (!value.is_null())
                        {
                            Strings(value, path);
                        }
                    }
                    else if (key == "environment" || key == "labels")
                    {
                        value = KeyValues(value, path);
                    }
                    else if (key == "profiles")
                    {
                        for (const auto& profile : Strings(value, path))
                        {
                            if (!std::regex_match(profile, std::regex{"[a-zA-Z0-9][a-zA-Z0-9_.-]*"}))
                            {
                                Invalid(path, "Invalid service profile");
                            }
                        }
                    }
                    else if (key == "ports" || key == "volumes")
                    {
                        if (value.is_null())
                        {
                            value = Json::array();
                        }
                        if (!value.is_array())
                        {
                            Invalid(path, "Expected array");
                        }
                        for (auto& item : value)
                        {
                            item = key == "ports" ? PortValue(item, path) : MountValue(item, path);
                        }
                    }
                    else if (key == "depends_on")
                    {
                        if (value.is_null())
                        {
                            value = Json::object();
                        }
                        if (value.is_array())
                        {
                            auto dependencies = Json::object();
                            for (const auto& dependency : Strings(value, path))
                            {
                                dependencies[dependency] = Json::object();
                            }
                            value = std::move(dependencies);
                        }
                        if (!value.is_object())
                        {
                            Invalid(path, "Expected dependency mapping or string array");
                        }
                        for (const auto& [dependency, options] : value.items())
                        {
                            Fields(options, path + "." + dependency, {"condition", "required", "restart"});
                            if (options.value("condition", Json("service_started")) != "service_started" ||
                                !Boolean(options.value("required", Json(true)), path) ||
                                Boolean(options.value("restart", Json(false)), path))
                            {
                                Unsupported(path + "." + dependency);
                            }
                        }
                    }
                }
            }
            return root;
        }

        static void MergeService(Json& target, const Json& source)
        {
            for (const auto& [key, value] : source.items())
            {
                if ((key == "environment" || key == "labels" || key == "depends_on") && target.contains(key))
                {
                    target[key].update(value);
                }
                else if (key == "ports" || key == "volumes" || key == "profiles")
                {
                    if (!target.contains(key))
                    {
                        target[key] = Json::array();
                    }
                    for (const auto& item : value)
                    {
                        auto& array = target[key];
                        const auto found = std::find_if(array.begin(), array.end(), [&](const Json& existing) {
                            return key == "volumes" ? existing["target"] == item["target"] : existing == item;
                        });
                        if (found == array.end())
                        {
                            array.push_back(item);
                        }
                        else
                        {
                            *found = item;
                        }
                    }
                }
                else
                {
                    target[key] = value;
                }
            }
        }

        Project ProjectModel(const Json& root)
        {
            Project project;
            project.Name = m_request.ProjectName;
            project.Directory = PathString(WindowsPath(m_request.ProjectDirectory));
            if (root.contains("volumes") && root["volumes"].is_object())
            {
                for (const auto& [name, unused] : root["volumes"].items())
                {
                    project.Volumes[name] = project.Name + "_" + name;
                }
            }
            for (const auto& [name, value] : root["services"].items())
            {
                const auto path = "services." + name;
                Service service;
                service.Image = String(value.value("image", Json("")), path + ".image");
                if (service.Image.empty())
                {
                    Invalid(path, "Final service requires a nonempty image");
                }
                for (const auto& [key, command] : {std::pair{"command", &service.CommandLine}, std::pair{"entrypoint", &service.Entrypoint}})
                {
                    if (value.contains(key))
                    {
                        command->State = value[key].is_null() ? CommandState::Null : CommandState::Value;
                        if (command->State == CommandState::Value)
                        {
                            command->Arguments = Strings(value[key], path);
                        }
                    }
                }
                for (const auto& [key, destination] :
                     {std::pair{"environment", &service.Environment}, std::pair{"labels", &service.Labels}})
                {
                    const auto entries = value.value(key, Json::object());
                    for (const auto& [entryName, entry] : entries.items())
                    {
                        if (!entry.is_null())
                        {
                            (*destination)[entryName] = entry.get<std::string>();
                        }
                        else if (std::string_view{key} == "labels")
                        {
                            (*destination)[entryName] = "";
                        }
                        else if (const auto found = m_environment.find(entryName); found != m_environment.end())
                        {
                            (*destination)[entryName] = found->second;
                        }
                    }
                }
                for (const auto& port : value.value("ports", Json::array()))
                {
                    service.Ports.push_back(
                        {port["target"].get<uint16_t>(),
                         port["published"].is_null() ? std::nullopt : std::optional{port["published"].get<uint16_t>()},
                         port["host_ip"].get<std::string>(),
                         port["protocol"].get<std::string>()});
                }
                for (const auto& mount : value.value("volumes", Json::array()))
                {
                    Mount item{mount["type"], mount["source"], mount["target"], mount["read_only"], mount["create_host_path"]};
                    if (item.Type == "volume" && !item.Source.empty() && !project.Volumes.contains(item.Source))
                    {
                        Invalid(path, "Undefined named volume: " + item.Source);
                    }
                    service.Mounts.push_back(std::move(item));
                }
                const auto dependencies = value.value("depends_on", Json::object());
                for (const auto& [dependency, unused] : dependencies.items())
                {
                    service.Dependencies.push_back(dependency);
                }
                service.Profiles = Strings(value.value("profiles", Json::array()), path);
                service.Enabled = service.Profiles.empty() ||
                                  std::any_of(m_request.Profiles.begin(), m_request.Profiles.end(), [&](const auto& selected) {
                                      return selected == "*" || std::find(service.Profiles.begin(), service.Profiles.end(), selected) !=
                                                                    service.Profiles.end();
                                  });
                project.Services[name] = std::move(service);
            }
            if (const auto error = ValidateDependencyGraph(project))
            {
                Invalid(error->Path, error->Message);
            }
            project.DefaultNetworkName = project.Services.empty() ? "" : project.Name + "_default";
            return project;
        }
    };

} // namespace

Result Normalize(const Request& request)
{
    if (const auto error = ValidateRequest(request))
    {
        return {{*error}, std::nullopt};
    }
    Result result;
    std::string sourceId;
    try
    {
        if (EncodeRequest(request).dump().size() > c_maxRequestBytes)
        {
            return Failure("limit_exceeded", "Request exceeds 4 MiB");
        }
        YamlReader reader;
        Interpreter interpreter{request, result.Diagnostics};
        Json merged{{"services", Json::object()}, {"volumes", Json::object()}};
        for (const auto& source : request.Sources)
        {
            sourceId = source.Id;
            const auto documents = YAML::LoadAll(source.Content);
            if (documents.size() != 1)
            {
                throw ParseError{"invalid_yaml", "Exactly one YAML document is required"};
            }
            const auto prepared = interpreter.Prepare(reader.Expand(documents[0]));
            for (const auto& [name, service] : prepared["services"].items())
            {
                if (!merged["services"].contains(name))
                {
                    merged["services"][name] = Json::object();
                }
                Interpreter::MergeService(merged["services"][name], service);
            }
            if (prepared.contains("volumes") && prepared["volumes"].is_object())
            {
                merged["volumes"].update(prepared["volumes"]);
            }
        }
        sourceId.clear();
        result.AcceptedProject = interpreter.ProjectModel(merged);
        if (EncodeResult(result).dump().size() > c_maxResponseBytes)
        {
            return Failure("limit_exceeded", "Response exceeds 16 MiB");
        }
        return result;
    }
    catch (const ParseError& error)
    {
        return Failure(error.Code, error.what(), sourceId, error.Path);
    }
    catch (const YAML::Exception& error)
    {
        return Failure("invalid_yaml", error.what(), sourceId);
    }
    catch (const nlohmann::json::type_error& error)
    {
        return Failure("invalid_request", error.what(), sourceId);
    }
    catch (const std::exception& error)
    {
        return Failure("internal_error", error.what(), sourceId);
    }
}

} // namespace wsl::windows::compose
