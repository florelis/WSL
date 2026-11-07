# compose-go DLL

Implements the internal `Normalize(Request)` API described in
[contract.md](contract.md). There is no Compose command or standalone parser tool.

Configure the normal WSL root build with `COMPOSE_GO_EXECUTABLE` (or `go` on
PATH), `COMPOSE_CGO_COMPILER` pointing to the verified MSYS2 Clang executable,
and any target/sysroot options in `COMPOSE_CGO_FLAGS`. The C compiler must target
the Windows architecture selected by `TARGET_PLATFORM`. Modules remain pinned
to compose-go v2.16.1 and the checked-in lockfiles.

Building a consumer also builds `compose-go.dll` beside the normal native
binaries and generates its private ABI header. The adapter loads it beside its
own module with restricted DLL search flags, never through PATH or CWD.
Go runtime modules remain pinned for the process lifetime, including rejected
ABIs. Responses are freed by their allocating DLL. Parsing is serialized inside
Go; there is no hard timeout or crash containment.

Limits: 4 MiB encoded requests, 16 MiB responses, effective YAML depth 64 and
100,000 expanded visits. Structural limits follow decoding, not hard memory
ceilings.

`ComposeParserTests` and `ComposeLibraryTests` live in `wsltests.dll`; the fake,
wrong-ABI and missing-export DLLs are test fixtures. `compose_go_check` runs
Go tests and vet for the selected architecture, requiring a compatible execution
host. Installer, signing and release deployment are deferred.

For x64-hosted ARM64 cross-builds, the original PoC used an MSYS2 x64 Clang
driver with ARM64 headers, CRT and runtime libraries. Supply the following
options through `COMPOSE_CGO_FLAGS`, replacing `<msys2>` with the verified
toolchain root:

```text
--target=aarch64-w64-windows-gnu --sysroot=<msys2>\clangarm64 -resource-dir=<msys2>\clangarm64\lib\clang\23 -isystem <msys2>\clang64\lib\clang\23\include
```

The original comparison toolchain used Microsoft Go 1.27.1-2, MSYS2
Clang/LLD/compiler-rt/libunwind 23.1.3-1, and MinGW-w64 CRT/headers
14.0.0.r536.g7ce22f663-1. Retaining these versions is not a release
recommendation or governance clearance. The Microsoft Go archive SHA-256 was
`22a8bc3d8af5e0fe0515c9ba7bb6324aa9d743c739c8fc670028eca0d9ef1c3b`, with a
verified Microsoft Corporation Authenticode signature. The MSYS2 base archive
`msys2-base-x86_64-20260927.tar.zst` had SHA-256
`8a2095647afbf799d113d0cae35f7544589d059819042635504e75bd2179bd02`;
its detached signature and installed package signatures were verified.
See [Microsoft Go installation](https://github.com/microsoft/go/blob/microsoft/main/eng/doc/Installation.md),
[MSYS2 environments](https://www.msys2.org/docs/environments/), and
the [compose-go pin](https://github.com/compose-spec/compose-go/tree/v2.16.1).
