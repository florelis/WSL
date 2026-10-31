# yaml-cpp Compose parser prototype

`Normalize(Request)` is implemented directly by the reusable `compose_parser`
C++ library. The focused driver lives in `tools\compose-parser-poc`; no parser
implementation lives in the driver. There is no Go, Docker, worker executable,
or dependency on the other prototype checkout.

See [contract.md](contract.md) for the common bounded Compose slice and typed
POC projection. This is not the production WSLC normalized model or reconciler.

## Build

From this worktree root, with Windows, CMake, MSVC, SDK 26100, and NuGet:

```powershell
nuget install Microsoft.Windows.ImplementationLibrary -Version 1.0.251108.1 -OutputDirectory out\compose-yaml-x64\packages -ExcludeVersion -NonInteractive -Verbosity quiet
cmake -S tools\compose-parser-poc -B out\compose-yaml-x64 -G "Visual Studio 18 2026" -A x64 -T version=14.51.36231 "-DCMAKE_SYSTEM_VERSION=10.0.26100.0"
cmake --build out\compose-yaml-x64 --config Release --parallel 6
ctest --test-dir out\compose-yaml-x64 -C Release --output-on-failure
```

For ARM64, configure a separate output directory:

```powershell
cmake -S tools\compose-parser-poc -B out\compose-yaml-arm64 -G "Visual Studio 18 2026" -A ARM64 -T version=14.51.36231 "-DCMAKE_SYSTEM_VERSION=10.0.26100.0" "-DWIL_INCLUDE_DIR=$PWD\out\compose-yaml-x64\packages\Microsoft.Windows.ImplementationLibrary\include"
cmake --build out\compose-yaml-arm64 --config Release --parallel 6
```

The bootstrap uses existing repository pins: yaml-cpp 0.9.0, JSON 3.12.0, and
WIL 1.0.251108.1. yaml-cpp is linked statically; the native targets use the DLL
MSVC runtime. Its tests/tools/install targets are disabled without overriding
the parent's `BUILD_TESTING`.

Build directories contain absolute paths. After moving a worktree, configure
a fresh output directory rather than reusing its moved CMake cache.

## Use and implementation

```cpp
Result result = Normalize(request);
```

The driver takes one request JSON on stdin, without a worker-path argument.
It exits 0 for success, 2 for a structured failure, or 3 for an unexpected
driver failure. See the `request` object in
`tools\compose-parser-poc\tests\fixtures\basic.json` for a complete example.

The library owns source admission, YAML merge keys/aliases and scalar types,
interpolation, field-specific multi-file merge, shorthand/default
normalization, lexical path handling, and profile selection. A common native
dependency-graph validator is also used to check decoded worker responses in
the other branch. Request state is local; it never discovers files, reads
`.env`, queries host environment, or changes CWD.

The YAML node tree is traversed read-only and expanded into owned values.
Explicit keys and merge-sequence precedence are preserved. Numeric tags and
Compose scalar-to-string conversion need handling beyond simply calling
`as<string>()`; tests caught differences for `0o12`, `1.0`, and invalid
explicit tags.

There is **no safe hard timeout or crash isolation** for an in-process parser.
CTest has an external timeout, but the reusable API cannot cancel an
arbitrarily stuck yaml-cpp operation. Structural checks run after decoding,
so the byte/node/depth budgets are not hard memory ceilings.

## Evidence and comparison

Validated using MSVC 19.51.36260.0 / toolset 14.51.36231 and SDK 10.0.26100.0:

| Command/check | Result |
| --- | --- |
| `cmake --build out\compose-yaml-x64 --config Release --parallel 6` | Passed |
| `ctest --test-dir out\compose-yaml-x64 -C Release --output-on-failure` | Passed |
| `cmake --build out\compose-yaml-arm64 --config Release --parallel 6` | Cross-build passed |
| ARM64 PE headers | `AA64` confirmed |
| ARM64 execution | Not run; no compatible test host used |

The shared suite covers golden projection, interpolation operators,
multi-file merge, profiles/dependencies, YAML aliases/types, unsupported
fields, captured inputs, concurrent calls, and exact/over byte/node/depth
limits. It is a bounded corpus, not full Compose conformance or a fuzzing
campaign. yaml-cpp 0.9.0 emits upstream C4267 warnings during its own build.
The native headers/protocol/contract, driver, semantic tests, and golden fixture
have matching SHA-256 hashes in both branches. Running the golden request
through both native drivers produced identical complete response JSON.

| Concern | yaml-cpp library | compose-go worker |
| --- | --- | --- |
| Build/tool deployment | MSVC/native library only | Microsoft Go plus MSVC; deploy worker |
| Compose behavior | Handwritten semantic implementation | Upstream Compose loader |
| Calls | In-process typed C++ API | Typed C++ API over checked pipes |
| Failure containment | Caller shares hangs/crashes | Kill/reap a separate process |
| Example x64 footprint | Driver including parser about 0.60 MB | Worker about 8.7 MB plus driver about 0.29 MB |

Sizes are unstripped Release POC artifacts, not shipping estimates or latency
measurements. The worker is the stronger starting point for expanding Compose
compatibility: it avoids owning the full semantic layer. yaml-cpp makes
native integration simpler, but the extra rules remain our maintenance
responsibility.

Production integration still requires a threat model, fuzzing, production
projection and capability policy, packaging/signing, and dependency governance
approval. No root build, service/SDK, CLI, localization, packaging, CI, or
governance-manifest changes are included.

References:
[yaml-cpp](https://github.com/jbeder/yaml-cpp),
[Compose merge](https://docs.docker.com/reference/compose-file/merge/),
[Compose interpolation](https://docs.docker.com/reference/compose-file/interpolation/),
[Compose services](https://docs.docker.com/reference/compose-file/services/).
