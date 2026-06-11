# Imported library example

This example packages existing vendor headers and precompiled artifacts without
compiling them. Its checked-in import profile targets `macos-arm64`.

From this directory on `macos-arm64`:

```sh
../../build/dev/forge build
../../build/dev/forge box create
../../build/dev/forge box list
```

On another target, add a matching `[import.<os>-<arch>]` profile and compatible
vendor artifacts before running the commands.
