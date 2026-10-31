# compose-go DLL prototype

`Normalize(Request, ParserOptions)` is a reusable MSVC C++ adapter that calls
compose-go v2.16.1 in the same process through a Go `c-shared` DLL. The focused
driver under `tools\compose-parser-poc` is not the implementation. No worker
process, Docker installation, WSL activation, or container execution is involved.

**Status:** x64 and ARM64 DLLs, native adapters, drivers, and test targets build.
The x64 C++ -> Go DLL semantic and ABI suites pass, along with Go tests, vet,
and the Go race detector. ARM64 artifacts were inspected but not executed:
no compatible host was available. These results cover the bounded POC
contract, not production readiness or full Compose compatibility.

This is the third independent worktree in the comparison, based on the same
`ee69aa7db4b463cce0c38e8db566a3a791a87342` commit. The worker and yaml-cpp
worktrees are unchanged and are not source/build dependencies.

## Interface and lifetime

```cpp
ParserOptions options;
options.LibraryPath = trustedAbsoluteDllPath;
Result result = Normalize(request, options);
```

The model and bounded semantics remain those in [contract.md](contract.md).
`LibraryPath` is trusted host configuration, not part of captured YAML.
There is deliberately no `Timeout` option: in-process parsing cannot safely be
terminated on a deadline. Use from `DllMain` or under the loader lock is unsupported.

- `LoadLibraryExW` uses the DLL directory and System32 search flags, not PATH
  or CWD discovery. A successfully loaded module is pinned for process lifetime,
  even if its ABI is rejected; it may already have started Go runtime threads.
  Transient loader references are balanced. If pinning itself fails, the
  adapter retains the load reference rather than unloading an active runtime.
- The generated `compose-go.h` supplies ABI declarations and status constants.
  Both native builds compile against this header without a handwritten copy
  of the function signatures. The fake DLL tests use linker exports to keep
  their definitions compatible with the generated declarations.
- `ComposeNormalize` accepts explicit-length UTF-8 JSON, copies the input,
  and returns C-allocated bytes. Only `ComposeFree` in the allocating DLL may
  free the response. No Go string, slice, map, or managed pointer crosses the ABI.
- Null/length combinations, request/response bounds, ABI version, strict
  response shape, and status/envelope agreement are checked. Arbitrary invalid
  native pointers and double frees remain caller bugs.
- The Go default logger is configured once, with discarded textual output and
  a warning hook. A Go mutex serializes interpretation and clears diagnostics
  on each request, including panic paths. Concurrent callers are safe to use,
  but parsing is not parallel.
- Ordinary parser panics become internal errors. Fatal runtime errors, OOM,
  native faults, and hangs affect the caller. This is not a security sandbox.
- Captured environment/CWD behavior is unchanged: no parser call changes the
  host environment or directory. Non-nil admitted `Config` maps bypass upstream
  file loading; includes/extends/env_file/label_file are rejected and disabled.
  Only admitted bind paths reach lexical normalization. No remote loaders or
  discovery helpers are used.

## Toolchain

The same Microsoft Go, MSYS2 Clang family, C ABI, and source files target both
x64 and ARM64. MSVC remains the compiler for native callers. ARM64 targeting
uses the x64 Clang driver with official ARM64 headers, CRT, compiler-rt and
unwind libraries; no compiler patch or rebuilt runtime was needed for either
full DLL build. This tested package combination is not a claim that MSYS2
officially supports every x64-hosted ARM64 cross-build.

Tools are isolated in ignored `build_tools`; no global PATH change was made.
The examples assume an ASCII, no-space MSYS2 installation path.

