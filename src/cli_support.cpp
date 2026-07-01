#include "cli_support.h"

#include <array>
#include <ostream>

namespace forge::cli
{
  namespace
  {
    constexpr std::array commands {
      std::string_view { "box" },
      std::string_view { "adopt" },
      std::string_view { "new" },
      std::string_view { "build" },
      std::string_view { "build-and-run" },
      std::string_view { "bump" },
      std::string_view { "clean" },
      std::string_view { "list" },
      std::string_view { "run" },
      std::string_view { "test" },
      std::string_view { "update" },
      std::string_view { "upgrade" },
      std::string_view { "release" },
      std::string_view { "workflow" },
      std::string_view { "prepare-release" },
      std::string_view { "release-git" },
      std::string_view { "release-github" },
    };
  }

  void print_help(std::ostream& output)
  {
    output
      << "Forge - a project workflow system for C++\n\n"
      << "Usage:\n"
      << "  forge <command> [options]\n"
      << "  forge <command> --help\n"
      << "  forge --help\n"
      << "  forge --version\n\n"
      << "Commands:\n"
      << "  adopt           Discover an existing project and create Forge metadata\n"
      << "  box             Create, inspect, verify, publish, and extract cboxes\n"
      << "  new             Create a new Forge C++ project\n"
      << "  build           Build a project, target, or workspace selection\n"
      << "  build-and-run   Build and run an executable project or target\n"
      << "  bump            Bump the project version and prepare release notes\n"
      << "  clean           Remove generated Forge state for a project or workspace\n"
      << "  list            List recipe profiles, targets, dependencies, and boxes\n"
      << "  run             Run an already-built executable project or target\n"
      << "  test            Build and run marked test targets\n"
      << "  update          Refresh lockfile entries for declared dependency versions\n"
      << "  upgrade         Change a GitHub dependency version, then refresh locks\n"
      << "  release         Build and package a local executable release\n"
      << "  workflow        Run or extend hosted CI workflows\n"
      << "  release-git     Create and push a tag that triggers hosted releases\n\n"
      << "Run 'forge <command> --help' for command-specific usage and examples.\n";
  }

