# CBox format version 1

A `.cbox` is a ZIP archive containing one reusable C++ package for one target.
It is distinct from an application release archive.

The implemented profiles support executable, static-library, shared-library,
and header-only packages:

```text
hello-0.1.0-macos-arm64.cbox
├── cbox.toml
└── bin/
    └── hello
```

Header-only boxes contain only the manifest and public headers:

```text
hello-1.0.0-macos-arm64.cbox
├── cbox.toml
└── include/
    └── hello/
        └── hello.h
```

```text
hello-1.0.0-macos-arm64.cbox
├── cbox.toml
├── include/
│   └── hello/
│       └── hello.h
└── lib/
    └── libhello.a
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

[[artifact]]
path = "bin/hello"
kind = "executable"
sha256 = "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"
```

Each `[[artifact]]` entry declares one packaged file. Static-library boxes
contain one `static_library` artifact under `lib/` and one or more
`public_header` artifacts under `include/`. Shared-library boxes contain one
`shared_library` artifact under `runtime/` and one or more `public_header`
artifacts under `include/`. Header-only boxes contain one or more
`public_header` artifacts and no library artifact.

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
- `include/` contains public headers.
- `lib/` contains static-library artifacts.
- `runtime/` contains shared-library runtime artifacts.
- Future profiles may add `licenses/`.
- Format 1 boxes contain exactly `cbox.toml` and the declared artifacts.

## Verification

`forge box inspect`, `forge box verify`, and `forge box extract` validate:

- The manifest uses supported format version 1 and contains every required
  field.
- Artifact paths are relative, remain inside the archive, and use the directory
  required by their artifact kind.
- The archive contains no undeclared files, symbolic links, or unsupported
  entries.
- Every artifact matches its lowercase SHA-256 checksum.

Forge validates the ZIP directory before extraction. Extraction then copies only
the validated manifest and artifacts into the destination.

## Compatibility

One box represents one OS and architecture target. Compiler, standard-library,
ABI, build-type, permissions, and deterministic archive rules will be added
before binary dependency resolution relies on boxes.
