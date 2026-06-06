# forge

> Write C++, write a recipe, Forge the rest.

A project workflow system that eliminates build and release glue from C++ projects.

## Status

Forge is currently in the design and prototyping phase.

## Motivation

Modern C++ projects often require significant amounts of glue:

* Build scripts
* Dependency management
* Release packaging
* Runtime dependency handling
* CI plumbing

Much of this complexity is unrelated to the software being built.

Forge aims to reduce this accidental complexity so developers can focus on writing C++.

## Plan

**Goals**

* ✓ Build projects
* ✓ Run projects
* ✓ Package projects
* ✓ Release projects

* ✓ Header-only dependencies
* ✓ Source dependencies
* ✓ Static libraries
* ✓ Dynamic libraries on macOS and Linux
* Imported vendor SDKs and precompiled binaries
* Reproducible runtime dependency assembly

* ✓ Recipe-driven workflow

**Non-goals (for now)**

* ✗ Replace CMake
* ✗ Replace compilers
* ✗ Replace IDEs
* ✗ Solve every C++ packaging problem

## Development

Forge is beginning as a dependency-free C++20 command-line application. CMake
is its first build backend, and packaged C++ artifacts are called **boxes** and
use the `.cbox` extension.

Requirements:

* CMake 3.25 or newer
* A C++20 compiler
* Ninja

Build and test:

```sh
cmake --preset dev
cmake --build --preset dev
ctest --preset dev
```

Run only fast unit tests or external-tool integration tests:

```sh
ctest --preset dev -L unit
ctest --preset dev -L integration
```

Run:

```sh
./build/dev/forge --help
```

Forge can also build and release itself using the root `forge.recipe.toml`:

```sh
./build/dev/forge build
./build/dev/forge release
```

The self-hosted executable is written to `.forge/build/forge`, and the release
archive is written to `.forge/release/forge-0.1.0.zip`.

Runnable and packageable sample projects are available under
[`examples/`](examples/).

Create a new C++ project:

```sh
./build/dev/forge new hello
```

This creates `hello/forge.recipe.toml` and `hello/main.cpp`. Forge refuses to
overwrite an existing path.

Adopt an existing C++ project:

```sh
cd path/to/project
/path/to/forge/build/dev/forge init
```

`forge init` creates only `forge.recipe.toml`. It discovers existing `.cpp`,
`.cc`, and `.cxx` files without moving or modifying project sources.

Build a Forge project:

```sh
cd path/to/project
/path/to/forge/build/dev/forge build
```

Forge builds executable, static-library, and shared-library projects with
exact source paths. Libraries declare public headers under `include/`:

```toml
[project]
name = "hello"
version = "1.0.0"
type = "static_library"
cpp_std = 20

[sources]
paths = ["src/hello.cpp"]
public_headers = ["include/hello/hello.h"]
```

Forge generates CMake infrastructure under `.forge/generated/` and builds into
`.forge/build/`.

Remove all generated project state, including builds, dependencies, boxes,
release artifacts, and caches:

```sh
forge clean
```

For safety, `forge clean` only runs from a directory containing
`forge.recipe.toml`.

Shared libraries use the same source and public-header layout:

```toml
[project]
name = "hello"
version = "1.0.0"
type = "shared_library"
cpp_std = 20

[sources]
paths = ["src/hello.cpp"]
public_headers = ["include/hello/hello.h"]
```

Shared-library dependencies are boxed with their runtime artifact, installed
under `.forge/deps/`, and copied into `.forge/build/runtime`. Forge-generated
binaries use an origin-relative runtime search path, and `forge release`
includes the runtime directory. Shared libraries currently support macOS and
Linux; Windows DLL and import-library support remains planned.

Header-only projects declare no source files:

```toml
[project]
name = "hello"
version = "1.0.0"
type = "header_only"
cpp_std = 20

[sources]
paths = []
public_headers = ["include/hello/hello.h"]
```

Forge generates and compiles one private validation translation unit per public
header. These temporary sources remain under `.forge/generated/`; header-only
boxes contain only the declared headers.

