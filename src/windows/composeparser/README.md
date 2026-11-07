# compose-go worker

Implements the internal `Normalize(Request)` API described in
[contract.md](contract.md). There is no Compose command or standalone parser tool.

Configure the normal WSL root build with `COMPOSE_GO_EXECUTABLE` pointing to the
verified Go executable (or supply `go` on PATH). The worker uses compose-go
v2.16.1 with the checked-in module lockfiles and `CGO_ENABLED=0`.
`TARGET_PLATFORM` selects amd64 or arm64.

Building a consumer also builds `compose-worker.exe` beside the normal native
binaries. The adapter resolves it beside its own module, never through PATH or
the current directory. Each call uses a new process, overlapped pipes, and a
kill-on-close job with a private 15-second deadline. It runs under the caller's
token and is not a security sandbox.

Limits: 4 MiB encoded requests, 16 MiB responses, 64 KiB stderr, effective YAML
depth 64 and 100,000 expanded visits. Structural limits follow decoding and are
not hard memory limits.

`ComposeParserTests` and `ComposeWorkerTests` are part of `wsltests.dll`.
`compose_fake_worker` is a test fixture, not a user-facing parser tool.
The `compose_go_check` target runs Go tests and vet for the selected architecture;
cross-built tests require a compatible execution host.
Installer, signing and release deployment are not implemented on this branch.

The original comparison toolchain was Microsoft Go 1.27.1-2, with archive
SHA-256 `22a8bc3d8af5e0fe0515c9ba7bb6324aa9d743c739c8fc670028eca0d9ef1c3b`
and a verified Microsoft Corporation Authenticode signature. Retaining this
toolchain for comparison is not a release recommendation or governance clearance.
See [Microsoft Go installation](https://github.com/microsoft/go/blob/microsoft/main/eng/doc/Installation.md)
and the [compose-go pin](https://github.com/compose-spec/compose-go/tree/v2.16.1).
