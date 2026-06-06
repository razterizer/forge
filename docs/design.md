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
that box.

Local path dependencies may be declared by executable, static-library,
shared-library, and header-only projects. Forge recursively builds library
dependencies, rejects cycles and conflicting project names, links the
transitive static-library closure in dependency order, and stages shared
libraries for running and releasing applications.

Local `.cbox` dependencies are verified and installed directly without their
source project. Format-1 boxes do not contain dependency graph metadata, so
direct boxes are currently treated as self-contained leaf dependencies.

## Initial roadmap

1. v0.1: `forge new`, `forge init`, `forge build`, `forge run`, and
   `forge release`
2. v0.2: local path dependencies
3. v0.3: `.cbox` creation and consumption
4. v0.4: shared-library projects and dynamically linked boxes on macOS and
   Linux; Windows DLL/import-library support remains
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
shared libraries, import libraries, and runtime binaries into verified boxes.
Forge will then assemble the required runtime libraries automatically when
building, running, and releasing an executable.

Workflow support will absorb repeated CI and release glue currently maintained
by projects such as Termin8or. The intended surface includes build/test
matrices, locked and development dependency modes, release asset selection,
release-note extraction, version generation, checksums, tags, and publication.
Actions that modify Git history or publish remotely must remain explicit and
reviewable.

## Deferred decisions

- Extended `.cbox` compatibility identity and deterministic archive rules
- Binary compatibility identity, including compiler and runtime ABI
- Version constraint syntax
- Registry protocol and shared binary cache behavior
- Windows DLL identity, runtime search paths, and import-library handling
- Imported binary recipe syntax and vendor SDK redistribution rules
