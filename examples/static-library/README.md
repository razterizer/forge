# Static library example

This example builds a static library and packages its library and public header
into a cbox.

From this directory:

```sh
../../build/dev/forge build
../../build/dev/forge box create
../../build/dev/forge box list
```

Use the filename printed by `box list` to inspect or verify the platform box:

```sh
../../build/dev/forge box inspect <box-filename>
../../build/dev/forge box verify <box-filename>
```
