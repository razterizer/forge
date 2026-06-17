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

`forge bump major|minor|patch` prepares the next release by updating the
recipe's semantic version and adding a matching topmost section to
`RELEASE_NOTES.md`. Existing build numbers are incremented. Bumping does not
build, tag, or publish anything.

`forge release` extracts the recipe version's `## <version>` section from
`RELEASE_NOTES.md`. The focused notes are included in the archive and written
to `.forge/release/RELEASE_NOTES.md` for GitHub release publication. A present
notes file without a matching version section causes release to fail.

Set `[release].build_number_format = "dotted"` alongside `[build].number` to
make `forge bump` generate and release commands extract headings such as
`## 1.4.0.7`. Use `"semver"` for `## 1.4.0+build.7`. The default remains the
version-only heading. The configured form also becomes the default Git release
tag, such as `release-1.4.0.7`.

`forge new` and `forge adopt` generate thin GitHub Actions adapters:

- `.github/workflows/release-linux.yml`
- `.github/workflows/release-macos.yml`
- `.github/workflows/release-windows.yml`

Each workflow reacts to `release-*` and `v*` tags, bootstraps Forge, runs
`forge workflow prepare-release`, and publishes its output to the matching GitHub
Release. Executable projects produce a target-qualified ZIP archive.
Static-library, dynamic-library, and header-only projects produce a
target-qualified `.cbox` and its `.sha256` checksum under `boxes/`. Existing
workflow and release-note files are left unchanged. Tag creation remains an
explicit opt-in action.

The Windows adapter explicitly initializes the x64 MSVC developer environment
and selects `cl` for its bootstrap build. The same environment is inherited by
`forge workflow prepare-release`, ensuring released Windows artifacts use MSVC rather
than an incidental compiler from the hosted runner.

The Linux workflow builds two variants before publishing:

- `linux-modern` uses `ubuntu-latest`.
- `linux-legacy` uses `ubuntu-22.04`.

Building the legacy variant on the older runner gives it an older glibc and
GNU C++ runtime compatibility baseline. Asset names include the variant so both
builds can coexist in one GitHub Release. Portable header-only cboxes retain
their natural `-ho.cbox` filename and are uploaded once by the modern job; the
legacy job skips them.

GitHub releases are explicit and separate from local releases:

```sh
forge release
forge release examples
forge workflow prepare-release
forge workflow prepare-release examples
forge release-git
forge release-git --tag="release-<version>-<curr-date>"
```

`forge release` builds and packages only on the local machine.
`forge workflow prepare-release` prepares the type-appropriate artifacts and
focused release notes expected by hosted release workflows. It performs the
necessary build, box creation, verification, and local publication steps
automatically.
Multi-target projects prepare each library target as its natural cbox and each
non-test executable target as a platform archive. Marked test executables are
not published. Selecting a target prepares only that target's hosted asset.
Use explicit `forge box create` when a complete format-3 aggregate container is
wanted.
`forge release-git` does not build locally; it creates and pushes the tag
that triggers the generated platform workflows. The default tag is
`release-<version>`. Build-qualified releases expand `<version>` using their
configured dotted or SemVer form. Formats may use `<name>`, `<version>`, `<build-nr>`,
`<curr-date>`, `<target>`, and `<configuration>`. Forge validates the expanded
tag and clean tracked Git state, then creates an annotated tag from the
matching release notes and pushes it to `origin`. Custom formats must match
`release-*` or `v*`, or the generated workflow triggers must be customized, to
publish hosted artifacts.

For a normal hosted release, only `forge release-git` is required. The
generated workflows invoke `forge workflow prepare-release` on each platform.
Running `forge workflow prepare-release` locally is useful for inspecting the
artifacts before tagging, while the individual box commands remain useful for
diagnostics and manual local publication.

`forge prepare-release` remains available as a deprecated compatibility alias.

## Existing workflows

Add cbox publication to an existing custom workflow without replacing its
user-owned jobs:

```sh
forge workflow list-features
forge workflow status --file=.github/workflows/release-linux.yml

forge workflow add-feature release-boxes \
  --file=.github/workflows/release-linux.yml

forge workflow add-feature release-boxes \
  --file=.github/workflows/release-linux.yml \
  --apply
```

Preview is the default. Applying injects a self-contained
`forge-release-boxes` job marked with `# forge-managed: release-boxes@2`.
The job runs only for Git tag refs, even if the containing workflow has broader
triggers. It resolves and checks out the latest published Forge release before
preparing boxes. Repeated application is safe. Forge refuses to overwrite an existing
`forge-release-boxes` job that lacks its managed metadata.

Inspect and maintain an injected feature as Forge evolves:

```sh
forge workflow status --file=.github/workflows/release-linux.yml
forge workflow update-feature release-boxes \
  --file=.github/workflows/release-linux.yml \
  --apply
forge workflow remove-feature release-boxes \
  --file=.github/workflows/release-linux.yml \
  --apply
```

Status reports `missing`, `current`, `outdated`, or `unmanaged collision`.
Update and remove also preview by default and touch only a job carrying the
matching Forge-managed marker.

`forge release-git --tag-force` deliberately replaces the existing local and
remote release tag. Use it only when repairing a broken published release.

## Remaining workflow roadmap

1. Keep `[profile.workflow-release.dependencies]` as the reproducible place for
   release-only dependency replacements.
2. Make adoption leave clear TODOs for local dependencies that need workflow
   replacements before hosted release jobs can reproduce them.
3. Harden cross-target locking, including commands such as
   `forge update dep --profile=workflow-release --target=windows-x86_64`.
4. Keep `release-boxes` and other hosted release asset jobs updateable while
   preserving `--skip-unsupported` behavior for platform-specific imported
   libraries.
5. Add release variants, platform-specific release contents, generated release
   manifests, and dry runs for release preparation, tagging, and publication.
6. Add package distribution planning, starting with GitHub-hosted `.deb` or
   APT-style installation, then a version-aware `razterizer/setup-forge`
   action.

Local commands and CI should execute the same Forge-defined workflow. CI files
should become thin adapters that install Forge and invoke the appropriate
profile. Forge-controlled distribution lets generated workflows receive a
current compatible Forge version even when official Ubuntu repositories retain
older packages for a distribution release. See the remaining roadmap in
[`design.md`](design.md) for the full milestone breakdown.
