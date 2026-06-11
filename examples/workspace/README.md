# Sibling source dependency workspace

This workspace demonstrates an executable linking directly to an editable
sibling source checkout:

```toml
[dependencies]
answer = { path = "../answer" }
```

From this directory:

```sh
../../build/dev/forge build
../../build/dev/forge run answer-app
../../build/dev/forge build answer
```

Building or running `answer-app` automatically builds, boxes, installs, and
links the sibling `answer` project. Editing `answer` and running the app again
rebuilds the dependency when needed.
