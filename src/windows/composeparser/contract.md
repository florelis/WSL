# Compose normalization contract

`wsl::windows::compose::Normalize(const Request&)` converts captured Compose
inputs into an owned `Project` and diagnostics. This is an internal C++ contract,
not a public SDK API or a complete Compose implementation. The `compose_parser`
build target supplies declarations only on the foundation branch; backend branches
provide the implementation. There is no fallback parser or command.

## Boundary

The caller reads files and captures project context before calling the parser.
All strings are UTF-8. Sources are ordered, with unique nonempty IDs and contents;
filenames are diagnostic metadata, never opened. Project names match
`[a-z0-9][a-z0-9_-]*`. The project directory is a fully qualified Windows drive
or UNC path, normalized lexically.

Environment lookup is explicit and case-sensitive, including on Windows.
`COMPOSE_PROJECT_NAME` comes from the explicit project name. Profiles are explicit;
`*` selects all profiles. No ambient environment, CWD, `.env`, file discovery,
network access, session creation, or container operation is part of normalization.
A future command passes the accepted model to a separate execution service.

## Supported model

| Input | Normalized representation |
| --- | --- |
| Services | Name-keyed services with a nonempty image; disabled services remain represented and validated |
| Command/entrypoint | String arrays, preserving absent, null, and explicitly empty states |
| Environment/labels | Map/list forms normalized to string maps; missing environment values resolve only from the request |
| Ports | Single TCP/UDP ports, 1..65535, optional published port and IPv4 binding |
| Mounts | Anonymous/named volumes and bind mounts; logical named-volume sources, lexical paths, explicit read-only/create-host-path flags |
| Dependencies | Named dependencies with `service_started`, required=true, restart=false semantics |
| Profiles | Profile names plus each service's enabled state |
| Named volumes | Logical names mapped to project-scoped names; only null/empty definitions |
| Network | Implicit project default network name only |

Interpolation is per source before merge: direct variables, default/required/
alternative operators, nesting, and `$$`; non-variable dollar signs remain literal.
Undefined direct variables warn and become empty strings. Map keys are not
interpolated. YAML anchors, aliases, and merge keys preserve explicit-key and
first-merge-map precedence. Duplicate explicit keys, nonstring keys, cycles,
custom tags, and multiple documents fail.

Ordered merging combines maps by key, replaces command/entrypoint, merges mounts
by target, deduplicates ports by host/target/published/protocol, and unions profiles.
Dependencies must exist and be acyclic; enabled services cannot require disabled
ones. Short binds default create-host-path to true, long binds to false.
Container targets are absolute Linux paths. Home expansion, drive-relative and
root-relative host paths, and drive-letter short mount syntax are unsupported.

Every source is admitted before merge or profile exclusion. Unlisted features
are rejected, not silently discarded: this includes build, include, extends,
env_file, label_file, explicit networks, secrets/configs, healthchecks, string
commands, `x-*`, `!reset`, and `!override`. These restrictions describe the bounded
contract, not invalid Compose syntax.

## Results and failures

`AcceptedProject` exists only on success. Failure has error diagnostics and no
partial project; success may have warnings. Diagnostics contain a code, severity
(`error` or `warning`), message, and optional source ID/property path. Codes include
`invalid_request`, `invalid_yaml`, `invalid_compose`, `unsupported_feature`,
`limit_exceeded`, `internal_error`, `backend_error`, and `timeout`.
Messages are diagnostic detail, not a localized command-line presentation API.
Callers must not depend on transport-specific errors or English wording.

Implementations enforce resource limits and document them separately. Limits
checked after decoding are not hard memory ceilings. This API promises neither a
security sandbox nor hard cancellation; an in-process backend cannot safely
terminate arbitrary parsing on a deadline. Concurrent calls must not mix captured
inputs or diagnostics, but parallel execution is not guaranteed.

References: [Compose specification](https://docs.docker.com/reference/compose-file/),
[interpolation](https://docs.docker.com/reference/compose-file/interpolation/),
[merge](https://docs.docker.com/reference/compose-file/merge/).
