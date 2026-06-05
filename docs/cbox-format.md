# CBox format version 1

A `.cbox` is a ZIP archive containing one reusable C++ artifact for one target.
It is distinct from an application release archive.

The first implemented profile supports executable artifacts:

```text
hello-0.1.0-macos-arm64.cbox
├── cbox.toml
└── bin/
    └── hello
```

When a project specifies a build number, Forge preserves it as SemVer build
metadata in the filename:

```text
termin8or-3.0.0+build.6-macos-arm64.cbox
```

## Manifest

Every box contains `cbox.toml` at its root:

```toml
[cbox]
format = 1

[package]
name = "hello"
version = "0.1.0"
type = "executable"

[target]
os = "macos"
arch = "arm64"

[artifact]
path = "bin/hello"
kind = "executable"
```

The manifest determines package identity. The archive filename is only a
human-readable label.

An optional build number is stored separately from the package version:

```toml
[package]
name = "termin8or"
version = "3.0.0"
build = 6
type = "executable"
```

This keeps dependency compatibility based on `3.0.0` while distinguishing
individual compiled artifacts.

## Paths

- Archive paths use `/` separators.
- Absolute paths and parent traversal are forbidden.
- `bin/` contains executable artifacts.
- Future profiles may add `include/`, `lib/`, `runtime/`, and `licenses/`.

## Compatibility

One box represents one OS and architecture target. Compiler, standard-library,
ABI, build-type, checksums, permissions, and deterministic archive rules will
be added before binary dependency resolution relies on boxes.
