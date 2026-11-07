# yaml-cpp backend

Implements the internal `Normalize(Request)` API described in
[contract.md](contract.md). Source admission, interpolation, merge, projection
and validation are locally maintained, rather than provided by compose-go.

Build from the normal WSL root. The parser uses the root-provided yaml-cpp
dependency, native compiler and CRT policy; no additional toolchain or runtime
artifact is required.

Normalization is request-local and in-process. Limits are 4 MiB encoded
requests, 16 MiB encoded results, effective YAML depth 64 and 100,000 expanded
visits. Structural limits follow YAML decoding, so they are not hard memory
ceilings. There is no hard timeout or crash containment.

`ComposeParserTests` in `wsltests.dll` is the same corpus as on the Go branches,
including literal-dollar interpolation. No standalone parser application is
built or shipped.

References: [yaml-cpp](https://github.com/jbeder/yaml-cpp),
[Compose merge](https://docs.docker.com/reference/compose-file/merge/),
[Compose interpolation](https://docs.docker.com/reference/compose-file/interpolation/).
