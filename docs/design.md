# Forge design baseline

Forge is a project-centric workflow tool for C++. It coordinates existing
compilers and build systems rather than replacing them.

## Principles

- Forge is a C++20 single executable.
- CMake is the first build backend.
- `forge.recipe.toml` describes human-authored project intent.
- `forge.lock.toml` records exact resolved dependencies and artifacts.
- A **box** is a packaged C++ artifact stored in a `.cbox` file.
- Forge starts locally. Remote registries and shared caches come later.
- `forge init` adopts an existing project and never creates, moves, or modifies
  source files.
- `forge new <name>` explicitly creates a new project with a recipe and starter
  source file.
- `forge build` generates private CMake infrastructure under `.forge/generated`
  and writes artifacts under `.forge/build`.
- `forge run` performs an incremental build before launching the executable and
  forwarding its arguments and exit status.
- `forge release` stages an application release and creates a versioned ZIP
  archive. Release archives are distinct from dependency `.cbox` artifacts.

## Project workspace

Forge keeps generated and downloaded state under an ignored `.forge`
directory:

```text
.forge/
├── boxes/       # packaged binary artifacts
├── deps/        # installed dependency boxes
├── build/       # build outputs
├── cache/       # reusable local cache
└── generated/   # generated CMake files and presets
```

Dependency resolution should prefer a compatible cached box. When no matching
box exists, Forge fetches or uses source, builds it, creates a box, and caches
that box. Source dependency boxes are reused when their package identity,
target, declared inputs, and embedded direct dependency checksums still match.

Local path dependencies may be declared by executable, static-library,
dynamic-library, and header-only projects. Forge recursively builds library
dependencies, rejects cycles and conflicting project names, links the
transitive static-library closure in dependency order, and stages dynamic
libraries for running and releasing applications.

Local `.cbox` dependencies are verified and installed directly without their
source project. Format-2 boxes embed their direct dependency boxes and Forge
recursively installs the complete graph. Format-1 boxes remain supported as
self-contained leaf dependencies.

Downloadable `.cbox` dependencies require an explicit SHA-256 checksum. Forge
uses the checksum as an immutable local cache key before applying normal box
verification and installation.

GitHub Release dependencies use `{ github = "owner/repository", version =
"<version>" }`. Forge resolves the conventional target-qualified box and
checksum assets from the matching `release-<version>` tag, then records the
exact target, URL, and checksum in `forge.lock.toml`. Normal builds require and
consume matching locked entries without re-resolving GitHub. `forge update`
and `forge update <dependency>` deliberately refresh current-target entries
without building the current project, while preserving other targets.

## Initial roadmap

1. v0.1: `forge new`, `forge init`, `forge build`, `forge run`, and
   `forge release`
2. v0.2: local path dependencies
3. v0.3: `.cbox` creation and consumption
4. v0.4: dynamic-library projects and dynamically linked boxes on macOS,
   Linux, and Windows
5. v0.5: compatible local box caching
6. v0.6: Git dependencies
7. v0.7: lock file generation and reproducible resolution
8. v0.8: imported vendor SDK and precompiled binary boxes
9. v0.9: reproducible runtime assembly for executables and releases
10. v0.10: declarative build, test, release, and publication workflows

The dependency-management roadmap aims to replace scattered source
submodules, copied libraries, implicit linker configuration, and manually
maintained runtime directories with one declared dependency graph. Recipes
will state whether dependencies link statically or dynamically, while lock
files will record exact source revisions, box identities, and checksums.

Imported binary recipes will describe vendor SDKs and other projects that
cannot be built by Forge. These recipes will map headers, static libraries,
dynamic libraries, import libraries, and runtime binaries into verified boxes.
Forge will then assemble the required runtime libraries automatically when
building, running, and releasing an executable.

The `imported_library` profile packages target-specific local headers and
precompiled artifacts without invoking a compiler. Consumers link every
contained static or import library and stage every dynamic-library runtime.

Compiled boxes carry a strict toolchain identity: compiler identifier, exact
compiler version, C++ standard, build configuration, and standard-library or
runtime ABI. Builds reject compiled dependencies whose identity differs from
the actual consuming CMake toolchain. Lockfiles continue to pin the complete
box checksum, which transitively pins its embedded toolchain identity.

Workflow support will absorb repeated CI and release glue currently maintained
by projects such as Termin8or. The intended surface includes build/test
matrices, locked and development dependency modes, release asset selection,
release-note extraction, version generation, checksums, tags, publication, and
declarative runtime asset staging for build, run, boxes, and release.
Actions that modify Git history or publish remotely must remain explicit and
reviewable.

## Deferred decisions

- Extended `.cbox` compatibility identity and deterministic archive rules
- Extended binary compatibility identity beyond the initial strict toolchain
  fields
- Version constraint syntax
- Registry protocol and shared binary cache behavior
- Vendor SDK redistribution rules
- Generic pre-build and post-build hooks, if declarative Forge features do not
  cover the concrete use cases
