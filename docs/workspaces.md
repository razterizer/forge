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

Dependency profiles are applied while determining the workspace graph and are
forwarded to project builds.

This first workspace milestone intentionally keeps releases, tests, runs,
adoption, and workspace generation project-scoped. Those commands can become
workspace-aware without changing the workspace format.
