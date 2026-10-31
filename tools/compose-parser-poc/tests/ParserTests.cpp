// Copyright (C) Microsoft Corporation. All rights reserved.
#include "precomp.h"
#include <fstream>
#include <future>
#include <functional>
#include <sstream>

using namespace wsl::windows::compose;
using Json = nlohmann::json;

namespace {

ParserOptions g_options;
size_t g_failures{};

void Check(bool condition, const std::string& message)
{
    if (!condition)
    {
        throw std::runtime_error(message);
    }
}

Request Input(std::string content)
{
    Request request;
    request.ProjectName = "demo";
    request.ProjectDirectory = "C:\\captured\\project";
    request.Sources.push_back({"base", "Z:\\does-not-exist\\compose.yaml", std::move(content)});
    return request;
}

Json Success(const Request& request)
{
    const auto result = Normalize(request, g_options);
    Check(result.AcceptedProject.has_value(), EncodeResult(result).dump());
    const auto encoded = EncodeResult(result);
    Check(EncodeResult(DecodeResult(encoded)) == encoded, "Protocol roundtrip differs");
    return encoded["project"];
}

void Error(const Request& request, const std::string& code)
{
    const auto result = Normalize(request, g_options);
    Check(!result.AcceptedProject && !result.Diagnostics.empty(), "Expected failure: " + code);
    Check(result.Diagnostics[0].Code == code, "Expected " + code + ", got " + EncodeResult(result).dump());
}

void Test(const std::string& name, const std::function<void()>& run)
{
    try
    {
        run();
        std::cout << "PASS " << name << '\n';
    }
    catch (const std::exception& error)
    {
        ++g_failures;
        std::cerr << "FAIL " << name << ": " << error.what() << '\n';
    }
}

void Golden()
{
    std::ifstream input{std::filesystem::path{COMPOSE_FIXTURES} / "basic.json", std::ios::binary};
    Check(input.good(), "Missing golden fixture");
    const auto fixture = Json::parse(input);
    const auto project = Success(DecodeRequest(fixture["request"]));
    Check(project == fixture["project"], "Golden mismatch:\n" + project.dump(2));
}

void Semantics()
{
    Test("golden", Golden);
    Test("merge-and-null", [] {
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
        const auto service = Success(request)["services"]["app"];
        Check(service["command"] == Json{{"state", "value"}, {"arguments", Json::array()}}, "Empty command");
        Check(service["entrypoint"]["state"] == "null", "Null override");
        Check(service["environment"] == Json{{"A", "override"}, {"B", "keep"}, {"C", "new"}}, "Environment merge");
        Check(service["labels"] == Json{{"a", "override"}, {"b", "new"}}, "Label merge");
        Check(service["ports"].size() == 2 && service["ports"][1]["protocol"] == "udp", "Port uniqueness");
        Check(
            service["mounts"].size() == 2 && service["mounts"][0]["source"] == "C:\\captured\\project\\new" &&
                service["mounts"][0]["create_host_path"] == false,
            "Mount replacement");
        Check(service["profiles"] == Json::array({"one", "two"}), "Profile union");
    });
    Test("interpolation", [] {
        auto request = Input(R"(services:
  app:
    image: "${IMAGE:-alpine}"
    command: ["$SET", "${SET}", "${EMPTY:-fallback}", "${EMPTY-fallback}", "${UNSET-fallback}", "${SET:+yes}", "${EMPTY:+no}", "${EMPTY+yes}", "${UNSET+no}", "${UNSET:-${SET}}", "$$SET", "${SET:?required}", "${EMPTY?allowed}"]
    environment: {UNSET: null, EMPTY: null, SET: null, COMPOSE_PROJECT_NAME: null}
)");
        request.Environment = {{"SET", "value"}, {"EMPTY", ""}};
        const auto service = Success(request)["services"]["app"];
        Check(
            service["command"]["arguments"] ==
                Json::array({"value", "value", "fallback", "", "fallback", "yes", "", "yes", "", "value", "$SET", "value", ""}),
            "Interpolation operators");
        Check(
            service["environment"] == Json{{"EMPTY", ""}, {"SET", "value"}, {"COMPOSE_PROJECT_NAME", "demo"}},
            "Environment lookup");
        auto missing = Input("services: {app: {image: '${MISSING:?required}'}}");
        Error(missing, "invalid_compose");
        Error(Input("services: {app: {image: '${EMPTY?required}'}}"), "invalid_compose");
        Error(Input("services: {app: {image: '${BROKEN'}}"), "invalid_compose");
        auto warning = Normalize(Input("services: {app: {image: alpine, command: ['$MISSING']}}"), g_options);
        Check(
            warning.AcceptedProject && warning.Diagnostics.size() == 1 && warning.Diagnostics[0].Severity == "warning",
            "Undefined warning");
    });
    Test("interpolation-before-merge", [] {
        auto request = Input("services: {app: {image: alpine, command: ['${MISSING:?must-fail}']}}");
        request.Sources.push_back({"override", "", "services: {app: {command: [okay]}}"});
        Error(request, "invalid_compose");
    });
    Test("yaml-merges-and-types", [] {
        const auto project = Success(Input(R"(services:
  a: &a
    image: alpine
    labels: {owner: first}
  b: &b
    image: busybox
  app:
    <<: [*a, *b]
    image: nginx
    environment: {BOOL: true, NUMBER: 42, QUOTED: "42", YES: yes}
)"));
        Check(project["services"]["app"]["image"] == "nginx", "Explicit merge precedence");
        Check(project["services"]["app"]["labels"]["owner"] == "first", "Merge sequence");
        Check(
            project["services"]["app"]["environment"] ==
                Json{{"BOOL", "true"}, {"NUMBER", "42"}, {"QUOTED", "42"}, {"YES", "yes"}},
            "YAML scalar types");
        Error(Input("services: {app: {image: 123}}"), "invalid_compose");
        Error(Input("services: {app: {image: alpine, image: busybox}}"), "invalid_yaml");
        Error(Input("services: {app: {image: alpine, environment: {1: value}}}"), "invalid_yaml");
        Error(Input("services: &cycle {app: *cycle}"), "invalid_yaml");
        Error(Input("services: {app: {image: !custom alpine}}"), "unsupported_feature");
        Error(Input("services: {app: {image: alpine}}\n---\nservices: {}"), "invalid_yaml");
    });
    Test("profiles-and-dependencies", [] {
        auto request = Input("services: {app: {image: alpine}, debug: {image: alpine, profiles: [debug]}}");
        Check(Success(request)["services"]["debug"]["enabled"] == false, "Inactive profile");
        request.Profiles = {"debug"};
        Check(Success(request)["services"]["debug"]["enabled"] == true, "Selected profile");
        request.Profiles = {"*"};
        Check(Success(request)["services"]["debug"]["enabled"] == true, "Wildcard profile");
        Error(Input("services: {app: {image: alpine, depends_on: [missing]}}"), "invalid_compose");
        Error(
            Input("services: {app: {image: alpine, depends_on: [other]}, other: {image: alpine, depends_on: [app]}}"),
            "invalid_compose");
        Error(
            Input("services: {app: {image: alpine, depends_on: [debug]}, debug: {image: alpine, profiles: [debug]}}"),
            "invalid_compose");
        Error(Input("services: {debug: {profiles: [debug]}}"), "invalid_compose");
        const auto service =
            Success(Input("services: {a: {image: alpine}, b: {image: alpine, depends_on: {a: {condition: service_started, required: true, restart: false}}}}"))
                ["services"]["b"];
        Check(service["dependencies"] == Json::array({"a"}), "Long dependencies");
    });
    Test("numeric-and-explicit-scalars", [] {
        const auto service = Success(Input(
            "services: {app: {image: alpine, environment: {FLOAT: 1.0, OCT: 0o12, HEX: 0xff, QUOTED: !!str 123}}}"))["services"]
                                                                                                                    ["app"];
        Check(
            service["environment"] == Json{{"FLOAT", "1"}, {"OCT", "10"}, {"HEX", "255"}, {"QUOTED", "123"}},
            "Numeric normalization: " + service["environment"].dump());
        Error(Input("services: {app: {image: alpine, environment: {BAD: !!int true}}}"), "invalid_yaml");
        Error(Input("services: {app: {image: alpine, environment: {BAD: !!bool 1}}}"), "invalid_yaml");
        const auto extremes =
            Success(Input("services: {app: {image: _1, environment: {TEXT: _123, LARGE: 18446744073709551615}}}"));
        Check(
            extremes["services"]["app"]["image"] == "_1" &&
                extremes["services"]["app"]["environment"] == Json{{"TEXT", "_123"}, {"LARGE", "18446744073709551615"}},
            "Scalar evidence preserved");
    });
    Test("null-and-empty-overrides", [] {
        auto request = Input(
            "services: {app: {image: alpine, environment: {A: value}, labels: {a: value}, ports: ['80'], volumes: ['/data']}}");
        request.Sources.push_back(
            {"override", "", "services: {app: {environment: null, labels: null, ports: null, volumes: null}}"});
        const auto service = Success(request)["services"]["app"];
        Check(
            service["environment"]["A"] == "value" && service["labels"]["a"] == "value" && service["ports"].size() == 1 &&
                service["mounts"].size() == 1,
            "Null field merge");
    });
    Test("captured-case-sensitive-environment", [] {
        auto request = Input("services: {app: {image: '${image:-lowercase}', environment: {Image: null, IMAGE: null}}}");
        request.Environment = {{"IMAGE", "uppercase"}, {"Image", "mixed"}};
        const auto service = Success(request)["services"]["app"];
        Check(
            service["image"] == "lowercase" && service["environment"] == Json{{"IMAGE", "uppercase"}, {"Image", "mixed"}},
            "Case-sensitive lookup");
    });
    Test("unicode-input-and-path", [] {
        auto request = Input("services: {app: {image: alpine, command: [\"\\u263a\"], environment: {TEXT: null}}}");
        request.ProjectDirectory = "C:\\captured\\caf\u00e9";
        request.Environment["TEXT"] = "caf\u00e9";
        const auto project = Success(request);
        Check(
            project["directory"] == request.ProjectDirectory && project["services"]["app"]["environment"]["TEXT"] == "caf\u00e9" &&
                project["services"]["app"]["command"]["arguments"][0] == "\u263a",
            "Unicode roundtrip");
    });
    Test("paths-and-mount-defaults", [] {
        const auto service = Success(Input(R"(services:
  app:
    image: alpine
    volumes:
      - "./one/../two:/data:ro"
      - {type: bind, source: 'D:\absolute\..\data', target: /other}
      - {type: bind, source: '\\server\share\data', target: /unc, bind: {create_host_path: true}}
      - "/anonymous"
)"))["services"]["app"];
        Check(
            service["mounts"][0]["source"] == "C:\\captured\\project\\two" && service["mounts"][0]["read_only"] == true &&
                service["mounts"][0]["create_host_path"] == true,
            "Relative short bind");
        Check(
            service["mounts"][1]["source"] == "D:\\data" && service["mounts"][1]["create_host_path"] == false,
            "Absolute long bind");
        Check(service["mounts"][2]["source"] == "\\\\server\\share\\data", "UNC bind");
        Check(service["mounts"][3]["type"] == "volume" && service["mounts"][3]["source"] == "", "Anonymous mount");
        for (const auto& path : {"~/data", "/rooted", "C:relative", "\\rooted"})
        {
            Error(
                Input("services: {app: {image: alpine, volumes: [{type: bind, source: '" + std::string{path} + "', target: /data}]}}"),
                "unsupported_feature");
        }
        Error(Input("services: {app: {image: alpine, volumes: ['C:\\data:/data']}}"), "unsupported_feature");
        Error(Input("services: {app: {image: alpine, volumes: ['missing:/data']}}"), "invalid_compose");
    });
    Test("unsupported-in-every-source", [] {
        for (const auto& field :
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
    });
    Test("request-and-json-validation", [] {
        auto request = Input("services: {app: {image: alpine}}");
        request.ProjectDirectory = "relative";
        Error(request, "invalid_request");
        request.ProjectDirectory = "\\\\server\\";
        Error(request, "invalid_request");
        request.ProjectDirectory = "C:\\project";
        request.Sources.push_back(request.Sources[0]);
        Error(request, "invalid_request");
        bool threw = false;
        try
        {
            ParseJson(R"({"version":1,"version":1})");
        }
        catch (const std::exception&)
        {
            threw = true;
        }
        Check(threw, "Duplicate JSON accepted");
    });
    Test("request-byte-boundary", [] {
        auto request = Input("services: {app: {image: alpine}}\n#");
        const auto size = EncodeRequest(request).dump().size();
        request.Sources[0].Content.append(c_maxRequestBytes - size, 'a');
        Success(request);
        request.Sources[0].Content.push_back('a');
        Error(request, "limit_exceeded");
    });
    Test("yaml-depth-boundary", [] {
        // The nested array leaf is depth 64; the unsupported field must be
        // reported only after structural admission succeeds.
        const std::string prefix = "services: {app: {image: alpine, unknown: ";
        Error(Input(prefix + std::string(60, '[') + "x" + std::string(60, ']') + "}}"), "unsupported_feature");
        Error(Input(prefix + std::string(61, '[') + "x" + std::string(61, ']') + "}}"), "limit_exceeded");
    });
    Test("yaml-node-boundary", [] {
        // Root/map key/sequence plus leaves: exactly 100,000 visits.
        std::string yaml = "unknown: [";
        for (size_t i = 0; i < c_maxNodes - 3; ++i)
        {
            yaml += i == 0 ? "x" : ",x";
        }
        yaml += "]";
        Error(Input(yaml), "unsupported_feature");
        yaml.insert(yaml.size() - 1, ",x");
        Error(Input(yaml), "limit_exceeded");
    });
    Test("concurrent-captured-environments", [] {
        std::vector<std::future<void>> calls;
        for (int i = 0; i < 6; ++i)
        {
            calls.push_back(std::async(std::launch::async, [i] {
                auto request = Input("services: {app: {image: '${IMAGE}'}}");
                request.Environment["IMAGE"] = "image" + std::to_string(i);
                Check(Success(request)["services"]["app"]["image"] == request.Environment["IMAGE"], "Cross-request state leak");
            }));
        }
        for (auto& call : calls)
        {
            call.get();
        }
    });
    Test("single-source-uniqueness", [] {
        auto request = Input(
            "services: {app: {image: alpine, ports: ['80', {target: 80}], volumes: ['/data', '/data'], profiles: [one, one]}}");
        request.Profiles = {"*"};
        const auto service = Success(request)["services"]["app"];
        Check(
            service["ports"].size() == 1 && service["mounts"].size() == 1 && service["profiles"].size() == 1,
            "Single-source uniqueness");
    });
    Test("yaml-alias-expansion-limit", [] {
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
    });
    Test("response-byte-boundary", [] {
        std::string yaml = "services: {app: {image: alpine, command: [";
        for (int i = 0; i < 16; ++i)
        {
            yaml += "'$PAYLOAD',";
        }
        yaml += "'$PAD']}}";
        auto request = Input(yaml);
        request.Environment = {{"PAYLOAD", ""}, {"PAD", ""}};
        const auto baseline = Normalize(request, g_options);
        Check(baseline.AcceptedProject.has_value(), EncodeResult(baseline).dump());
        const auto remaining = c_maxResponseBytes - EncodeResult(baseline).dump().size();
        request.Environment["PAYLOAD"].assign(remaining / 16, 'x');
        request.Environment["PAD"].assign(remaining % 16, 'x');
        const auto exact = Normalize(request, g_options);
        Check(
            exact.AcceptedProject && EncodeResult(exact).dump().size() == c_maxResponseBytes,
            "Exact response limit: " +
                (exact.AcceptedProject ? std::to_string(EncodeResult(exact).dump().size()) : EncodeResult(exact).dump()));
        request.Environment["PAD"].push_back('x');
        Error(request, "limit_exceeded");
    });
}

} // namespace

int wmain(int argc, wchar_t** argv)
{
    if (argc > 1)
    {
        g_options.LibraryPath = argv[1];
    }
    Semantics();
    return g_failures == 0 ? 0 : 1;
}
