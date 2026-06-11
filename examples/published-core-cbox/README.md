# Published Core cbox dependency

This usage example becomes runnable after Core creates its first
Forge-generated GitHub Release containing cboxes.

Add the published package to a consumer recipe, replacing `<published-version>`
with the packaged Core version:

```toml
[dependencies]
Core = { github = "razterizer/Core", version = "<published-version>" }
```

If Core publishes a multi-component project box, also select the library target
declared by Core's Forge recipe:

```toml
[dependencies]
Core = {
  github = "razterizer/Core",
  version = "<published-version>",
  component = "<library-target>"
}
```

Resolve the platform-specific cbox and checksum, then build normally:

```sh
forge update Core
forge build
git add forge.lock.toml
```

Commit `forge.lock.toml` so subsequent builds consume the exact resolved Core
asset without re-resolving GitHub.
