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

## Planned

The next workflow slices should be:

1. Named build and test profiles.
2. Release variants and platform-specific release contents.
3. Generated release manifests and checksums.
4. Version and release-note validation.
5. Dry-run-first Git tagging and hosted-release publication.

Local commands and CI should execute the same Forge-defined workflow. CI files
should become thin adapters that install Forge and invoke the appropriate
profile.
