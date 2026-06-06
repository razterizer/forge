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

The first dependency-resolution slice supports direct local path dependencies
for executable projects. Dependencies must currently be static-library or
header-only projects without dependencies of their own.

## Initial roadmap

1. v0.1: `forge new`, `forge init`, `forge build`, `forge run`, and
   `forge release`
2. v0.2: local path dependencies
3. v0.3: `.cbox` creation and consumption
4. v0.4: Git dependencies
5. v0.5: lock file generation and reproducible resolution

## Deferred decisions

- Extended `.cbox` compatibility identity and deterministic archive rules
- Binary compatibility identity, including compiler and runtime ABI
- Version constraint syntax
- Registry protocol and shared binary cache behavior
