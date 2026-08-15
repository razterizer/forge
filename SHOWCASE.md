# Showcase: package EnTT, then run EnTT-Pacman

This is an end-to-end, real-world Forge adoption walkthrough. It packages the
[EnTT](https://github.com/skypjack/entt) entity-component-system library as a
header-only `.cbox`, then builds and runs the
[EnTT-Pacman](https://github.com/indianakernick/EnTT-Pacman) game against that
box.

It demonstrates three related Forge capabilities working together:

1. importing an existing CMake header-only library;
2. consuming the resulting cbox from another adopted project; and
3. recognizing SDL2 from CMake and offering the declared macOS system-package
   provider during an interactive build.

The walkthrough was verified on macOS arm64 with AppleClang and Homebrew.

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
