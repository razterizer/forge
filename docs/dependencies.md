# Dependencies

Forge dependencies have two independent dimensions:

- **Source or package**: build a dependency from source, or consume a `.cbox`.
- **Local or remote**: read it from the filesystem, or retrieve it from a
  remote repository or release.

Profiles do not impose a dependency mode. A named dependency profile is a
complete dependency-set override and may use any supported dependency form.

## Dependency forms

| Form | Recipe fields | What Forge consumes | Lockfile required |
| --- | --- | --- | --- |
| Local source | `path` | An editable sibling Forge project | No |
| Remote source | `git`, `commit` | An exact detached Git checkout | No |
| Local package | `box` | An existing local `.cbox` | No |
| Direct remote package | `url`, `sha256` | A downloadable `.cbox` with an explicit checksum | No |
| GitHub Release package | `github`, `version` | A conventionally named release `.cbox` | Yes |

Every source dependency is built into and consumed through a verified box.
The distinction is whether Forge observes and builds the dependency sources or
starts from an already packaged artifact.

Dependency entries may include a `targets` array to restrict them to specific
platform targets:

```toml
[dependencies]
3rdparty_OpenAL = { path = "../3rdparty_OpenAL", targets = ["windows-x86_64"] }
```

Forge ignores target-filtered dependencies on other platforms when resolving,
building, and packaging boxes. This is useful for adapters that use a packaged
SDK on one platform and a system-installed SDK on another.

### Local source

Use an editable sibling checkout during active development:

```toml
[dependencies]
Core = { path = "../Core" }
```

Forge rebuilds the dependency box when its recipe, sources, public headers, or
direct dependency boxes change.

### Remote source

Use an exact Git commit when the dependency should still be built from source
but its source revision must be reproducible:

```toml
[dependencies]
Core = {
  git = "https://github.com/razterizer/Core.git",
  commit = "<exact-full-commit>"
}
```

The exact commit in the recipe is the pin, so no lockfile entry is required.

### Local package

Use a local box to test or consume an already packaged artifact:

```toml
[dependencies]
Core = { box = "../packages/Core-1.5.0+build.8-ho.cbox" }
```

### Direct remote package

Use a URL and explicit checksum for a remotely hosted box that does not follow
Forge's GitHub Release conventions:

```toml
[dependencies]
Core = {
  url = "https://example.invalid/Core-1.5.0+build.8-ho.cbox",
  sha256 = "<lowercase-sha256>"
}
```

The explicit checksum is the immutable pin.

### GitHub Release package

Use the repository and packaged version for a box published by a Forge release
workflow:

```toml
[dependencies]
Core = { github = "razterizer/Core", version = "1.5.0+build.8" }
```

Run `forge update Core` to resolve the release asset for the current target and
write its exact URL, checksum, package identity, and selected component to
`forge.lock.toml`. Normal builds require the matching lock entry and do not
re-resolve GitHub.

## Dependency profiles

Profile names such as `dev` and `pinned` are conventions chosen by the project.
They have no built-in dependency semantics. Each profile may contain any of the
dependency forms above.

A common workflow uses editable sibling sources for development and released
packages for reproducible builds:

```toml
[profile.dev.dependencies]
Core = { path = "../Core" }

[profile.pinned.dependencies]
Core = { github = "razterizer/Core", version = "1.5.0+build.8" }
```

Resolve the GitHub Release package once for the current target:

```sh
forge update Core --profile=pinned
```

Then select the desired dependency set for each command:

```sh
forge build --profile=dev
forge test --profile=dev

forge build --profile=pinned
forge test --profile=pinned
```

Commit `forge.lock.toml` after updating remotely resolved GitHub Release
packages. Compiled package entries are recorded per target, allowing Linux,
macOS, and Windows resolutions to coexist. Portable header-only packages are
recorded once with `target = "any"` and reused on every host.

## Shared transitive dependencies

Forge installs one dependency for each package identity in the selected target
and profile. If two branches of the dependency graph require the same package,
their exact versions must agree. Forge reuses the single resolved package when
they match, and rejects the graph when they do not instead of silently choosing
the newest version.

For example, a consumer may depend on both `Termin8or` and `8Beat` when both
resolve to the same `Core` version. If one branch requires `Core 1.5.0+build.8`
and another requires `Core 1.6.0+build.1`, Forge reports a dependency conflict
so the application can upgrade or pin its dependency graph intentionally.

## Choosing a form

- Use **local source** while changing both projects together.
- Use **remote source** when reproducibly building a dependency from an exact
  source revision.
- Use a **local package** when testing exactly what was packaged.
- Use a **direct remote package** for a box hosted outside Forge's GitHub
  Release conventions.
- Use a **GitHub Release package** when consuming a published Forge library
  release.

For box layout and validation rules, see [cbox-format.md](cbox-format.md).
For the complete recipe surface, see [recipe-schema.md](recipe-schema.md).
