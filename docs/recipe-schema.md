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
- Header-only projects require public headers and an empty source-path array.
- Public headers must remain under `include/`.
- Build numbers must be non-negative.
- Projects may declare local static-library and header-only dependencies using
  name and path inline tables.

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