| Dependency | Version/provenance |
| --- | --- |
| Microsoft Go | 1.27.1-2, independently copied from the comparison toolchain; Microsoft Corporation Authenticode signature revalidated |
| MSYS2 base | `msys2-base-x86_64-20260927.tar.zst`, downloaded from the official repository and detached signature verified |
| Clang / LLD / compiler-rt / libunwind | MSYS2 23.1.3-1 |
| MinGW-w64 CRT / headers | MSYS2 14.0.0.r536.g7ce22f663-1 |
| Native compiler | MSVC 19.51.36260.0, toolset 14.51.36231, SDK 10.0.26100.0 |
| Native dependencies | nlohmann-json 3.12.0 (SHA-256 pinned by CMake), WIL 1.0.251108.1 (restored with NuGet) |
| Go dependencies | Existing `go.mod` and `go.sum`, including compose-go v2.16.1, unchanged |

The MSYS2 archive SHA-256 is
`8a2095647afbf799d113d0cae35f7544589d059819042635504e75bd2179bd02`.
GPG reported a good signature with the documented primary fingerprint
`0EBF782C5D53F7E5FB02A66746BD761F7A49B0EC`.
The isolated pacman install checked package signatures and integrity.
Microsoft Go's previously verified source archive SHA-256 is
`22a8bc3d8af5e0fe0515c9ba7bb6324aa9d743c739c8fc670028eca0d9ef1c3b`.
This older Go patch was retained for comparison, not recommended for release.

Installed compiler/target packages:

```text
mingw-w64-clang-x86_64-clang
mingw-w64-clang-x86_64-lld
mingw-w64-clang-aarch64-headers
mingw-w64-clang-aarch64-crt
mingw-w64-clang-aarch64-compiler-rt
mingw-w64-clang-aarch64-libwinpthread
mingw-w64-clang-aarch64-libunwind
```

## Build and run

Validated from this worktree with the tool versions above:

```powershell
$root = (Get-Location).Path
$go = "$root\build_tools\microsoft-go-1.27.1-2\go\bin\go.exe"
$msys = "$root\build_tools\msys64"
$clang = "$msys\clang64\bin\clang.exe"
nuget install Microsoft.Windows.ImplementationLibrary -Version 1.0.251108.1 -OutputDirectory out\compose-dll-x64\packages -ExcludeVersion -NonInteractive -Verbosity quiet
cmake -S tools\compose-parser-poc -B out\compose-dll-x64 -G "Visual Studio 18 2026" -A x64 -T version=14.51.36231 "-DCMAKE_SYSTEM_VERSION=10.0.26100.0" "-DCOMPOSE_GO_EXECUTABLE=$go" "-DCOMPOSE_CGO_COMPILER=$clang"
cmake --build out\compose-dll-x64 --config Release --parallel 6
cmake --build out\compose-dll-x64 --config Release --target compose_go_check
ctest --test-dir out\compose-dll-x64 -C Release --output-on-failure
```

For ARM64, use the same targets with compiler-driver target options:

```powershell
$target = "--target=aarch64-w64-windows-gnu --sysroot=$msys\clangarm64 -resource-dir=$msys\clangarm64\lib\clang\23 -isystem $msys\clang64\lib\clang\23\include"
cmake -S tools\compose-parser-poc -B out\compose-dll-arm64 -G "Visual Studio 18 2026" -A ARM64 -T version=14.51.36231 "-DCMAKE_SYSTEM_VERSION=10.0.26100.0" "-DCOMPOSE_GO_EXECUTABLE=$go" "-DCOMPOSE_CGO_COMPILER=$clang" "-DCOMPOSE_CGO_FLAGS=$target" "-DWIL_INCLUDE_DIR=$root\out\compose-dll-x64\packages\Microsoft.Windows.ImplementationLibrary\include"
cmake --build out\compose-dll-arm64 --config Release --parallel 6
```

Do not run ARM64 test executables on an x64-only host. Native ARM64 hosts can
instead use MSYS2's native `clangarm64\bin\clang.exe` without cross-target flags.
This native-host path has not been executed.

CMake sets `CGO_ENABLED=1`, `GOOS=windows`, architecture-specific `GOARCH`,
`GOTOOLCHAIN=local`, disabled Microsoft toolchain telemetry, and separate
build-local Go caches. Target options belong on `CC`: putting `--target` in
`CGO_LDFLAGS` was rejected by Go's linker-metadata validation.
Do not weaken the flag validation. Fresh build directories are required after
moving the checkout.

