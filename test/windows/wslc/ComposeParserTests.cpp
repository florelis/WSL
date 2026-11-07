// Copyright (C) Microsoft Corporation. All rights reserved.
#include "precomp.h"
#include "Common.h"
#include "ComposeParser.hpp"
#include "private/Protocol.hpp"
#include <future>

namespace ComposeTests {
using namespace wsl::windows::compose;

namespace {

    Request Input(std::string content)
    {
        Request request;
        request.ProjectName = "demo";
        request.ProjectDirectory = "C:\\captured\\project";
        request.Sources.push_back({"base", "Z:\\does-not-exist\\compose.yaml", std::move(content)});
        return request;
    }

    Project Success(const Request& request)
    {
        auto result = Normalize(request);
        for (const auto& diagnostic : result.Diagnostics)
        {
            LogInfo("%s: %s", diagnostic.Code.c_str(), diagnostic.Message.c_str());
        }
        VERIFY_IS_TRUE(result.AcceptedProject.has_value());
        return std::move(result.AcceptedProject.value());
    }

    void Error(const Request& request, std::string_view code)
    {
        const auto result = Normalize(request);
        VERIFY_IS_FALSE(result.AcceptedProject.has_value());
        VERIFY_IS_FALSE(result.Diagnostics.empty());
        VERIFY_IS_TRUE(result.Diagnostics.at(0).Code == code);
    }

} // namespace

class ComposeParserTests
{
    WSLC_TEST_CLASS(ComposeParserTests)

    TEST_METHOD(CapturedProject)
    {
        auto request = Input(R"(name: ignored
services:
  db:
    image: postgres:16
    volumes: ['data:/var/lib/data']
  web:
    image: '${IMAGE:-nginx:alpine}'
    command: [serve, '--port', '80']
    entrypoint: null
    environment: {MODE: production, TOKEN: null, MISSING: null}
    labels: [owner=team, bare]
    ports: ['127.0.0.1:8080:80']
    volumes: ['./html:/srv:ro']
    depends_on: [db]
volumes: {data: {}}
)");
        request.Environment = {{"TOKEN", "captured"}};
        const auto project = Success(request);
        VERIFY_IS_TRUE(project.Name == "demo" && project.Directory == request.ProjectDirectory);
        VERIFY_IS_TRUE(project.DefaultNetworkName == "demo_default");
        VERIFY_IS_TRUE((project.Volumes == std::map<std::string, std::string>{{"data", "demo_data"}}));
        VERIFY_ARE_EQUAL(size_t{2}, project.Services.size());
        const auto& db = project.Services.at("db");
        VERIFY_IS_TRUE(db.Image == "postgres:16" && db.Enabled);
        VERIFY_IS_TRUE(db.CommandLine.State == CommandState::Absent && db.Entrypoint.State == CommandState::Absent);
        VERIFY_IS_TRUE(db.CommandLine.Arguments.empty() && db.Entrypoint.Arguments.empty());
        VERIFY_IS_TRUE(db.Environment.empty() && db.Labels.empty() && db.Ports.empty());
        VERIFY_IS_TRUE(db.Dependencies.empty() && db.Profiles.empty());
        VERIFY_ARE_EQUAL(size_t{1}, db.Mounts.size());
        VERIFY_IS_TRUE(db.Mounts[0].Type == "volume" && db.Mounts[0].Source == "data");
        VERIFY_IS_TRUE(db.Mounts[0].Target == "/var/lib/data" && !db.Mounts[0].ReadOnly && !db.Mounts[0].CreateHostPath);
        const auto& web = project.Services.at("web");
        VERIFY_IS_TRUE(web.Image == "nginx:alpine" && web.Enabled && web.Profiles.empty());
        VERIFY_IS_TRUE(web.CommandLine.State == CommandState::Value && web.Entrypoint.State == CommandState::Null);
        VERIFY_IS_TRUE((web.CommandLine.Arguments == std::vector<std::string>{"serve", "--port", "80"}));
        VERIFY_IS_TRUE(web.Entrypoint.Arguments.empty());
        VERIFY_IS_TRUE((web.Environment == std::map<std::string, std::string>{{"MODE", "production"}, {"TOKEN", "captured"}}));
        VERIFY_IS_TRUE((web.Labels == std::map<std::string, std::string>{{"owner", "team"}, {"bare", ""}}));
        VERIFY_IS_TRUE((web.Dependencies == std::vector<std::string>{"db"}));
        VERIFY_ARE_EQUAL(size_t{1}, web.Ports.size());
        VERIFY_IS_TRUE(web.Ports[0].Target == 80 && web.Ports[0].Published == 8080);
        VERIFY_IS_TRUE(web.Ports[0].HostIp == "127.0.0.1" && web.Ports[0].Protocol == "tcp");
        VERIFY_ARE_EQUAL(size_t{1}, web.Mounts.size());
        VERIFY_IS_TRUE(web.Mounts[0].Type == "bind" && web.Mounts[0].Source == "C:\\captured\\project\\html");
        VERIFY_IS_TRUE(web.Mounts[0].Target == "/srv" && web.Mounts[0].ReadOnly && web.Mounts[0].CreateHostPath);
    }