  bool print_command_help(std::string_view command, std::ostream& output)
  {
    if (command == "adopt")
    {
      output
        << "Discover an existing C++ project and create Forge metadata.\n\n"
        << "Usage:\n"
        << "  forge adopt [--dependency-style=<style>] [--library-type=<type>]\n"
        << "              [--init-version=<ver>] [--version-header-path=<path>]\n\n"
        << "Options:\n"
        << "  --dependency-style=<style>\n"
        << "                         Dependency style: local or git\n"
        << "                         local keeps verified sibling dependencies as paths;\n"
        << "                         git verifies inferred GitHub source dependencies\n"
        << "  --github               Alias for --dependency-style=git\n"
        << "  --library-type=<type>  Resolve an ambiguous library as header_only,\n"
        << "                         static_library, or dynamic_library\n"
        << "                         Use imported_library in the recipe for\n"
        << "                         prebuilt binaries with import profiles\n\n"
        << "  --init-version=<ver>   Override the initial version; a fourth dotted\n"
        << "                         component initializes build.number\n"
        << "  --version-header-path=<path>\n"
        << "                         Use or create a Forge-managed version header\n\n"
        << "Adoption inspects existing sources, CMake, Visual Studio, and Xcode\n"
        << "metadata, then creates Forge recipes, workspaces, and missing release\n"
        << "support without overwriting existing Forge files. It does not rewrite\n"
        << "or remove the project's existing build infrastructure.\n\n"
        << "Examples:\n"
        << "  forge adopt\n"
        << "  forge adopt --library-type=static_library\n"
        << "  forge adopt --dependency-style=git\n";
      return true;
    }

    if (command == "box")
    {
      output
        << "Create and operate on verified Forge cbox packages.\n\n"
        << "Usage:\n"
        << "  forge box list [--platforms]\n"
        << "  forge box create [target]\n"
        << "  forge box <inspect|verify|extract|publish> <path-or-filename>\n\n"
        << "Commands:\n"
        << "  list     List local and published boxes with package/component summaries\n"
        << "  create   Build and package the project or selected target\n"
        << "  inspect  Summarize a verified cbox, then print its manifest\n"
        << "  verify   Validate a cbox without installing it\n"
        << "  extract  Validate and extract a cbox\n"
        << "  publish  Copy a verified cbox and checksum into boxes/\n\n"
        << "Bare filenames are resolved from .forge/boxes/ and then boxes/.\n";
      return true;
    }

    if (command == "new")
    {
      output
        << "Create a new Forge C++ executable project.\n\n"
        << "Usage:\n"
        << "  forge new <name> [--init-version=<ver>] [--version-header-path=<path>]\n\n"
        << "Creates a new directory containing a starter source, recipe, release\n"
        << "notes, GitHub release workflows, and generated-file ignore rules.\n";
      return true;
    }

    if (command == "build")
    {
      output
        << "Build the current project, selected target, or workspace selection.\n\n"
        << "Usage:\n"
        << "  forge build [target] [--profile=<name>] [--define=<symbol> ...]\n\n"
        << "Options:\n"
        << "  --profile=<name>   Select matching dependency and build profiles\n"
        << "  --define=<symbol>  Add a temporary NAME or NAME=value definition\n";
      return true;
    }

    if (command == "run")
    {
      output
        << "Run an already-built executable project or target.\n\n"
        << "Usage:\n"
        << "  forge run [target|project[/target]] [-- arguments...]\n\n"
        << "Arguments after '--' are forwarded to the executable. Workspace runs\n"
        << "require a project or project/target selection. Use 'forge build' first,\n"
        << "or use 'forge build-and-run' to build before launching.\n";
      return true;
    }

    if (command == "build-and-run")
    {
      output
        << "Build and run an executable project or target.\n\n"
        << "Usage:\n"
        << "  forge build-and-run [target|project[/target]] [--profile=<name>] [-- arguments...]\n\n"
        << "Arguments after '--' are forwarded to the executable. Workspace runs\n"
        << "require a project or project/target selection.\n";
      return true;
    }

    if (command == "list")
    {
      output
        << "List Forge recipe and package information.\n\n"
        << "Usage:\n"
        << "  forge list <profiles|targets|platforms|deps|dependencies|boxes> [--platforms]\n\n"
        << "Categories:\n"
        << "  profiles      List dependency/build profiles and variant roles\n"
        << "  targets       List buildable project targets\n"
        << "  platforms     List supported platforms\n"
        << "  deps          List direct dependencies by recipe section\n"
        << "  dependencies  Alias for deps\n"
        << "  boxes         Alias for box list; accepts --platforms\n";
      return true;
    }

    if (command == "test")
    {
      output
        << "Build and run marked test targets.\n\n"
        << "Usage:\n"
        << "  forge test [target|project[/target]] [--profile=<name>] [-- arguments...]\n\n"
        << "Without a selection, runs every executable target marked test = true.\n"
        << "Arguments after '--' are forwarded to each selected test executable.\n";
      return true;
    }

    if (command == "update")
    {
      output
        << "Resolve and lock GitHub cbox dependencies for the current target.\n\n"
        << "Usage:\n"
        << "  forge update [dependency] [--profile=<name> | --all-profiles] "
        << "[--target=<os-arch> | --all-targets | --release-targets]\n\n"
        << "Options:\n"
        << "  --profile=<name>    Select a dependency profile before resolving\n"
        << "  --all-profiles      Resolve default dependencies and dependency profiles\n"
        << "  --target=<os-arch>  Resolve dependencies for another platform\n\n"
        << "  --all-targets       Resolve targets already represented in forge.lock.toml\n\n"
        << "  --release-targets   Resolve the standard release matrix: linux-x86_64,\n"
        << "                      macos-arm64, and windows-x86_64\n\n"
        << "Writes exact package identities, selected components, URLs, targets,\n"
        << "and checksums to forge.lock.toml without building the current project.\n"
        << "Existing entries for other targets are preserved.\n";
      return true;
    }

    if (command == "upgrade")
    {
      output
        << "Change a GitHub cbox dependency version and update its locks.\n\n"
        << "Usage:\n"
        << "  forge upgrade [dependency] (--to=<version> | --latest) "
        << "[--profile=<name> | --all-profiles] "
        << "[--target=<os-arch> | --all-targets | --release-targets]\n\n"
        << "Options:\n"
        << "  --to=<version>      Set the dependency recipe version before resolving\n"
        << "  --latest            Use latest GitHub release tags; without a dependency,\n"
        << "                      upgrade all direct GitHub dependencies\n"
        << "  --profile=<name>    Select a dependency profile before upgrading\n"
        << "  --all-profiles      Upgrade default dependencies and dependency profiles\n"
        << "  --target=<os-arch>  Resolve dependencies for another platform\n\n"
        << "  --all-targets       Resolve targets already represented in forge.lock.toml\n\n"
        << "  --release-targets   Resolve the standard release matrix: linux-x86_64,\n"
        << "                      macos-arm64, and windows-x86_64\n\n"
        << "Updates forge.recipe.toml, then writes exact package identities, URLs,\n"
        << "targets, and checksums to forge.lock.toml.\n";
      return true;
    }

    if (command == "bump")
    {
      output
        << "Bump the semantic project version and prepare release notes.\n\n"
        << "Usage:\n"
        << "  forge bump <major|minor|patch>\n\n"
        << "Updates forge.recipe.toml, increments the build number, and adds a new\n"
        << "topmost section to RELEASE_NOTES.md. A configured version header is\n"
        << "regenerated from the new version and build number.\n";
      return true;
    }

    if (command == "clean")
    {
      output
        << "Remove generated Forge state for the current project or workspace.\n\n"
        << "Usage:\n"
        << "  forge clean\n\n"
        << "Removes .forge/ build output, generated files, dependency installs,\n"
        << "boxes, release artifacts, and caches. In a workspace root, removes\n"
        << "that state from every workspace project. Source files are preserved.\n";
      return true;
    }

    if (command == "release")
    {
      output
        << "Build and package a local executable release.\n\n"
        << "Usage:\n"
        << "  forge release [target]\n\n"
        << "Creates a local ZIP under .forge/release/. This command does not create\n"
        << "a Git tag or publish a GitHub Release.\n";
      return true;
    }

    if (command == "release-git" || command == "release-github")
    {
      output
        << "Create and push a Git tag that triggers generated hosted workflows.\n\n"
        << "Usage:\n"
        << "  forge release-git [--dry-run] [--tag=<format> | --tag-force[=<format>]]\n\n"
        << "Options:\n"
        << "  --dry-run             Check release readiness without creating a tag\n"
        << "  --tag=<format>        Use a custom release tag format\n"
        << "  --tag-force[=<format>] Replace an existing local and remote tag\n\n"
        << "The generated workflows run 'forge workflow prepare-release' on each\n"
        << "platform and publish their assets. A normal hosted release requires\n"
        << "only this command.\n";
      return true;
    }

    if (command == "workflow")
    {
      output
        << "Run CI workflow steps locally and manage Forge-owned workflow features.\n\n"
        << "Usage:\n"
        << "  forge workflow prepare-release [target] [--skip-unsupported]\n"
        << "  forge workflow list-features\n"
        << "  forge workflow status --file=<workflow>\n"
        << "  forge workflow add-feature release-boxes --file=<workflow> [--apply]\n"
        << "  forge workflow update-feature release-boxes --file=<workflow> [--apply]\n"
        << "  forge workflow remove-feature release-boxes --file=<workflow> [--apply]\n\n"
        << "Commands:\n"
        << "  prepare-release  Build hosted release assets locally\n"
        << "  list-features    List available managed workflow features\n"
        << "  status           Inspect managed feature state in one workflow\n"
        << "  add-feature      Preview or inject a managed feature\n"
        << "  update-feature   Preview or replace an outdated managed feature\n"
        << "  remove-feature   Preview or remove a managed feature\n\n"
        << "prepare-release builds the hosted assets and focused release notes that\n"
        << "generated platform workflows upload. Run it locally only to inspect or\n"
        << "debug those assets before using 'forge release-git'.\n\n"
        << "Feature changes preview by default. Pass --apply to write the selected\n"
        << "workflow. Forge updates or removes only jobs carrying matching\n"
        << "forge-managed metadata.\n";
      return true;
    }

    if (command == "prepare-release")
    {
      output
        << "'forge prepare-release' is a deprecated compatibility alias.\n\n"
        << "Use:\n"
        << "  forge workflow prepare-release [target] [--skip-unsupported]\n";
      return true;
    }

    return false;
  }