The driver accepts an absolute library path and one JSON request on stdin.
The fixture's `request` object is an example, not the entire fixture file.
The actual x64 driver was also exercised with the fixture:

```powershell
Get-Content tools\compose-parser-poc\tests\fixtures\basic.json | ConvertFrom-Json | Select-Object -ExpandProperty request | ConvertTo-Json -Depth 100 -Compress | & .\out\compose-dll-x64\Release\compose-parser-poc.exe "$root\out\compose-dll-x64\composeparser\compose-go.dll"
```

This exited 0 with a successful normalized project and no diagnostics.

## Validation evidence

Validated on 2026-10-27. The configure commands above passed for both platforms;
the WIL restore succeeded through ordinary `nuget install` on that date.

| Command/check | Result |
| --- | --- |
| `cmake --build out\compose-dll-x64 --config Release --parallel 6` | Passed, all POC targets |
| `cmake --build out\compose-dll-arm64 --config Release --parallel 6` | Passed, all POC targets cross-built on x64 |
| `ctest --test-dir out\compose-dll-x64 -C Release --output-on-failure` | Passed `compose-contract` and `compose-library` |
| `cmake --build out\compose-dll-x64 --config Release --target compose_go_check` | Go tests and vet passed |
| Go race-detector command below | Passed |
| PE headers/exports and `go version -m` | Correct x64/ARM64 machines, three C ABI exports, Microsoft Go 1.27.1-2, `CGO_ENABLED=1`, `-buildmode=c-shared` |
| Changed native code `clang-format --dry-run --Werror` / changed Go code `gofmt -l` | Passed / no unformatted files |
| Shared-source SHA-256 and semantic-test diff | Parser, admission, module metadata, native protocol, and golden fixture match the worker; test changes only replace `WorkerPath` with `LibraryPath` |
| ARM64 execution | Not run: no compatible host |

The race-detector invocation uses the same x64 compiler and caches:

```powershell
cmake -E env GOTOOLCHAIN=local CGO_ENABLED=1 GOOS=windows GOARCH=amd64 MS_GOTOOLCHAIN_TELEMETRY_ENABLED=0 "CC=$clang" "GOMODCACHE=$root\out\compose-dll-x64\go-modules" "GOCACHE=$root\out\compose-dll-x64\go-cache" "$go" -C src\windows\composeparser\go test -mod=readonly -race ./...
```

Native tests cover the existing semantic corpus with only the library-path
setup changed, plus ABI arguments, independent response buffers, repeated and
concurrent warning/environment isolation, invalid DLL paths/exports/versions,
malformed responses, status mismatch, and DLL-owned cleanup on error paths.
Go tests cover the golden projection, captured environment, request validation,
concurrent warning isolation, and ordinary panic recovery followed by reuse.
The Go race run exercises the Go package, not the MSVC caller; native concurrent
calls are exercised separately by `compose-library`. Repeated calls and fake-DLL
allocation accounting check cleanup behavior, not a proof of leak freedom.

## Artifacts and runtime dependencies

Release artifacts (unstripped Go DLLs; not shipping package sizes):

| Target | x64 bytes | ARM64 bytes |
| --- | --- | --- |
| `composeparser\compose-go.dll` | 12,118,016 | 11,578,368 |
| `Release\compose-parser-poc.exe` | 267,776 | 287,744 |

Each build also produces `composeparser\compose-go.h` and
`composeparser\Release\compose_parser.lib`. The native adapter dynamically loads
the DLL, so callers do not need a MinGW import library.

Both Go DLLs directly import only `KERNEL32.dll` and Windows UCRT API sets
(`stdio`, `runtime`, `heap`, and `private`). No MSYS2, libwinpthread, libunwind,
or compiler DLL appears in either import table. This does not imply that no
third-party runtime code was linked into the DLL: Go, MinGW-w64 CRT and
compiler-runtime code must still be considered for distribution/governance.

