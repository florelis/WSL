# compose-go worker prototype

`Normalize(Request, ParserOptions)` is a reusable native API, implemented by
`compose_parser`. It launches `compose-worker.exe` directly with Win32 APIs,
sends captured inputs through pipes, and returns a typed result. The driver in
`tools\compose-parser-poc` is not the implementation.

See [contract.md](contract.md) for the deliberately bounded Compose slice,
projection, diagnostics, and limits. This is not a production WSLC parser,
capability profile, or reconciler.

## Build

Windows, CMake, MSVC x64/ARM64, SDK 26100, NuGet, and Microsoft Go are required
to build. Go is not required on the machine running the resulting worker.
No cgo, GCC, MinGW, Go DLL, Docker installation, or WSL activation is used.

From this worktree root, after acquiring and verifying Microsoft Go:

```powershell
$go = (Resolve-Path .\build_tools\microsoft-go-1.27.1-2\go\bin\go.exe).Path
nuget install Microsoft.Windows.ImplementationLibrary -Version 1.0.251108.1 -OutputDirectory out\compose-worker-x64\packages -ExcludeVersion -NonInteractive -Verbosity quiet
cmake -S tools\compose-parser-poc -B out\compose-worker-x64 -G "Visual Studio 18 2026" -A x64 -T version=14.51.36231 "-DCMAKE_SYSTEM_VERSION=10.0.26100.0" "-DCOMPOSE_GO_EXECUTABLE=$go"
cmake --build out\compose-worker-x64 --config Release --parallel 6
ctest --test-dir out\compose-worker-x64 -C Release --output-on-failure
```

The bootstrap pins JSON 3.12.0 and WIL 1.0.251108.1. Module metadata pins
compose-go v2.16.1 and its dependencies. Initial restore:

```powershell
cmake -E env GOTOOLCHAIN=local CGO_ENABLED=0 MS_GOTOOLCHAIN_TELEMETRY_ENABLED=0 "$go" -C src\windows\composeparser\go mod download
cmake -E env GOTOOLCHAIN=local CGO_ENABLED=0 MS_GOTOOLCHAIN_TELEMETRY_ENABLED=0 "$go" -C src\windows\composeparser\go test -mod=readonly ./...
```

For ARM64, configure a separate output directory with `-A ARM64` and its own
WIL include path (or an explicit `WIL_INCLUDE_DIR`). Build all targets there;
do not run ARM64 tests on an x64-only machine. The component selects
`GOOS=windows`, `GOARCH=amd64/arm64`, `CGO_ENABLED=0`, `GOTOOLCHAIN=local`, and
uses read-only module builds.

Build directories contain absolute paths. After a worktree move, use a fresh
build directory; do not reuse the moved CMake cache. Nothing depends on the
other prototype checkout. Local `GOMODCACHE`/`GOCACHE` overrides are optional.

## Use

The host supplies a trusted absolute executable path separately from the
untrusted captured Compose request:

```cpp
ParserOptions options;
options.WorkerPath = trustedWorkerPath;
Result result = Normalize(request, options);
```

The focused driver accepts one request JSON on stdin and an absolute worker
path as its only argument. It exits 0 on success, 2 for a structured failure,
and 3 for an unexpected driver failure. A complete example is in
`tools\compose-parser-poc\tests\fixtures\basic.json` (use its `request` object).

## Boundary and integration

- Admission visits every source before merge or profile exclusion. It expands
  aliases with a request-wide budget and rejects unimplemented forms.
- compose-go performs interpolation, canonicalization, merge, defaults, and
  consistency validation. The adapter does not implement Compose semantics.
- The pinned loader receives nonnil `Config` maps and captured contents.
  Project naming is explicit. Include/extends are rejected and disabled;
  env/label-file resolution is disabled. Only admitted bind paths reach the
  lexical path resolver; home expansion and symlink-resolving features are
  rejected before it. No discovery helpers or remote loaders are called.
- Environment lookup is explicit and case-sensitive. The worker installs its
  warning sink once at startup; no parser call changes process environment,
  CWD, or logging configuration.
- The native adapter uses overlapped named pipes, a handle inheritance list,
  an atomically assigned kill-on-close job, one deadline, and cancellation
  completion before releasing I/O buffers. It drains stdout/stderr while
  writing stdin and rejects size limits, malformed results, or exit mismatch.
- This is process isolation under the caller's token, **not a security
  sandbox**. YAML allocation precedes structural validation; limits are not
  hard memory ceilings.

