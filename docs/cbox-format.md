# CBox format version 2

A `.cbox` is a ZIP archive containing one reusable C++ package for one target.
It is distinct from an application release archive.

The implemented profiles support executable, static-library, dynamic-library,
imported-library, and header-only packages:

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
├── dependencies/
│   └── doubled.cbox
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
format = 2

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

[[dependency]]
name = "doubled"
version = "1.0.0"
type = "header_only"
path = "dependencies/doubled.cbox"
sha256 = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"
```

Each `[[artifact]]` entry declares one packaged file. Static-library boxes
contain one `static_library` artifact under `lib/` and one or more
`public_header` artifacts under `include/`. Dynamic-library boxes contain one
`dynamic_library` artifact under `runtime/` and one or more `public_header`
artifacts under `include/`. Windows dynamic-library boxes additionally contain
one `import_library` artifact under `lib/`. Header-only boxes contain one or more
`public_header` artifacts and no library artifact.

Imported-library boxes contain one or more `public_header` artifacts and any
number of `static_library`, `dynamic_library`, and `import_library` artifacts.
They package an existing target-specific SDK or precompiled binary layout
without requiring Forge to build it.

The manifest determines package identity. The archive filename is only a
human-readable label.

Each `[[dependency]]` entry declares one direct dependency embedded as another
verified `.cbox`. Child boxes recursively carry their own direct dependencies,
making a format-2 box self-contained and portable without source projects,
lockfiles, or additional downloads.

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
- `lib/` contains static-library artifacts and Windows import libraries.
- `runtime/` contains dynamic-library runtime artifacts.
- `dependencies/` contains direct dependency boxes.
- Future profiles may add `licenses/`.
- Format 2 boxes contain exactly `cbox.toml`, declared artifacts, and declared
  dependency boxes.

## Verification

`forge box inspect`, `forge box verify`, `forge box publish`, and
`forge box extract` validate:

- The manifest uses a supported format version and contains every required
  field.
- Artifact paths are relative, remain inside the archive, and use the directory
  required by their artifact kind.
- The archive contains no undeclared files, symbolic links, or unsupported
  entries.
- Every artifact matches its lowercase SHA-256 checksum.
- Every embedded dependency matches its declared checksum.

Forge validates the ZIP directory before extraction. Extraction then copies only
the validated manifest, artifacts, and dependency boxes into the destination.
Verification, publication, and dependency consumption recursively validate each
embedded box and ensure its package identity, package type, and target match its
declaration.

`forge box publish <box>` publishes a verified box locally into the
project-root `boxes/` directory and writes `<box>.sha256` using the standard
`<checksum>  <filename>` format. The command must run from a Forge project root.

## Compatibility

One box represents one OS and architecture target. Forge validates both before
installing a direct local box dependency. Compiler, standard-library, ABI,
build-type, permissions, and deterministic archive rules remain future
compatibility dimensions.

Forge continues to consume format-1 boxes as self-contained leaf dependencies.
New boxes are written as format 2. Legacy boxes using `shared_library` as a
package type or artifact kind remain accepted as aliases for `dynamic_library`.