    TEST_METHOD(MergeAndNull)
    {
        auto request = Input(R"(services:
  app:
    image: alpine
    command: [old]
    entrypoint: [old]
    environment: {A: base, B: keep}
    labels: [a=base]
    ports: ["8080:80"]
    volumes: ["./old:/data", "/cache"]
    profiles: [one]
)");
        request.Profiles = {"*"};
        request.Sources.push_back({"override", "", R"(services:
  app:
    command: []
    entrypoint: null
    environment: [A=override, C=new]
    labels: {a: override, b: new}
    ports: [{target: 80, published: "8080"}, "9090:90/udp"]
    volumes: [{type: bind, source: ./new, target: /data}]
    profiles: [two]
)"});
        const auto service = Success(request).Services.at("app");
        VERIFY_IS_TRUE(service.CommandLine.State == CommandState::Value && service.CommandLine.Arguments.empty());
        VERIFY_IS_TRUE(service.Entrypoint.State == CommandState::Null);
        VERIFY_IS_TRUE(
            (service.Environment == std::map<std::string, std::string>{{"A", "override"}, {"B", "keep"}, {"C", "new"}}));
        VERIFY_IS_TRUE((service.Labels == std::map<std::string, std::string>{{"a", "override"}, {"b", "new"}}));
        VERIFY_IS_TRUE(service.Ports.size() == 2 && service.Ports[1].Protocol == "udp");
        VERIFY_IS_TRUE(service.Mounts.size() == 2 && service.Mounts[0].Source == "C:\\captured\\project\\new");
        VERIFY_IS_FALSE(service.Mounts[0].CreateHostPath);
        VERIFY_IS_TRUE((service.Profiles == std::vector<std::string>{"one", "two"}));
    }

    TEST_METHOD(Interpolation)
    {
        auto request = Input(R"(services:
  app:
    image: "${IMAGE:-alpine}"
    command: ["$SET", "${SET}", "${EMPTY:-fallback}", "${EMPTY-fallback}", "${UNSET-fallback}", "${SET:+yes}", "${EMPTY:+no}", "${EMPTY+yes}", "${UNSET+no}", "${UNSET:-${SET}}", "$$SET", "${SET:?required}", "${EMPTY?allowed}"]
    environment: {UNSET: null, EMPTY: null, SET: null, COMPOSE_PROJECT_NAME: null}
)");
        request.Environment = {{"SET", "value"}, {"EMPTY", ""}};
        const auto service = Success(request).Services.at("app");
        VERIFY_IS_TRUE(
            (service.CommandLine.Arguments ==
             std::vector<std::string>{
                 "value", "value", "fallback", "", "fallback", "yes", "", "yes", "", "value", "$SET", "value", ""}));
        VERIFY_IS_TRUE(
            (service.Environment ==
             std::map<std::string, std::string>{{"EMPTY", ""}, {"SET", "value"}, {"COMPOSE_PROJECT_NAME", "demo"}}));
        Error(Input("services: {app: {image: '${MISSING:?required}'}}"), "invalid_compose");
        Error(Input("services: {app: {image: '${EMPTY?required}'}}"), "invalid_compose");
        Error(Input("services: {app: {image: '${BROKEN'}}"), "invalid_compose");
        const auto warning = Normalize(Input("services: {app: {image: alpine, command: ['$MISSING']}}"));
        VERIFY_IS_TRUE(warning.AcceptedProject && warning.Diagnostics.size() == 1 && warning.Diagnostics[0].Severity == "warning");
    }

    TEST_METHOD(LiteralDollars)
    {
        const auto service =
            Success(Input("services: {app: {image: alpine, command: ['cost $5', '$', '$-x', '$$HOME']}}")).Services.at("app");
        VERIFY_IS_TRUE((service.CommandLine.Arguments == std::vector<std::string>{"cost $5", "$", "$-x", "$HOME"}));
        Error(Input("services: {app: {image: alpine, command: ['${5}']}}"), "invalid_compose");
    }

    TEST_METHOD(InterpolationBeforeMerge)
    {
        auto request = Input("services: {app: {image: alpine, command: ['${MISSING:?must-fail}']}}");
        request.Sources.push_back({"override", "", "services: {app: {command: [okay]}}"});
        Error(request, "invalid_compose");
    }

    TEST_METHOD(YamlMergesAndTypes)
    {
        const auto service = Success(Input(R"(services:
  a: &a
    image: alpine
    labels: {owner: first}
  b: &b
    image: busybox
  app:
    <<: [*a, *b]
    image: nginx
    environment: {BOOL: true, NUMBER: 42, QUOTED: "42", YES: yes}
)"))
                                 .Services.at("app");
        VERIFY_IS_TRUE(service.Image == "nginx" && service.Labels.at("owner") == "first");
        VERIFY_IS_TRUE(
            (service.Environment ==
             std::map<std::string, std::string>{{"BOOL", "true"}, {"NUMBER", "42"}, {"QUOTED", "42"}, {"YES", "yes"}}));
        Error(Input("services: {app: {image: 123}}"), "invalid_compose");
        Error(Input("services: {app: {image: alpine, image: busybox}}"), "invalid_yaml");
        Error(Input("services: {app: {image: alpine, environment: {1: value}}}"), "invalid_yaml");
        Error(Input("services: &cycle {app: *cycle}"), "invalid_yaml");
        Error(Input("services: {app: {image: !custom alpine}}"), "unsupported_feature");
        Error(Input("services: {app: {image: alpine}}\n---\nservices: {}"), "invalid_yaml");
    }

    TEST_METHOD(ProfilesAndDependencies)
    {
        auto request = Input("services: {app: {image: alpine}, debug: {image: alpine, profiles: [debug]}}");
        VERIFY_IS_FALSE(Success(request).Services.at("debug").Enabled);
        request.Profiles = {"debug"};
        VERIFY_IS_TRUE(Success(request).Services.at("debug").Enabled);
        request.Profiles = {"*"};
        VERIFY_IS_TRUE(Success(request).Services.at("debug").Enabled);
        Error(Input("services: {app: {image: alpine, depends_on: [missing]}}"), "invalid_compose");
        Error(
            Input("services: {app: {image: alpine, depends_on: [other]}, other: {image: alpine, depends_on: [app]}}"),
            "invalid_compose");
        Error(
            Input("services: {app: {image: alpine, depends_on: [debug]}, debug: {image: alpine, profiles: [debug]}}"),
            "invalid_compose");
        Error(Input("services: {debug: {profiles: [debug]}}"), "invalid_compose");
        const auto service = Success(Input("services: {a: {image: alpine}, b: {image: alpine, depends_on: {a: {condition: "
                                           "service_started, required: true, restart: false}}}}"))
                                 .Services.at("b");
        VERIFY_IS_TRUE((service.Dependencies == std::vector<std::string>{"a"}));
    }

    TEST_METHOD(NumericAndExplicitScalars)
    {
        const auto service =
            Success(Input("services: {app: {image: alpine, environment: {FLOAT: 1.0, OCT: 0o12, HEX: 0xff, QUOTED: !!str 123}}}"))
                .Services.at("app");
        VERIFY_IS_TRUE(
            (service.Environment ==
             std::map<std::string, std::string>{{"FLOAT", "1"}, {"OCT", "10"}, {"HEX", "255"}, {"QUOTED", "123"}}));
        Error(Input("services: {app: {image: alpine, environment: {BAD: !!int true}}}"), "invalid_yaml");
        Error(Input("services: {app: {image: alpine, environment: {BAD: !!bool 1}}}"), "invalid_yaml");
        const auto extremes =
            Success(Input("services: {app: {image: _1, environment: {TEXT: _123, LARGE: 18446744073709551615}}}"))
                .Services.at("app");
        VERIFY_IS_TRUE(extremes.Image == "_1");
        VERIFY_IS_TRUE(
            (extremes.Environment == std::map<std::string, std::string>{{"TEXT", "_123"}, {"LARGE", "18446744073709551615"}}));
    }

    TEST_METHOD(NullOverrides)
    {
        auto request = Input(
            "services: {app: {image: alpine, environment: {A: value}, labels: {a: value}, ports: ['80'], volumes: ['/data']}}");
        request.Sources.push_back(
            {"override", "", "services: {app: {environment: null, labels: null, ports: null, volumes: null}}"});
        const auto service = Success(request).Services.at("app");
        VERIFY_IS_TRUE(service.Environment.at("A") == "value" && service.Labels.at("a") == "value");
        VERIFY_IS_TRUE(service.Ports.size() == 1 && service.Mounts.size() == 1);
    }

    TEST_METHOD(CapturedCaseSensitiveEnvironment)
    {
        auto request = Input("services: {app: {image: '${image:-lowercase}', environment: {Image: null, IMAGE: null}}}");
        request.Environment = {{"IMAGE", "uppercase"}, {"Image", "mixed"}};
        const auto service = Success(request).Services.at("app");
        VERIFY_IS_TRUE(service.Image == "lowercase");
        VERIFY_IS_TRUE((service.Environment == std::map<std::string, std::string>{{"IMAGE", "uppercase"}, {"Image", "mixed"}}));
    }

    TEST_METHOD(UnicodeInputAndPath)
    {
        auto request = Input("services: {app: {image: alpine, command: [\"\\u263a\"], environment: {TEXT: null}}}");
        request.ProjectDirectory = "C:\\captured\\caf\xc3\xa9";
        request.Environment["TEXT"] = "caf\xc3\xa9";
        const auto project = Success(request);
        VERIFY_IS_TRUE(project.Directory == request.ProjectDirectory);
        VERIFY_IS_TRUE(project.Services.at("app").Environment.at("TEXT") == request.Environment["TEXT"]);
        VERIFY_IS_TRUE(project.Services.at("app").CommandLine.Arguments.at(0) == "\xe2\x98\xba");
    }

    TEST_METHOD(PathsAndMountDefaults)
    {
        const auto service = Success(Input(R"(services:
  app:
    image: alpine
    volumes:
      - "./one/../two:/data:ro"
      - {type: bind, source: 'D:\absolute\..\data', target: /other}
      - {type: bind, source: '\\server\share\data', target: /unc, bind: {create_host_path: true}}
      - "/anonymous"
)"))
                                 .Services.at("app");
        VERIFY_ARE_EQUAL(size_t{4}, service.Mounts.size());
        VERIFY_IS_TRUE(service.Mounts[0].Source == "C:\\captured\\project\\two" && service.Mounts[0].ReadOnly && service.Mounts[0].CreateHostPath);
        VERIFY_IS_TRUE(service.Mounts[1].Source == "D:\\data" && !service.Mounts[1].CreateHostPath);
        VERIFY_IS_TRUE(service.Mounts[2].Source == "\\\\server\\share\\data");
        VERIFY_IS_TRUE(service.Mounts[3].Type == "volume" && service.Mounts[3].Source.empty());
        for (const auto* path : {"~/data", "/rooted", "C:relative", "\\rooted"})
        {
            Error(
                Input("services: {app: {image: alpine, volumes: [{type: bind, source: '" + std::string{path} + "', target: /data}]}}"),
                "unsupported_feature");
        }
        Error(Input("services: {app: {image: alpine, volumes: ['C:\\data:/data']}}"), "unsupported_feature");
        Error(Input("services: {app: {image: alpine, volumes: ['missing:/data']}}"), "invalid_compose");
    }

    TEST_METHOD(UnsupportedInEverySource)
    {
        for (const auto* field :
             {"build",
              "include",
              "extends",
              "env_file",
              "label_file",
              "configs",
              "secrets",
              "healthcheck",
              "deploy",
              "develop",
              "networks",
              "x-hidden"})
        {
            auto request = Input("services: {app: {image: alpine, profiles: [inactive], " + std::string{field} + ": ignored}}");
            request.Sources.push_back({"override", "", "services: {app: {image: busybox}}"});
            Error(request, "unsupported_feature");
        }
        Error(Input("include: missing.yaml\nservices: {}"), "unsupported_feature");
        Error(Input("services: {app: {image: alpine, command: 'echo hello'}}"), "unsupported_feature");
        Error(Input("services: {app: {image: alpine, ports: ['8000-8002:80']}}"), "unsupported_feature");
        Error(Input("services: {app: {image: alpine, ports: ['[::1]:8080:80']}}"), "unsupported_feature");
        Error(Input("services: {app: {image: alpine, volumes: ['./data:/data:cached']}}"), "unsupported_feature");
        Error(Input("services: {app: {image: alpine, depends_on: {other: {condition: service_healthy}}}}"), "unsupported_feature");
    }

    TEST_METHOD(RequestValidation)
    {
        auto request = Input("services: {app: {image: alpine}}");
        request.ProjectDirectory = "relative";
        Error(request, "invalid_request");
        request.ProjectDirectory = "\\\\server\\";
        Error(request, "invalid_request");
        request.ProjectDirectory = "C:\\project";
        request.Sources.push_back(request.Sources[0]);
        Error(request, "invalid_request");
        request.Sources.clear();
        Error(request, "invalid_request");
        request = Input("services: {app: {image: alpine}}");
        request.Environment["BAD"] = std::string(1, static_cast<char>(0xff));
        Error(request, "invalid_request");
    }

    TEST_METHOD(RequestByteBoundary)
    {
        auto request = Input("services: {app: {image: alpine}}\n#");
        request.Sources[0].Content.append(c_maxRequestBytes - EncodeRequest(request).dump().size(), 'a');
        Success(request);
        request.Sources[0].Content.push_back('a');
        Error(request, "limit_exceeded");
    }

    TEST_METHOD(YamlDepthBoundary)
    {
        const std::string prefix = "services: {app: {image: alpine, unknown: ";
        Error(Input(prefix + std::string(60, '[') + "x" + std::string(60, ']') + "}}"), "unsupported_feature");
        Error(Input(prefix + std::string(61, '[') + "x" + std::string(61, ']') + "}}"), "limit_exceeded");
    }

    TEST_METHOD(YamlNodeBoundary)
    {
        std::string yaml = "unknown: [";
        for (size_t i = 0; i < c_maxNodes - 3; ++i)
        {
            yaml += i == 0 ? "x" : ",x";
        }
        yaml += "]";
        Error(Input(yaml), "unsupported_feature");
        yaml.insert(yaml.size() - 1, ",x");
        Error(Input(yaml), "limit_exceeded");
    }

    TEST_METHOD(ConcurrentCapturedEnvironments)
    {
        std::vector<std::future<Result>> calls;
        for (int i = 0; i < 6; ++i)
        {
            calls.push_back(std::async(std::launch::async, [i] {
                auto request = Input("services: {app: {image: '${IMAGE}'}}");
                request.Environment["IMAGE"] = "image" + std::to_string(i);
                return Normalize(request);
            }));
        }
        for (size_t i = 0; i < calls.size(); ++i)
        {
            const auto result = calls[i].get();
            VERIFY_IS_TRUE(result.AcceptedProject.has_value());
            VERIFY_IS_TRUE(result.AcceptedProject->Services.at("app").Image == "image" + std::to_string(i));
        }
    }

    TEST_METHOD(SingleSourceUniqueness)
    {
        auto request = Input(
            "services: {app: {image: alpine, ports: ['80', {target: 80}], volumes: ['/data', '/data'], profiles: [one, one]}}");
        request.Profiles = {"*"};
        const auto service = Success(request).Services.at("app");
        VERIFY_IS_TRUE(service.Ports.size() == 1 && service.Mounts.size() == 1 && service.Profiles.size() == 1);
    }

    TEST_METHOD(YamlAliasExpansionLimit)
    {
        std::string yaml = "a: &a [x,x,x,x,x,x,x,x,x,x]\n";
        for (char key = 'b'; key <= 'e'; ++key)
        {
            yaml += std::string{key} + ": &" + key + " [";
            for (int i = 0; i < 10; ++i)
            {
                yaml += (i == 0 ? "*" : ",*") + std::string(1, key - 1);
            }
            yaml += "]\n";
        }
        Error(Input(yaml), "limit_exceeded");
    }

    TEST_METHOD(ResponseByteBoundary)
    {
        std::string yaml = "services: {app: {image: alpine, command: [";
        for (int i = 0; i < 16; ++i)
        {
            yaml += "'$PAYLOAD',";
        }
        yaml += "'$PAD']}}";
        auto request = Input(yaml);
        request.Environment = {{"PAYLOAD", ""}, {"PAD", ""}};
        const auto baseline = Normalize(request);
        VERIFY_IS_TRUE(baseline.AcceptedProject.has_value());
        const auto remaining = c_maxResponseBytes - EncodeResult(baseline).dump().size();
        request.Environment["PAYLOAD"].assign(remaining / 16, 'x');
        request.Environment["PAD"].assign(remaining % 16, 'x');
        const auto exact = Normalize(request);
        VERIFY_IS_TRUE(exact.AcceptedProject.has_value());
        VERIFY_ARE_EQUAL(c_maxResponseBytes, EncodeResult(exact).dump().size());
        request.Environment["PAD"].push_back('x');
        Error(request, "limit_exceeded");
    }

    TEST_METHOD(PrivateProtocolValidation)
    {
        VERIFY_THROWS(ParseJson(R"({"version":1,"version":1})"), std::invalid_argument);
        const auto result = Normalize(Input("services: {app: {image: alpine}}"));
        VERIFY_IS_TRUE(result.AcceptedProject.has_value());
        const auto encoded = EncodeResult(result);
        VERIFY_IS_TRUE(EncodeResult(DecodeResult(encoded)) == encoded);
    }
};
} // namespace ComposeTests