  bool is_command(std::string_view candidate)
  {
    for (const auto command : commands)
    {
      if (candidate == command)
        return true;
    }

    return false;
  }

  std::optional<std::string_view> option_value(std::string_view argument,
                                               std::string_view prefix)
  {
    if (!argument.starts_with(prefix))
      return std::nullopt;

    return argument.substr(prefix.size());
  }

  bool set_once(std::optional<std::string>& target,
                std::string_view value)
  {
    if (target)
      return false;

    target = std::string { value };
    return true;
  }

  bool set_once(std::optional<std::filesystem::path>& target,
                std::string_view value)
  {
    if (target)
      return false;

    target = std::filesystem::path { value };
    return true;
  }

  bool read_required_option(std::string_view value,
                            std::string_view empty_message,
                            std::ostream& error)
  {
    if (!value.empty())
      return true;

    error << empty_message << '\n';
    return false;
  }

  bool read_profile_option(std::string_view argument,
                           std::optional<std::string>& profile,
                           std::ostream& error)
  {
    const auto value = option_value(argument, "--profile=");

    if (!value)
      return false;

    if (profile)
    {
      error << "forge: dependency profile may only be specified once\n";
      return false;
    }

    profile = std::string { *value };

    if (profile->empty())
    {
      error << "forge: dependency profile cannot be empty\n";
      return false;
    }

    return true;
  }

