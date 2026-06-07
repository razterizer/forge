# Forge recipe schema

Forge provides a JSON Schema for `forge.recipe.toml`:

```text
schemas/forge.recipe.schema.json
```

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
- Build numbers must be non-negative.
- Projects may declare local static-library, dynamic-library, imported-library,
  and header-only dependencies using name and either project-path or cbox-path
  inline tables.
- Downloadable cbox dependencies require both `url` and lowercase `sha256`.
- GitHub Release cbox dependencies require `github = "owner/repository"` and a
  packaged `version`. `forge update` writes their exact target-specific
  resolutions to `forge.lock.toml`; normal builds require those locked entries.
- Executable projects may declare project-relative `[runtime].files`, which
  Forge stages beside the executable and includes in boxes and releases.
- Repositories may replace the legacy project target with one or more
  `[target.<name>]` sections. Each named target declares its own type, C++
  standard, sources, public headers, and runtime files.
- Named targets may declare internal library target dependencies. Forge builds
  and links their transitive closure and rejects missing or cyclic targets.
- Named executable targets marked with `test = true` are run by `forge test`.

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

The compiler fields are mandatory for imported binaries. Forge compares them
with the actual toolchain selected by CMake before linking a consumer.

For projects built by Forge, `[project].cpp_std` tells Forge which C++ standard
to use for the build. Forge then records that value as `[toolchain].cpp_std` in
the compiled box so consumers can check binary compatibility. Imported-library
profiles declare their toolchain value explicitly because Forge did not build
those binaries.

## Taplo and VS Code

`forge new` and `forge init` automatically add the Taplo-compatible schema
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
