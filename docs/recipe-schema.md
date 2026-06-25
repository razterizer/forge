# Forge recipe schema

Forge provides a JSON Schema for `forge.recipe.toml`:

```text
schemas/forge.recipe.schema.json
```

See [dependencies.md](dependencies.md) for the dependency forms, profile
selection, and lockfile model described by the schema.

TOML-aware editors and language servers can use it for validation,
documentation, and completion of keys and values such as:

```toml
[project]
type = "static_library"
```

The schema describes the current recipe surface and enforces project-specific
rules:

- Executables require at least one source path.
- Static libraries require source paths and public headers.
- Dynamic libraries require source paths and public headers.
- Imported libraries require a matching target-specific import profile with
  header roots and at least one precompiled library artifact.
- Header-only projects require public headers and an empty source-path array.
- Public headers must remain under `include/`.
- Source and named-target `include_dirs` declare private project-relative
  include search directories. `forge adopt` infers these when local headers
  unambiguously satisfy source `#include` directives.
- `macos_system_include_dirs`, `linux_system_include_dirs`, and
  `windows_system_include_dirs` declare platform-specific external include
  search directories. Forge emits them as CMake `SYSTEM` include directories
  and does not require them to be project-relative.
- `macos_system_library_dirs`, `linux_system_library_dirs`, and
  `windows_system_library_dirs` declare platform-specific external library
  search directories for SDKs installed outside the project tree.
- `[build].defines` and named-target `defines` declare persistent preprocessor
  definitions. Definitions use `NAME` or `NAME=value` syntax. Repeatable
  `forge build --define=<symbol>` options temporarily add private definitions
  to selected root builds without changing their recipes.
- Build numbers must be non-negative.
- `[release].build_number_format` may be `"dotted"` or `"semver"` to make
  release-note headings use `<version>.<number>` or
  `<version>+build.<number>`. It also qualifies default Git release tags and
  requires `[build].number`.
- `[release].bundle_name` may set the filename-safe base name for
  multi-executable hosted release bundles, such as `8beat` for
  `8beat-release-1.0.0.1-macos.zip`.
- `[release].variants` may set multiple hosted executable release variants,
  such as `[{ profile = "workflow-release", suffix = "openal" },
  { profile = "applaudio-release", suffix = "applaudio" }]`. Each variant
  selects matching dependency/build profiles and appends its suffix to staged
  executable names, for example `demo_1_openal` and `demo_1_applaudio`.
- `[box].variants` may set multiple hosted dependency box variants, using the
  same `{ profile, suffix }` form. Each variant selects matching
  dependency/build profiles and appends its suffix to the generated cbox name,
  for example `8Beat-1.0.0+build.1-openal-macos-arm64.cbox` and
  `8Beat-1.0.0+build.1-applaudio-macos-arm64.cbox`.
- `[release].readme` may map platform-specific README files into executable
  release archives as `README.txt`, for example
  `{ linux = "demos/README_LINUX.md", macos = "demos/README_MACOS.md",
  windows = "demos/README_WINDOWS.md" }`.
- `[profile.workflow-release.build]` and
  `[profile.workflow-release.dependencies]` are the reserved hosted-workflow
  profile. `forge workflow prepare-release` selects it automatically when
  present and rejects local-path dependencies in that profile. Its dependency
  table replaces the complete default dependency set.
- `[version_header]` declares a project-relative generated C/C++ header path
  and uppercase macro prefix. `forge bump` regenerates its version string,
  major, minor, patch, and build macros.
- Projects may declare local static-library, dynamic-library, imported-library,
  and header-only dependencies using name and either project-path or cbox-path
  inline tables.
- Dependency inline tables may include `targets = ["<os>-<arch>", ...]` to
  enable a dependency only for selected platform targets, for example a
  Windows-only imported SDK cbox while Linux and macOS use system packages.
