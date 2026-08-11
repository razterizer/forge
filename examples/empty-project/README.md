# Empty project example

This directory intentionally contains no recipe or source files. It
demonstrates adopting an empty project.

From this directory:

```sh
../../build/dev/forge adopt
cat forge.recipe.toml
../../build/dev/forge build
```

`forge adopt` creates a reviewable recipe with an empty source list. The final
build is expected to fail until sources and a project type are added.