Production integration still needs packaging/signing, trusted worker location,
threat-model review, fuzzing, capability/version policy, production model
projection, and governance approval. The root build, service/SDK, CLI,
localization, packaging, and governance manifest are unchanged.

## Tool provenance and evidence

Microsoft Go **1.27.1-2**, downloaded from
<https://aka.ms/golang/release/latest/go1.27.1-2.windows-amd64.zip>.
Verified archive SHA-256:
`22a8bc3d8af5e0fe0515c9ba7bb6324aa9d743c739c8fc670028eca0d9ef1c3b`.
The compiler's Authenticode signature was valid for Microsoft Corporation.
`go version -m` on the ARM64 worker reports:
`microsoft_toolset_version=go1.27.1-2-microsoft`, `microsoft_systemcrypto=1`,
`CGO_ENABLED=0`, `GOARCH=arm64`, `GOOS=windows`, `-buildmode=exe`.

The graph includes the existing compose-go dependencies plus its YAML v4 RC,
logrus, mapstructure, JSON schema, Docker utility, and `golang.org/x` modules.
`go.sum` is retained; `go list -m all` includes test-only/transitive modules.
The worker's embedded module list is the runtime-linked inventory. Neither
list is a completed Component Governance review.

Validated using MSVC 19.51.36260.0 / toolset 14.51.36231 and SDK 10.0.26100.0:

| Command/check | Result |
| --- | --- |
| `cmake --build out\compose-worker-x64 --config Release --parallel 6` | Passed |
| `ctest --test-dir out\compose-worker-x64 -C Release --output-on-failure` | Passed both suites |
| `go -C src\windows\composeparser\go test -mod=readonly ./...` with the environment above | Passed |
| `go -C src\windows\composeparser\go vet -mod=readonly ./...` with the environment above | Passed |
| `cmake --build out\compose-worker-arm64 --config Release --parallel 6` | Cross-build passed |
| PE headers / `go version -m` | Native ARM64 adapter and ARM64 cgo-free worker confirmed |
| ARM64 execution | Not run; no compatible test host used |

The shared tests cover golden projection, merge, interpolation operators,
profiles/dependencies, aliases/types, unsupported fields, captured inputs,
concurrency, and exact/over byte/node/depth boundaries. Fake-worker tests cover
output-before-input backpressure, early exit, blocked writer, timeout,
malformed/duplicate/unknown/version-mismatched output, invalid success,
exit mismatch, stdout/stderr limits, and repeated-call handle growth.
The native headers/protocol/contract, driver, semantic tests, and golden fixture
have matching SHA-256 hashes in both branches. Running the golden request
through both native drivers produced identical complete response JSON.

MSVC dumpbin 14.51.36260.0 read the ARM64 headers but crashed on
`/dependents` for the Go ARM64 executable. The installed LLVM `llvm-readobj
--file-headers --coff-imports` successfully inspected it. Both worker import
tables name `kernel32.dll`; this is not an inventory of dynamically loaded
Windows runtime libraries.

## Comparison

| Concern | compose-go worker | yaml-cpp library |
| --- | --- | --- |
| Compose rules | Upstream loader plus bounded admission/projection | Locally maintained interpretation |
| Native integration | Launch, pipes, protocol, deployment | Direct call and native dependency |
| Build | MSVC plus Microsoft Go | MSVC only |
| Failure boundary | Worker can be killed/reaped | Caller shares crashes/hangs |
| Example x64 footprint | Worker about 8.7 MB plus native driver about 0.29 MB | Driver including parser about 0.60 MB |
| Coverage evidence | Same bounded contract tests | Same bounded contract tests |

These are unstripped Release POC artifacts, not shipping package sizes or
performance benchmarks. Prefer the worker for further Compose-compatibility
work: the extra process/deployment machinery is explicit, while yaml-cpp
requires owning every additional semantic rule. The native prototype needed
explicit YAML numeric/tag handling beyond `YAML::Node::as<string>()`; passing
this corpus does not establish full Compose compatibility for either backend.

References:
[Microsoft Go installation](https://github.com/microsoft/go/blob/microsoft/main/eng/doc/Installation.md),
[Microsoft Go migration](https://github.com/microsoft/go/blob/microsoft/main/eng/doc/MigrationGuide.md),
[compose-go pin](https://github.com/compose-spec/compose-go/tree/v2.16.1),
[Compose specification](https://docs.docker.com/reference/compose-file/).