- Pinned Git source dependencies require `git` and an exact full 40- or
  64-hex-character `commit`. Forge caches the detached checkout and treats it
  like a local source project.
- Named `[profile.<name>.dependencies]` sections provide complete dependency-set
  overrides. `[profile.<name>.build]` sections add build configuration,
  `cpp_std`, `include_dirs`, platform system include dirs, and `defines`
  overrides. Both are selected by `forge build`, `forge build-and-run`, or
  `forge test` with `--profile=<name>`.
- Downloadable cbox dependencies require both `url` and lowercase `sha256`.
- GitHub Release cbox dependencies require `github = "owner/repository"` and a
  packaged `version`. `forge update` writes their exact target-specific
  resolutions to `forge.lock.toml`; normal builds require those locked entries.
  Multi-component GitHub releases may declare `package` for the aggregate cbox
  identity and `component` for the named library selected from it. Dependency
  cbox variants may declare `variant = "name"` to resolve filenames with that
  variant suffix.
- Projects may declare project-relative `[runtime].files`. Executable runtime
  files are staged beside the executable and included in boxes and releases.
  Library and header-only runtime files are exported in boxes and staged
  transitively by consumers. String entries preserve their paths; `{ source,
  destination }` entries map a project file or directory to a different
  executable-relative destination such as `Termin8or/fonts`.
- Repositories may replace the legacy project target with one or more
  `[target.<name>]` sections. Each named target declares its own type, C++
  standard, sources, public headers, and runtime files.
- Named targets may declare internal library target dependencies. Forge builds
  and links their transitive closure and rejects missing or cyclic targets.
- Legacy `[build]` sections and named targets may declare `macos_frameworks`,
  `macos_libraries`, `linux_libraries`, and `windows_libraries`. Named library
  target requirements propagate to dependent targets.
- Named executable targets marked with `test = true` are run by `forge test`.
- Named targets may be selected for boxing and release preparation. Internal
  library target dependencies are recursively packaged as embedded boxes.

`shared_library` remains accepted as a legacy alias for `dynamic_library`.

Imported-library profiles use `[import.<os>-<arch>]`, for example:

```toml
[project]
name = "vendor-sdk"
version = "4.2.0"
type = "imported_library"

[import.windows-x86_64]
compiler = "MSVC"
compiler_version = "19.40.33811.0"
cpp_std = 20
configuration = "Release"
runtime = "msvc-dynamic"
public_headers = ["vendor/include"]
dynamic_libraries = ["vendor/bin/sdk.dll"]
import_libraries = ["vendor/lib/sdk.lib"]
```

The compiler fields are mandatory for imported binaries. Forge compares the
compiler family, C++ standard, build configuration, and runtime with the actual
toolchain selected by CMake before linking a consumer. The exact compiler
version remains recorded metadata for inspection, but compatible hosted runners
may move between patch or minor compiler releases without invalidating an
otherwise matching imported-library box.

For projects built by Forge, `[project].cpp_std` tells Forge which C++ standard
to use for the build. Forge then records that value as `[toolchain].cpp_std` in
the compiled box so consumers can check binary compatibility. Imported-library
profiles declare their toolchain value explicitly because Forge did not build
those binaries.

## Taplo and VS Code

`forge new` and `forge adopt` automatically add the Taplo-compatible schema
directive at the start of generated recipes:

```toml
#:schema https://raw.githubusercontent.com/razterizer/forge/main/schemas/forge.recipe.schema.json
```

VS Code extensions backed by Taplo, including Even Better TOML, then provide
Forge-specific validation and completion.

During local schema development, the directive can instead use an absolute path
to `schemas/forge.recipe.schema.json`.

## Xcode

Xcode does not currently consume JSON Schema for TOML. The schema remains the
shared definition for other editors, documentation, CI, and future Forge
validation commands.

Forge also provides `schemas/forge.workspace.schema.json` for solution-like
`forge.workspace.toml` files. Workspace files list project directories while
their individual recipes remain authoritative.
