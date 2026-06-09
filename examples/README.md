# Forge examples

Each subdirectory demonstrates a project root that Forge can adopt, build, or
run:

- `empty-project/` contains no recipe or source files. Run `forge adopt` there
  to create a recipe with an empty source list.
- `header-only/` packages and validates a header-only library.
- `static-library/` builds and packages a static library.
- `imported-library/` packages local precompiled artifacts for `macos-arm64`
  without building them.
- `executable/` builds and runs an executable with multiple source files.
- `workspace/` groups a static library and dependent executable into a
  solution-like multi-project build.

From an example containing a recipe:

```sh
cd examples/executable
../../build/dev/forge run
```

Create boxes for library examples with:

```sh
cd examples/static-library
../../build/dev/forge box create
```

Build the workspace example from its root:

```sh
cd examples/workspace
../../build/dev/forge build
```
