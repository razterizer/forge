# Workflow direction

Forge aims to replace repeated project-specific build and release scripts with
declarative, locally reproducible workflows.

The initial workflow inventory is based on patterns already used by Termin8or:

- Build and test on Linux, macOS, and Windows.
- Build against both development and locked dependencies.
- Build examples and unit tests as separate jobs.
- Package selected executables, assets, and platform instructions.
- Produce multiple compatibility variants, such as modern and legacy Linux
  binaries.
- Extract release notes for a version.
- Generate version headers.
- Create annotated release tags.
- Publish archives to a hosted release.

## Implemented

Release recipes may include additional project files and directories:

```toml
[release]
files = ["RELEASE_NOTES.md", "assets", "examples"]
```

Forge preserves project-relative paths and rejects paths outside the project
and symbolic links.

`forge release` extracts the recipe version's `## <version>` section from
`RELEASE_NOTES.md`. The focused notes are included in the archive and written
to `.forge/release/RELEASE_NOTES.md` for GitHub release publication. A present
notes file without a matching version section causes release to fail.

`forge new` and `forge init` generate thin GitHub Actions adapters:

- `.github/workflows/release-linux.yml`
- `.github/workflows/release-macos.yml`
- `.github/workflows/release-windows.yml`

Each workflow reacts to `release-*` and `v*` tags, bootstraps Forge, runs
`forge release`, gives the archive a platform-specific name, and publishes it
to the matching GitHub Release. Existing workflow and release-note files are
left unchanged. Tag creation remains an explicit opt-in action.

Tagged releases are opt-in:

```sh
forge release --tag
forge release --tag="release-<version>-<curr-date>"
```

The default tag is `release-<version>`. Formats may use `<name>`, `<version>`,
`<build-nr>`, `<curr-date>`, `<target>`, and `<configuration>`. Forge validates
the expanded tag and clean tracked Git state before and after building, then
creates an annotated tag from the extracted release notes and pushes it to
`origin`. Custom formats must match `release-*` or `v*`, or the generated
workflow triggers must be customized, to publish hosted artifacts.

## Planned

The next workflow slices should be:

1. Named build and test profiles.
2. Release variants and platform-specific release contents.
3. Generated release manifests and checksums.
4. Dry-run support for Git tagging.
5. Generated version headers.

Local commands and CI should execute the same Forge-defined workflow. CI files
should become thin adapters that install Forge and invoke the appropriate
profile.