  void print_new_usage(std::ostream& error)
  {
    error
      << "forge: usage: forge new <name> [--init-version=<ver>] "
      << "[--version-header-path=<path>]\n";
  }

  void print_adopt_usage(std::ostream& error)
  {
    error
      << "forge: usage: forge adopt [--dependency-style=<style>] "
      << "[--library-type=<type>] [--init-version=<ver>] "
      << "[--version-header-path=<path>]\n";
  }

  void print_update_usage(std::ostream& error)
  {
    error
      << "forge: usage: forge update [dependency] [--profile=<name> | --all-profiles] "
      << "[--target=<os-arch> | --all-targets | --release-targets]\n";
  }

  void print_upgrade_usage(std::ostream& error)
  {
    error
      << "forge: usage: forge upgrade [dependency] (--to=<version> | --latest) "
      << "[--profile=<name> | --all-profiles] "
      << "[--target=<os-arch> | --all-targets | --release-targets]\n";
  }

  void print_build_usage(std::ostream& error)
  {
    error << "forge: usage: forge build [target] [--profile=<name>] [--define=<symbol> ...]\n";
  }

  void print_workflow_feature_usage(std::string_view operation, std::ostream& error)
  {
    error
      << "forge: usage: forge workflow " << operation << " release-boxes "
      << "--file=<workflow> [--apply]\n";
  }

  void print_prepare_release_usage(std::ostream& error)
  {
    error << "forge: usage: forge workflow prepare-release [target] [--skip-unsupported]\n";
  }

  void print_release_git_usage(std::ostream& error)
  {
    error
      << "forge: usage: forge release-git "
      << "[--dry-run] [--tag=<format> | --tag-force[=<format>]]\n";
  }
}
