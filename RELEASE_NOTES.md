# Release notes

## 0.5.0

- Added the first multi-project workspace milestone: `forge.workspace.toml`
  groups independent Forge projects, validates their local dependency graph,
  and supports workspace-wide or selected-project builds, selected project
  runs, and aggregated workspace tests.
- Added persistent project and named-target preprocessor definitions plus
  repeatable temporary `forge build --define=<symbol>` additions.
- Made `forge adopt` import common `.vcxproj` metadata and turn supported
  Visual Studio `.sln` files into Forge workspaces with local project
  dependencies.
- Added build profiles for configuration-specific C++ standards, include
  directories, and definitions; `forge adopt` generates them from Visual
  Studio configurations and recursively imported concrete `.props` files.
- Made `forge adopt` import concrete CMake and Xcode project metadata,
  configuration-matched `.xcconfig` files, and safely merge mirrored native
  and CMake definitions while ignoring CMake-generated IDE projects. Concrete
  target-free CMake superprojects become Forge workspaces.
- Added concise phase and per-project progress reporting to `forge adopt`.

## 0.4.0

- Made `forge adopt` the primary command for adopting existing projects while
  retaining `forge init` as a compatibility alias.
- Added pinned Git source dependencies using exact full commit IDs, with
  shallow project-local checkout caching and normal transitive source
  dependency builds.
- Added named development and pinned dependency profiles selectable by
  `forge build`, `forge run`, and `forge test`.
- Made `forge adopt` infer executable, multi-executable, static-library, and
  header-only recipes from existing sources, public headers, and `main()`
  entry points.
- Renamed Forge's process helper to avoid shadowing the Windows system
  `<process.h>` header.
- Made generated Linux release workflows publish modern and legacy
  compatibility builds from `ubuntu-latest` and `ubuntu-22.04`.
- Made `forge adopt` infer local include search roots from resolvable
  `#include` directives.
- Made `forge adopt` use local include relationships to group implementation
  files into inferred executable targets.
- Made `forge adopt` report unresolved library-looking includes as dependency
  candidates while excluding known standard and platform headers.
- Made `forge adopt` infer unambiguous local path dependencies from sibling
  single-target Forge libraries.
- Added non-destructive GitHub dependency suggestions and explicit
  `forge adopt --github` verification with exact Git commit pins.
- Made Windows release workflows explicitly initialize and select MSVC,
  preventing released executables from accidentally using an incompatible
  compiler runtime found on the hosted runner.

## 0.3.0

- Added target-specific `imported_library` recipes that package local vendor
  headers and precompiled static, dynamic, and import libraries without
  invoking a compiler, then link and stage every contained artifact when
  consumed as dependencies.
- Added strict compiled-box toolchain identities covering compiler, exact
  compiler version, C++ standard, configuration, and runtime ABI, with
  incompatible dependencies rejected before linking.
- Added declarative executable runtime assets that Forge stages for builds and
  runs and includes automatically in executable boxes and releases.
- Added backward-compatible named targets with `forge build <target>` and
  `forge run <target> -- [arguments...]`.
- Added transitive internal dependencies between named library and executable
  targets, with missing-target and cycle validation.
- Added `forge test [target]` for building and running marked named test
  executables with argument forwarding and aggregate results.
- Added named-target boxing and release preparation, including recursively
  embedded internal dependency boxes and staged internal dynamic libraries.

## 0.2.0

- Added verified direct local `.cbox` dependencies through `{ box = "..." }`.
- Added checksum-verified downloadable `.cbox` dependencies with local caching.
- Added GitHub Release `.cbox` dependency shorthand with target-aware asset
  resolution.
- Added authoritative target-specific `forge.lock.toml` files and explicit
  `forge update [dependency]` refreshes for reproducible GitHub dependencies.
- Added self-contained format-2 boxes that embed and recursively resolve their
  transitive dependency boxes.
- Added compatible local source-dependency box caching. Forge reuses verified
  boxes when their package identity, target, declared inputs, and direct
  dependency checksums still match, while changed dependencies invalidate
  dependent boxes.
- Renamed the public dynamic-linking project type to `dynamic_library`, while
  preserving `shared_library` as a legacy recipe and box alias.
- Added Windows dynamic-library builds and boxes with separate DLL runtime and
  import-library artifacts, automatic dependent linking, and DLL staging.
- Added `forge box publish <box>` for verified local publication and checksum
  generation.
- Added `forge prepare-release` for type-aware hosted release assets:
  target-qualified ZIPs for executables and published `.cbox` files with
  checksums for libraries.
- Simplified `forge release-git` so its default invocation creates and pushes
  the conventional `release-<version>` tag.
- Added `forge bump major|minor|patch` for updating the recipe version,
  incrementing an existing build number, and preparing matching release notes.

## 0.1.0

- Added `forge new` and `forge init` for creating and adopting C++ projects
  without imposing a source-directory layout.
- Added recipe-driven builds for executables, static libraries, shared
  libraries on macOS and Linux, and validated header-only libraries.
- Added `forge run` for incremental builds and argument-forwarding execution.
- Added recursive local path dependencies with cycle detection, transitive
  linking, and runtime shared-library assembly.
- Added verified `.cbox` creation, inspection, verification, and extraction
  with multi-artifact packaging and SHA-256 checksums.
- Added application release archives with declarative extra contents and
  version-specific notes extracted from `RELEASE_NOTES.md`.
- Added optional monotonically increasing build numbers represented as SemVer
  build metadata.
- Added a hosted JSON Schema automatically associated with generated
  `forge.recipe.toml` files.
- Added Linux, macOS, and Windows GitHub release workflows generated by
  `forge new` and `forge init`.
- Added explicit Git releases through `forge release-git`, with custom tag
  formats, release-note annotations, and deliberate forced tag replacement,
  while keeping `forge release` local-only.
- Fixed Linux runtime loading for transitive shared-library dependencies.
- Added `forge clean` for removing all generated project state under `.forge/`.
- Added runnable examples, unit and integration tests, CMake presets, and an
  Xcode project.
