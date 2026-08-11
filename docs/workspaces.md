# Forge workspaces

A Forge workspace groups independently defined Forge projects without merging
their recipes or source trees. It is the cross-platform equivalent of a Visual
Studio solution or a CMake superproject.

Create `forge.workspace.toml` in the common parent directory:

```toml
#:schema https://raw.githubusercontent.com/razterizer/forge/main/schemas/forge.workspace.schema.json

[workspace]
name = "game-suite"
projects = ["Core", "Termin8or", "Tools/MapEditor"]
```

Every listed directory must remain inside the workspace and contain its own
`forge.recipe.toml`. Project recipes remain the source of truth for project
names, targets, versions, dependencies, and build behavior.

`forge adopt` can generate this structure from a Visual Studio solution. Run it
from a directory containing one `.sln` and no root `.vcxproj`. Every C++
project must live in a distinct subdirectory inside the solution root.

Build the complete workspace from its root:

```sh
forge build
```

Forge validates the complete workspace dependency graph, rejects duplicate
projects and cycles, and builds every root project. Existing local path
dependencies recursively build their dependency closures, so shared libraries
are not built twice.

Build one project and its dependency closure:

```sh
forge build Termin8or
forge build Termin8or --profile=dev
```

Dependency and build profiles are applied while determining the workspace
graph and are forwarded to project builds.

Run a workspace project, or a named executable target within it:

```sh
forge build-and-run Termin8or
forge build-and-run Termin8or/examples -- --help
```

The project selection is mandatory for `forge run` and `forge build-and-run`
because a workspace may contain several executable projects. Use
`<project>/<target>` when selecting a named target.

Run every marked test target across the workspace:

```sh
forge test
forge test Termin8or
forge test Termin8or/unit_tests -- --quick
```

Workspace-wide tests skip projects that contain no marked test targets,
continue through project test failures, and report an aggregate project
summary.

Clean removes generated state from every project when run from the workspace
root:

```sh
forge clean
```

Releases remain project-scoped. Adoption can create a workspace from a Visual
Studio solution without changing the workspace format.
