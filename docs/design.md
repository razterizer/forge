# Forge design baseline

Forge is a project-centric workflow tool for C++. It coordinates existing
compilers and build systems rather than replacing them.

## Principles

- Forge is a C++20 single executable.
- CMake is the first build backend.
- `forge.recipe.toml` describes human-authored project intent.
- `forge.lock.toml` records exact resolved dependencies and artifacts.
- A **box** is a packaged C++ artifact stored in a `.cbox` file.
- Forge starts locally. Remote registries and shared caches come later.
- `forge adopt` adopts an existing project and never creates, moves, or modifies
  source files. It discovers public headers and `main()` entry points to infer
  executable, multi-executable, static-library, and header-only recipes. For
  multi-executable projects, unambiguous local include relationships guide
  source ownership while uncertain sources remain shared. `forge init` remains
  a compatibility alias. Remaining library-looking includes are reported as
  dependency candidates. Unambiguous public-header matches from sibling
  single-target Forge libraries are added as local path dependencies. A GitHub
  origin enables non-network same-owner suggestions; `forge adopt --github`
  explicitly verifies and pins accepted repositories by exact commit.
- `forge adopt` imports concrete metadata from CMake, Visual Studio projects
  and solutions, Xcode projects, MSBuild property sheets, and
  configuration-matched `.xcconfig` files. Generated CMake IDE projects do not
  outrank their `CMakeLists.txt`; hand-maintained mirrored native projects
  remain authoritative and receive additional concrete CMake settings.
  Concrete target-free CMake superprojects become Forge workspaces containing
  their `add_subdirectory(...)` projects.
- Library projects containing examples or tests retain a named library target,
  and inferred executable targets depend on it. `forge adopt --library-type`
  provides an explicit header-only, static-library, or dynamic-library hint
  when metadata remains ambiguous. Source dependencies automatically select a
  named library target matching the package name.
- When mirrored native and CMake projects disagree about the C++ standard,
  adoption preserves the highest declared requirement.
- `forge new <name>` explicitly creates a new project with a recipe and starter
  source file.
- `forge build` generates private CMake infrastructure under `.forge/generated`
  and writes artifacts under `.forge/build`.
- `forge run` performs an incremental build before launching the executable and
  forwarding its arguments and exit status.
- Runtime assets may preserve their project-relative paths or map a source file
  to a different executable-relative destination. Adoption infers only
  high-confidence literal file accesses with adjacent or unique matches.
- Named targets use `[target.<name>]` sections and isolated generated and build
  directories. The legacy single-target recipe form remains supported.
- Named targets may depend on other named library targets in the same recipe.
  Forge builds and links the internal dependency closure.
- `forge test` builds and runs marked named executable targets and summarizes
  all results after continuing through failures.
- Named target boxes recursively embed boxes for their direct internal library
  dependencies. Named executable releases stage required dynamic libraries.
- `forge release` stages an application release and creates a versioned ZIP
  archive. Release archives are distinct from dependency `.cbox` artifacts.
- `forge.workspace.toml` groups independently defined Forge projects. Workspace
  builds validate the complete local project graph and build root projects,
  while existing recipe dependencies remain the source of truth for edges.
  Workspace runs select a project or named target, and workspace tests
  aggregate marked tests across projects.

## Project workspace

Forge keeps generated and downloaded state under an ignored `.forge`
directory:

```text
.forge/
├── boxes/       # packaged binary artifacts
├── deps/        # installed dependency boxes
├── build/       # build outputs
├── cache/       # reusable local cache
└── generated/   # generated CMake files and presets
```

Dependency resolution should prefer a compatible cached box. When no matching
box exists, Forge fetches or uses source, builds it, creates a box, and caches
that box. Source dependency boxes are reused when their package identity,
target, declared inputs, and embedded direct dependency checksums still match.

Local path dependencies may be declared by executable, static-library,
dynamic-library, and header-only projects. Forge recursively builds library
dependencies, rejects cycles and conflicting project names, links the
transitive static-library closure in dependency order, and stages dynamic
libraries for running and releasing applications.

Pinned Git source dependencies use `{ git = "<repository>", commit =
"<full-commit-id>" }`. Forge fetches only the declared commit into an immutable
project-local cache, verifies cached HEAD before reuse, and then resolves the
checkout through the same recursive source-dependency pipeline. The recipe pin
is already exact, so it is not duplicated in `forge.lock.toml`.

