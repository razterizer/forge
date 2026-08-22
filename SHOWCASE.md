# Showcase

These are end-to-end, real-world Forge adoption walkthroughs. The first packages the
[EnTT](https://github.com/skypjack/entt) entity-component-system library as a
header-only `.cbox`, then builds and runs the
[EnTT-Pacman](https://github.com/indianakernick/EnTT-Pacman) game against that
box.

It demonstrates three related Forge capabilities working together:

1. importing an existing CMake header-only library;
2. consuming the resulting cbox from another adopted project; and
3. recognizing SDL2 from CMake and offering the declared macOS system-package
   provider during an interactive build.

The walkthroughs were verified on macOS arm64 with AppleClang and Homebrew.

## Validated no-tinkering suite

The walkthroughs below include historical compatibility and package-selection
steps, so they are not all candidates for unattended adoption validation. The
separate suite records only demos that must work from a clean checkout without
editing a generated recipe. After building Forge, run it directly from this
checkout:

```sh
scripts/validate-showcases.sh \
  --forge /path/to/forge/build/dev/forge \
  --run
```

The runner clones pinned Raylib 6.0, Meshoptimizer 1.2, fmt 12.2.0, and
spdlog 1.17.0 sources into a fresh temporary sandbox, then removes it when
validation finishes. It never changes an existing checkout; pass `--keep` to
retain the sandbox for inspection.

### Registered examples

| Showcase | Validation | Package output |
| --- | --- | --- |
| Raylib 6.0 | Builds `textures_tiled_drawing` and `audio_sound_loading`, verifies staged resources, and runs them with `--run` | Runtime demos; no separate showcase cbox |
| Meshoptimizer 1.2 | Adopts and builds the static library | Creates and verifies a cbox |
| fmt 12.2.0 | Adopts and builds the static library | Creates and verifies a cbox |
| spdlog 1.17.0 | Adopts and builds the static library | Creates and verifies a cbox |

Add a showcase to the script only after it satisfies the no-tinkering adoption
gate.

## Package a native dependency chain for a Blender extension

[meshoptimizer](https://github.com/zeux/meshoptimizer),
[AssetKit](https://github.com/recp/AssetKit), and
[assetkit-blender](https://github.com/recp/assetkit-blender) form a useful
three-level validation chain:

```text
meshoptimizer (C++ static library)
        ↓
AssetKit (C/C++ static library with bundled CMake subprojects)
        ↓
assetkit-blender (C Python extension module)
```

It demonstrates sibling dependency discovery, component boxes with embedded
dependencies, C and C++ compilation in one chain, private build-header
propagation, Python development-header discovery, and preservation of a
`Python3_add_library(... WITH_SOABI)` module's package location.

### Prepare and adopt the producers

Clone the repositories side by side, then initialise AssetKit's declared
submodules before adoption:

```sh
mkdir -p ~/source/github
cd ~/source/github
git clone https://github.com/zeux/meshoptimizer.git
git clone --recurse-submodules https://github.com/recp/AssetKit.git
git clone https://github.com/recp/assetkit-blender.git

cd AssetKit
git submodule update --init --recursive
```

Adopt and box Meshoptimizer first:

```sh
cd ~/source/github/meshoptimizer
/path/to/forge/build/dev/forge adopt
/path/to/forge/build/dev/forge box create
```

Then adopt AssetKit. Its generated recipe recognises the neighbouring
Meshoptimizer checkout as the `meshoptimizer` sibling dependency. Build and
package the actual `assetkit` component, rather than the aggregate project
box:

```sh
cd ~/source/github/AssetKit
/path/to/forge/build/dev/forge adopt
/path/to/forge/build/dev/forge box create assetkit
```

The selected box contains AssetKit and its CMake subproject dependencies, such
as `ds` and `libdeflate_static`, as embedded boxes. This makes the package
self-contained for a downstream project without flattening its dependency
identity.

### Adopt and build the Blender module

Adopt the consumer from its repository root:

```sh
cd ~/source/github/assetkit-blender
/path/to/forge/build/dev/forge adopt
/path/to/forge/build/dev/forge build
```

Forge discovers the sibling `assetkit` project, resolves its component box and
embedded dependencies, and finds the Python development-module headers through
`find_package(Python3 COMPONENTS Development.Module)`. A successful macOS
build ends with a Python importable native module in the original package
directory, rather than a generic library under `.forge/build/`:

```text
Built .../assetkit-blender/src/assetkit_blender/_assetkit_blender.cpython-313-darwin.so
```

Finally, package the module itself:

```sh
/path/to/forge/build/dev/forge box create
```

This produces a target-qualified box such as
`.forge/boxes/assetkit_blender_native-0.1.0-macos-arm64.cbox`. The box carries
the compiled extension artifact and the resolved AssetKit dependency closure.
The Python package's `.py` files remain source-managed by the Blender add-on;
the cbox validates and distributes its native artifact graph.

## Package EnTT, then run EnTT-Pacman

## Prepare the projects

Clone the projects side by side. Forge's adoption pass can then identify the
EnTT checkout as a sibling dependency of EnTT-Pacman.

```sh
mkdir -p ~/source/github
cd ~/source/github
git clone https://github.com/skypjack/entt.git
git clone https://github.com/indianakernick/EnTT-Pacman.git
```

Build Forge first, substituting its absolute path in the commands below:

```sh
cd /path/to/forge
cmake --preset dev
cmake --build --preset dev
```

## Adopt and package EnTT

```sh
cd ~/source/github/entt
/path/to/forge/build/dev/forge adopt
/path/to/forge/build/dev/forge build
/path/to/forge/build/dev/forge box create
```

EnTT is a CMake interface library whose public headers live in `src/entt`.
Forge imports that source-root header layout and creates a header-only box such
as:

```text
.forge/boxes/EnTT-0.1.0-ho.cbox
```

The exact versioned filename is determined by the generated recipe. Inspect it
with `forge box inspect <box>`; the installed header tree belongs under
`include/entt/` in the box.

## Adopt EnTT-Pacman and select the cbox

```sh
cd ~/source/github/EnTT-Pacman
/path/to/forge/build/dev/forge adopt
```

The adoption report should say that it imported platform link requirements and
found EnTT as a sibling project. Its generated recipe contains a local-source
EnTT dependency, which is useful for editing both projects but does not test
the package. Replace that dependency with the EnTT cbox, using the filename
created above:

```toml
[dependencies]
EnTT = { box = "../entt/.forge/boxes/EnTT-0.1.0-ho.cbox" }
```

Remove the sibling-source dependency section. Also remove `"third_party"`
from the generated `include_dirs` if present, leaving `"src"`; this ensures
the build cannot accidentally use EnTT-Pacman's vendored EnTT copy. Keep the
generated `[build]` section. It contains the portable SDL2 requirements:

```toml
[build]
macos_libraries = ["SDL2"]
macos_brew_packages = ["sdl2-compat"]
linux_libraries = ["SDL2"]
linux_apt_packages = ["libsdl2-dev"]
windows_libraries = ["SDL2"]
```

The provider mapping is explicit project metadata. Forge currently recognizes
this known SDL2 CMake package; it does not guess arbitrary package-manager
names from unresolved `#include` directives.

## Build and install the declared provider

Run an interactive build:

```sh
/path/to/forge/build/dev/forge build
```

On a macOS host without SDL2, Forge asks before changing the system:

```text
forge: missing system library SDL2
Install provider with: brew install sdl2-compat ? [y/N]
```

Answer `y` to approve the installation. Forge then obtains the Homebrew prefix
and supplies its include and library paths to CMake. Non-interactive and CI
builds never install system packages; their recipe remains reproducible and
can be provisioned by the host setup instead.

## Update the older EnTT-Pacman API calls

EnTT-Pacman uses an older EnTT API. With a current EnTT cbox, make these
mechanical source updates in the game checkout:

```cpp
reg.has<Component>(entity)              // becomes reg.all_of<Component>(entity)
reg.remove_if_exists<A, B, C>(entity)   // becomes reg.remove<A, B, C>(entity)
```

The changes apply in `src/sys/can_move.cpp`, `change_ghost_mode.cpp`,
`player_ghost_collide.cpp`, and `render.cpp`. They are a project compatibility
migration, not a change to the EnTT box or to Forge.

Rebuild and run:

```sh
/path/to/forge/build/dev/forge build
.forge/build/EnTT_Pacman
```

At this point the game builds, links, and runs using the EnTT cbox. A build
command will show both the extracted package include root and the installed
SDL2 provider, for example:

```text
-I.../EnTT-Pacman/.forge/deps/EnTT/include
-isystem /opt/homebrew/opt/sdl2-compat/include/SDL2
```

## What Forge automated and what remained explicit

| Automated by Forge | Chosen by the project owner |
| --- | --- |
| CMake project and header-layout import | Pinning EnTT-Pacman to a particular local cbox |
| cbox extraction and public include propagation | Confirming a system package installation |
| SDL2 requirement import from `find_package(SDL2)` | Updating the game's legacy EnTT API calls |
| Homebrew prefix discovery and CMake path setup | Publishing a cbox for other machines, when desired |

This division is intentional: Forge automates repeatable build mechanics while
leaving version selection, system changes, and source compatibility decisions
visible in the project recipe and source tree.

## Build StellarEngine with system or local fmt and spdlog

[StellarEngine](https://github.com/Gellert5225/StellarEngine) is a larger CMake
superproject: its Metal editor depends on GLFW, EnTT, glm, nativefiledialog,
yaml-cpp, fmt, and spdlog; it also carries a vcpkg manifest and copies a
`Resources` tree next to the editor executable. It demonstrates that a project
can switch one adopted provider between the host package manager and editable
sibling source checkouts without rewriting its generated platform-link rules.

Clone the engine and the two libraries side by side. StellarEngine needs its
submodule, while spdlog's adopted recipe in this example depends on the
neighbouring fmt checkout:

```sh
mkdir -p ~/source/github
cd ~/source/github
git clone --recurse-submodules https://github.com/Gellert5225/StellarEngine.git
git clone https://github.com/fmtlib/fmt.git
git clone https://github.com/gabime/spdlog.git
```

Adopt the library checkouts first. The generated fmt and spdlog recipes select
their concrete CMake library target, so they do not pull their test and example
targets into the build:

```sh
cd ~/source/github/fmt
/path/to/forge/build/dev/forge adopt

cd ~/source/github/spdlog
/path/to/forge/build/dev/forge adopt
```

Then adopt the engine from its repository root:

```sh
cd ~/source/github/StellarEngine
/path/to/forge/build/dev/forge adopt
```

Forge creates a workspace with `Stellar` and `StellarEditor`. The editor recipe
also records the CMake `copy_directory` rule as `runtime.files = ["Resources"]`,
so its Metal shader and textures are staged beside the executable.

### System-provider flavour

The freshly adopted `Stellar/forge.recipe.toml` uses the platform requirements
in its generated `[build]` section. Leave it without a dependency-style
override and build from the workspace root:

```sh
/path/to/forge/build/dev/forge build
```

On macOS, Forge uses the declared Homebrew provider hints for fmt and spdlog
when they are needed, alongside the project's vcpkg manifest for dependencies
declared there. The relevant generated metadata remains visible and portable:

```toml
[build]
macos_libraries = ["fmt", "glfw", "nfd", "spdlog", "yaml-cpp"]
macos_brew_packages = ["entt", "fmt", "glfw", "glm", "nativefiledialog-extended", "spdlog", "yaml-cpp"]
```

### Local-source fmt and spdlog flavour

To replace the system fmt/spdlog pair with the neighbouring checkouts, add this
to `Stellar/forge.recipe.toml` after `[project]`:

```toml
[defaults]
style = "local-source"

[dependencies.style.local-source]
spdlog = { path = "../../spdlog", provides = ["spdlog"] }
```

Build again from the workspace root:

```sh
/path/to/forge/build/dev/forge build
```

`provides = ["spdlog"]` replaces the adopted `spdlog` provider and its
associated `fmt` provider hint. Forge consequently builds or reuses the local
spdlog box and its local fmt dependency, then extracts both into
`Stellar/.forge/deps/`. Typical confirmation looks like:

```text
Using cached dependency FMT
Using cached dependency spdlog
Extracted .../Stellar/.forge/deps/spdlog-1.17.0-macos-arm64
Extracted .../Stellar/.forge/deps/FMT-12.2.1-macos-arm64
Built workspace StellarEngine
```

This works even though the adopted fmt and spdlog projects request C++11 while
Stellar uses C++20: a consumer may use a newer language standard than a
compiled dependency, but Forge rejects the unsafe reverse relationship.

Finally run the editor with its workspace project selector:

```sh
/path/to/forge/build/dev/forge run StellarEditor
```

The application reaching its editor window verifies the generated workspace,
local provider override, compiled dependency chain, Metal/Objective-C++ build,
and runtime resource staging. StellarEngine's current resource drag-and-drop
crash is an upstream editor issue: it attempts to parse every dropped resource
as a YAML scene, rather than a Forge packaging or dependency-resolution issue.
