# Sibling cbox dependency

This example demonstrates consuming a built package instead of linking directly
to the sibling project's sources. The app recipe points to:

```toml
[dependencies]
answer = { box = "../packages/answer.cbox" }
```

From this directory, build the sibling library box and copy it to the stable
cross-platform filename used by the app recipe:

```sh
(cd answer && ../../../build/dev/forge box create)
cp answer/.forge/boxes/*.cbox packages/answer.cbox
(cd app && ../../../build/dev/forge build-and-run)
```

The app verifies and installs `packages/answer.cbox`; it does not build the
`answer` source checkout. Rebuild and copy the box again after changing the
library.