Projects may use local static-library, shared-library, and header-only
dependencies:

```toml
[dependencies]
answer = { path = "../answer" }
format = { path = "../format" }
```

Forge builds each dependency into a verified box, installs it under
`.forge/deps/<name>/`, adds its public include directory, and links static
libraries when present. Dependencies are resolved recursively, shared
dependencies are built once per command, and dependency cycles are rejected.

Projects may also consume an existing local box directly:

```toml
[dependencies]
answer = { box = "../packages/answer-1.0.0-macos-arm64.cbox" }
```

Forge verifies the box, checks its package name and target, installs its exact
declared artifacts under `.forge/deps/answer/`, and imports its headers and
library. Format-1 boxes do not yet record dependency graphs, so direct boxes
must currently be self-contained leaf dependencies.

Boxes may also be downloaded from a URL with an explicit external checksum:

```toml
[dependencies]
answer = {
  url = "https://github.com/example/answer/releases/download/release-1.0.0/answer-1.0.0-macos-arm64.cbox",
  sha256 = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"
}
```

Forge downloads the box through CMake into `.forge/cache/downloads/`, verifies
the expected SHA-256 before opening it, and then applies normal box validation
and installation. The checksum is the immutable cache key.

### Local dependency example

A workspace may contain a static library, a header-only library, and an
executable that uses both:

```text
workspace/
├── answer/
│   ├── forge.recipe.toml
│   ├── include/answer/answer.h
│   └── src/answer.cpp
├── doubled/
│   ├── forge.recipe.toml
│   └── include/doubled/doubled.h
└── calculator/
    ├── forge.recipe.toml
    └── main.cpp
```

The `answer` static-library recipe is:

```toml
[project]
name = "answer"
version = "1.0.0"
type = "static_library"
cpp_std = 20

[sources]
paths = ["src/answer.cpp"]
public_headers = ["include/answer/answer.h"]
```

The `doubled` header-only recipe is:

```toml
[project]
name = "doubled"
version = "1.0.0"
type = "header_only"
cpp_std = 20

[sources]
paths = []
public_headers = ["include/doubled/doubled.h"]
```

The `calculator` executable declares both dependencies:

```toml
[project]
name = "calculator"
version = "1.0.0"
type = "executable"
cpp_std = 20

[sources]
paths = ["main.cpp"]

[dependencies]
answer = { path = "../answer" }
doubled = { path = "../doubled" }
```

Dependency keys must match the dependency recipes' project names. The
application may then include both libraries normally:

```cpp
#include <answer/answer.h>
#include <doubled/doubled.h>

#include <iostream>

int main()
{
  std::cout << doubled(answer()) << '\n';
}
```

Run the application from its project directory:

```sh
cd workspace/calculator
/path/to/forge run
```

Libraries declare their own dependencies using the same syntax. Dependency
paths are relative to the project that declares them, and Forge recursively
installs the complete dependency closure for the final application:

```toml
# answer/forge.recipe.toml
[dependencies]
doubled = { path = "../doubled" }
```

Build and run a Forge project:

```sh
/path/to/forge/build/dev/forge run
/path/to/forge/build/dev/forge run --message "hello world"
```

`forge run` performs an incremental build, forwards its remaining arguments to
the executable, and returns the executable's exit status.

Create a release archive:

```sh
/path/to/forge/build/dev/forge release
```

`forge release` builds the project, stages the executable with root-level
`README.md` and `LICENSE` files when present, and creates
`.forge/release/<name>-<version>.zip`.

When `RELEASE_NOTES.md` exists, Forge requires a `## <version>` section matching
the recipe version. It packages only that section and writes a copy to
`.forge/release/RELEASE_NOTES.md` for hosted release publication:

```markdown
# Release notes

## 0.2.0

- Added something useful.

## 0.1.0

- Initial release.
```

Additional release files and directories can be declared in the recipe:

```toml
[release]
files = ["RELEASE_NOTES.md", "assets", "examples"]
```

Forge copies declared entries recursively while preserving their
project-relative paths. Paths must remain inside the project, and symbolic
links are rejected.

`forge new` and `forge init` create `RELEASE_NOTES.md` and Linux, macOS, and
Windows release workflows under `.github/workflows`. Pushing a `release-*` or
`v*` tag builds Forge, runs `forge prepare-release`, and publishes the resulting
artifacts to the matching GitHub Release. Executable projects produce a
target-qualified ZIP archive. Static-library, shared-library, and header-only
projects produce a target-qualified `.cbox` and its `.sha256` checksum under
`boxes/`. Existing workflow files and release notes are never overwritten by
`forge init`. Generated `.gitignore` files exclude Forge build state. Tag
creation remains an explicit opt-in action.

Prepare the same hosted release assets locally:

```sh
forge prepare-release
```

The command also writes the focused release notes used by GitHub Actions to
`.forge/release/RELEASE_NOTES.md`. It performs the necessary build, box
creation, verification, and local publication steps automatically. You do not
need to run those commands individually before a release.

Trigger the generated GitHub release workflows by creating and pushing an
annotated Git release tag:

```sh
forge release-git
forge release-git --tag="<name>-<version>+build.<build-nr>"
```

`forge release` remains a local-only build and packaging command.
`forge release-git` does not build locally; it pushes a tag that causes GitHub
Actions to prepare and publish the platform assets. Its default tag is
`release-<version>`. Custom formats support `<name>`,
`<version>`, `<build-nr>`, `<curr-date>`, `<target>`, and `<configuration>`.
`<build-nr>` requires `[build].number`; `<curr-date>` uses the current UTC
date; `<target>` is the current OS and architecture; and `<configuration>` is
`release`.

GitHub releases require a Git repository with clean tracked and staged state
and reject invalid or existing local tags. Forge creates an annotated tag from
the matching release notes and pushes it to `origin`. If pushing fails, the
local tag remains for inspection or a manual retry.

To deliberately replace an existing local and remote tag, use
`forge release-git --tag-force`. This rewrites published Git history and should
normally only be used to repair a broken release.

Generated GitHub workflows react to `release-*` and `v*`. A custom tag format
must match one of those patterns, or the generated workflow triggers must be
customized, to publish hosted artifacts.

Create, inspect, verify, publish locally, and extract an executable,
static-library, shared-library, or header-only box:

```sh
/path/to/forge/build/dev/forge box create
/path/to/forge/build/dev/forge box inspect .forge/boxes/hello-0.1.0-macos-arm64.cbox
/path/to/forge/build/dev/forge box verify .forge/boxes/hello-0.1.0-macos-arm64.cbox
/path/to/forge/build/dev/forge box publish .forge/boxes/hello-0.1.0-macos-arm64.cbox
/path/to/forge/build/dev/forge box extract .forge/boxes/hello-0.1.0-macos-arm64.cbox
```

`forge box publish <box>` verifies and publishes the box locally by copying it
into the project-root `boxes/` directory and writing a sibling `.sha256` file
suitable for a later hosted release or copying into a dependency recipe.
Republishing identical contents is safe; Forge refuses to overwrite a
same-named box with different contents. Publishing must run from a directory
containing `forge.recipe.toml`.

Projects may specify an optional build number without changing their dependency
version:

```toml
[build]
number = 6
```

Forge packages version `3.0.0`, build `6` as
`<name>-3.0.0+build.6-<os>-<arch>.cbox`.

See [docs/cbox-format.md](docs/cbox-format.md) for the implemented format.

Forge provides a JSON Schema for recipe validation and editor completion at
[`schemas/forge.recipe.schema.json`](schemas/forge.recipe.schema.json). See
[docs/recipe-schema.md](docs/recipe-schema.md) for editor integration.
`forge new` and `forge init` automatically associate generated recipes with the
hosted schema.

See [docs/design.md](docs/design.md) for the current design baseline and
roadmap.
