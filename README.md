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
* ✓ Dynamic libraries

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

Run:

```sh
./build/dev/forge --help
```

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

The first build implementation supports executable projects with exact source
paths. Forge generates CMake infrastructure under `.forge/generated/` and
builds into `.forge/build/`.

See [docs/design.md](docs/design.md) for the current design baseline and
roadmap.