The native x64 driver additionally imports `MSVCP140.dll`, `VCRUNTIME140.dll`,
and `VCRUNTIME140_1.dll`; ARM64 imports the first two. The installed x64 VC
runtime imports were inspected and lead only to those VC runtime libraries,
Kernel32, and Windows UCRT API sets. Deploy the appropriate supported VC
runtime with the native caller. ARM64 VC runtime transitive imports were not
inspected on an ARM64 installation.

These are static import-table checks, not a complete trace of dynamically
loaded Windows libraries. No Go compiler or MSYS2 environment is a runtime
prerequisite for the resulting parser. Release deployment still needs signing,
minimum-OS/runtime verification, and a complete component inventory.

Inspection commands:

```powershell
& "$go" version -m out\compose-dll-x64\composeparser\compose-go.dll out\compose-dll-arm64\composeparser\compose-go.dll
& "C:\Program Files\Microsoft Visual Studio\18\Insiders\VC\Tools\Llvm\x64\bin\llvm-readobj.exe" --file-headers --coff-exports out\compose-dll-x64\composeparser\compose-go.dll out\compose-dll-arm64\composeparser\compose-go.dll out\compose-dll-arm64\Release\compose-parser-poc.exe
& "C:\Program Files\Microsoft Visual Studio\18\Insiders\VC\Tools\Llvm\x64\bin\llvm-readobj.exe" --coff-imports out\compose-dll-x64\composeparser\compose-go.dll out\compose-dll-arm64\composeparser\compose-go.dll out\compose-dll-x64\Release\compose-parser-poc.exe out\compose-dll-arm64\Release\compose-parser-poc.exe
```

## Comparison and remaining product work

| Concern | compose-go worker | compose-go DLL | yaml-cpp |
| --- | --- | --- | --- |
| Compose semantics | Upstream plus bounded admission/projection | Same upstream pipeline | Locally maintained |
| Native boundary | Process and pipes | C ABI and DLL-owned buffers | Native calls |
| Build tools | MSVC + Microsoft Go | MSVC + Microsoft Go + MSYS2 C toolchain | MSVC |
| Failure containment | Separate worker, hard deadline possible | Shares crashes/hangs with caller | Shares crashes/hangs with caller |
| Lifetime | Per-request worker | Process-lifetime Go runtime | Ordinary native lifetime |

No performance winner is established by this POC. Production integration needs
trusted deployment paths, signing/packaging, fuzzing and threat-model review,
capability/version policy, production model projection, and governance review.
Both build tools and any runtime-linked third-party code require appropriate
OSS handling; this is not CG clearance or proof that all toolchain dependencies
are development-only. Root builds, service/SDK, CLI, localization, packaging,
pipelines, and governance manifests were not changed.

## References

- [compose-go v2.16.1](https://github.com/compose-spec/compose-go/tree/v2.16.1)
- [cgo exports, ownership and compiler options](https://pkg.go.dev/cmd/cgo)
- [Go build modes](https://pkg.go.dev/cmd/go#hdr-Build_modes)
- [Microsoft Go installation](https://github.com/microsoft/go/blob/microsoft/main/eng/doc/Installation.md)
- [Microsoft Go comparison archive](https://aka.ms/golang/release/latest/go1.27.1-2.windows-amd64.zip)
- [MSYS2 archive and signature](https://repo.msys2.org/distrib/x86_64/)
- [MSYS2 signature verification](https://www.msys2.org/docs/installer/)
- [MSYS2 environments](https://www.msys2.org/docs/environments/)
- [MSYS2 native ARM64 requirements](https://www.msys2.org/docs/arm64/)
- [MSYS2 ARM64 target CRT](https://packages.msys2.org/packages/mingw-w64-clang-aarch64-crt)
- [Windows DLL loading](https://learn.microsoft.com/windows/win32/api/libloaderapi/nf-libloaderapi-loadlibraryexw)
- [Process-lifetime module pinning](https://learn.microsoft.com/windows/win32/api/libloaderapi/nf-libloaderapi-getmodulehandleexw)
- [Microsoft OSS usage requirements](https://docs.opensource.microsoft.com/using/)
