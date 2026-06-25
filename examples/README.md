# Forge examples

Each example has its own README with commands that can be run from that
example's directory. The commands assume Forge has been built at `build/dev`.

| Example | Demonstrates |
| --- | --- |
| [`empty-project/`](empty-project/) | Adopting a directory with no sources |
| [`executable/`](executable/) | Building, running, and releasing an executable |
| [`header-only/`](header-only/) | Creating and inspecting a header-only cbox |
| [`static-library/`](static-library/) | Building and boxing a static library |
| [`imported-library/`](imported-library/) | Packaging precompiled vendor artifacts |
| [`workspace/`](workspace/) | Linking to a sibling source checkout with `path` |
| [`sibling-cbox/`](sibling-cbox/) | Linking to a cbox built from a sibling checkout |
| [`published-core-cbox/`](published-core-cbox/) | Linking to a published Core cbox after its first release |
| [`real-world-ecosystem/`](real-world-ecosystem/) | Reproducing the Core, sound library, Termin8or, 8Beat, and Pilot_Episode dependency graph |

For a quick first run:

```sh
cd examples/executable
../../build/dev/forge build-and-run
```