Named `[profile.<name>.dependencies]` sections replace the root recipe's normal
dependency set when selected with `--profile=<name>`. The selected profile
propagates through the recursive source graph. Child dependencies use a
matching profile when present and otherwise retain their normal dependencies.
This supports editable local development graphs and exact pinned CI graphs
without maintaining separate recipes.

Local `.cbox` dependencies are verified and installed directly without their
source project. Format-2 boxes embed their direct dependency boxes and Forge
recursively installs the complete graph. Format-1 boxes remain supported as
self-contained leaf dependencies.

Downloadable `.cbox` dependencies require an explicit SHA-256 checksum. Forge
uses the checksum as an immutable local cache key before applying normal box
verification and installation.

GitHub Release dependencies use `{ github = "owner/repository", version =
"<version>" }`, with optional `package` and `component` identities for aggregate
project boxes. Forge resolves the conventional target-qualified box and
checksum assets from the matching release tag, including configured
build-qualified tags with fallback to legacy `release-<version>` tags, then
records the exact package, component, target, URL, and checksum in
`forge.lock.toml`.
Normal builds require and consume matching locked entries without re-resolving
GitHub. `forge update` and `forge update <dependency>` deliberately refresh
current-target entries without building the current project, while preserving
other targets. Header-only release boxes use one portable `any` lock entry.
Lockfile format 2 records package and component identity; format-1 lockfiles
remain readable and are upgraded on the next update.

## Remaining roadmap

Forge has implemented the original local workflow, dependency, adoption,
workspace, box, runtime-assembly, and hosted-release milestones. The remaining
roadmap focuses on making those features compose cleanly and stabilizing them
for 1.0.

### v0.7: Complete multi-target package publication

- Verify aggregate boxes, natural library cboxes, executable archives, and
  component consumption on Linux, macOS, and Windows.

Release criterion: Core and Termin8or publish directly consumable library
cboxes and downloadable non-test executable archives on every platform, while
aggregate boxes remain available for explicit component-container workflows.

### v0.8: Complete workspace and adoption lifecycle

- Add workspace-aware `forge clean`, release preparation, and publication.
- Make repeated `forge adopt` runs preserve intentional recipe and workflow
  edits while reporting newly discovered metadata.
- Infer the project version from the latest valid `RELEASE_NOTES.md` heading
  when stronger project metadata does not declare one.
- Improve ambiguous source ownership and dependency resolution prompts.
- Finish CMake, Visual Studio solution, and Xcode workspace interoperability
  for mixed and mirrored project layouts.
- Add concise progress and actionable summaries to long build, update, and
  workspace operations.

Release criterion: Core, Termin8or, and Asciiroid_Belt can be adopted, built,
tested, run, cleaned, and prepared for release together without maintaining a
parallel hand-written build graph.

### v0.9: Complete reproducible dependency management

- Support dependencies of `imported_library` packages.
- Extend lockfiles to all remotely resolved artifacts and selected components.
- Define version-constraint syntax and deterministic update behavior.
- Make compiler and ABI compatibility policies explicit and configurable
  without silently accepting unsafe combinations.
- Improve dependency diagnostics for conflicts, unavailable components, and
  incompatible boxes.

Release criterion: a committed recipe and lockfile reproduce the same complete
dependency graph and selected artifacts on every supported host.

### v0.10: Complete declarative workflows

- Add declarative release variants and platform-specific contents.
- Generate release manifests containing artifacts, checksums, components,
  dependencies, and toolchain identities.
- Add dry-run support for release preparation, tagging, and publication.
- Make generated CI files thin, updateable adapters around locally runnable
  Forge commands.

Release criterion: projects no longer need custom build, test, packaging,
tagging, or publication scripts for supported workflows.

### v1.0: Stabilize and harden

- Freeze and document the recipe, workspace, lockfile, and cbox compatibility
  contracts, with explicit migration behavior.
- Make box and release archives deterministic and add ZIP64 support.
- Audit path handling, extraction, checksums, and remote-resolution security.
- Establish supported compiler, runtime, OS, and architecture matrices.
- Expand end-to-end tests and dogfood Forge releases using Forge itself.
- Complete reference documentation and troubleshooting guidance.

Release criterion: Forge can serve as the primary cross-platform workflow and
dependency system for its own repository and the Core, Termin8or, and
Asciiroid_Belt project family.

## Post-1.0 directions

- Registry protocol and shared binary caches.
- Package signing and provenance.
- Vendor SDK redistribution-policy metadata.
- Generic pre-build and post-build hooks only where declarative Forge features
  cannot represent the use case safely.
