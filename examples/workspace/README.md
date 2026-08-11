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
../../build/dev/forge build-and-run answer-app
../../build/dev/forge build answer
```

Building `answer-app`, or launching it with `build-and-run`, automatically
builds, boxes, installs, and links the sibling `answer` project. Editing
`answer` and using `build-and-run` again rebuilds the dependency when needed.
