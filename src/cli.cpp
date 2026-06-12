#include "cli.h"
#include "box.h"
#include "bump.h"
#include "build.h"
#include "clean.h"
#include "github.h"
#include "init.h"
#include "new.h"
#include "recipe.h"
#include "release.h"
#include "run.h"
#include "test.h"
#include "workspace.h"

#include <array>
#include <filesystem>
#include <ostream>
#include <vector>

namespace forge::cli
{
  namespace
  {

    constexpr std::array commands {
      std::string_view { "box" },
      std::string_view { "adopt" },
      std::string_view { "init" },
      std::string_view { "new" },
      std::string_view { "build" },
      std::string_view { "bump" },
      std::string_view { "clean" },
      std::string_view { "run" },
      std::string_view { "test" },
      std::string_view { "update" },
      std::string_view { "release" },
      std::string_view { "workflow" },
      std::string_view { "prepare-release" },
      std::string_view { "release-git" },
      std::string_view { "release-github" },
    };

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
        << "  init            Alias for adopt\n"
        << "  new             Create a new Forge C++ project\n"
        << "  build           Build a project, target, or workspace selection\n"
        << "  bump            Bump the project version and prepare release notes\n"
        << "  clean           Remove all generated Forge state for a project\n"
        << "  run             Build and run an executable project or target\n"
        << "  test            Build and run marked test targets\n"
        << "  update          Resolve and lock GitHub cbox dependencies\n"
        << "  release         Build and package a local executable release\n"
        << "  workflow        Run or extend hosted CI workflows\n"
        << "  release-git     Create and push a tag that triggers hosted releases\n\n"
        << "Run 'forge <command> --help' for command-specific usage and examples.\n";
    }

    bool print_command_help(std::string_view command, std::ostream& output)
    {
      if (command == "adopt" || command == "init")
      {
        output
          << "Discover an existing C++ project and create Forge metadata.\n\n"
          << "Usage:\n"
          << "  forge adopt [--github] [--library-type=<type>]\n\n"
          << "Options:\n"
          << "  --github               Verify inferred GitHub source dependencies\n"
          << "  --library-type=<type>  Resolve an ambiguous library as header_only,\n"
          << "                         static_library, or dynamic_library\n\n"
          << "Adoption inspects existing sources, CMake, Visual Studio, and Xcode\n"
          << "metadata, then creates Forge recipes, workspaces, and missing release\n"
          << "support without overwriting existing Forge files. It does not rewrite\n"
          << "or remove the project's existing build infrastructure.\n\n"
          << "Examples:\n"
          << "  forge adopt\n"
          << "  forge adopt --library-type=static_library\n"
          << "  forge adopt --github\n";
        return true;
      }

      if (command == "box")
      {
        output
          << "Create and operate on verified Forge cbox packages.\n\n"
          << "Usage:\n"
          << "  forge box list\n"
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
          << "  forge new <name>\n\n"
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
          << "Build and run an executable project or target.\n\n"
          << "Usage:\n"
          << "  forge run [target|project[/target]] [--profile=<name>] [-- arguments...]\n\n"
          << "Arguments after '--' are forwarded to the executable. Workspace runs\n"
          << "require a project or project/target selection.\n";
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
          << "  forge update [dependency] [--profile=<name>]\n\n"
          << "Options:\n"
          << "  --profile=<name>   Select a dependency profile before resolving\n\n"
          << "Writes exact package identities, selected components, URLs, targets,\n"
          << "and checksums to forge.lock.toml without building the current project.\n"
          << "Existing entries for other targets are preserved.\n";
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
          << "Remove all generated Forge state for the current project.\n\n"
          << "Usage:\n"
          << "  forge clean\n\n"
          << "Removes .forge/ build output, generated files, dependency installs,\n"
          << "boxes, release artifacts, and caches. Source files are preserved.\n";
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
          << "  forge release-git [--tag=<format> | --tag-force[=<format>]]\n\n"
          << "Options:\n"
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
          << "  forge workflow prepare-release [target]\n"
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
          << "  forge workflow prepare-release [target]\n";
        return true;
      }

      return false;
    }

    bool is_command(std::string_view candidate)
    {
      for (const auto command : commands)
      {
        if (candidate == command)
        {
          return true;
        }
      }

      return false;
    }

    bool read_profile_option(std::string_view argument,
                             std::optional<std::string>& profile,
                             std::ostream& error)
    {
      if (!argument.starts_with("--profile="))
      {
        return false;
      }

      if (profile)
      {
        error << "forge: dependency profile may only be specified once\n";
        return false;
      }

      profile = std::string { argument.substr(std::string_view { "--profile=" }.size()) };

      if (profile->empty())
      {
        error << "forge: dependency profile cannot be empty\n";
        return false;
      }

      return true;
    }

  } // namespace

  int run(std::span<const std::string_view> arguments,
          std::ostream& output,
          std::ostream& error)
  {
    std::error_code filesystem_error;
    const auto working_directory = std::filesystem::current_path(filesystem_error);

    if (filesystem_error)
    {
      error << "forge: could not determine the current directory\n";
      return 2;
    }

    return run(arguments, working_directory, output, error);
  }

  int run(std::span<const std::string_view> arguments,
          const std::filesystem::path& working_directory,
          std::ostream& output,
          std::ostream& error)
  {
    if (arguments.empty() || arguments.front() == "--help" || arguments.front() == "-h")
    {
      print_help(output);
      return 0;
    }

    if (arguments.front() == "--version")
    {
      output << "forge " << version << '\n';
      return 0;
    }

    if (!is_command(arguments.front()))
    {
      error << "forge: unknown command '" << arguments.front() << "'\n";
      return 2;
    }

    if ((arguments.size() == 2
         && (arguments[1] == "--help" || arguments[1] == "-h"))
        || ((arguments.front() == "box" || arguments.front() == "workflow")
            && (arguments.back() == "--help" || arguments.back() == "-h")))
    {
      print_command_help(arguments.front(), output);
      return 0;
    }

    if (arguments.front() == "new")
    {
      if (arguments.size() != 2)
      {
        error << "forge: usage: forge new <name>\n";
        return 2;
      }

      return new_project(working_directory, arguments[1], output, error);
    }

    if (arguments.front() == "box")
    {
      if (arguments.size() == 2 && arguments[1] == "list")
      {
        return list_boxes(working_directory, output, error);
      }

      if ((arguments.size() == 2 || arguments.size() == 3) && arguments[1] == "create")
      {
        const auto target = arguments.size() == 3
          ? std::optional<std::string> { arguments[2] }
          : std::nullopt;
        return create_box(working_directory, target, output, error);
      }

      if (arguments.size() == 3 && arguments[1] == "inspect")
      {
        return inspect_box(arguments[2], working_directory, output, error);
      }

      if (arguments.size() == 3 && arguments[1] == "verify")
      {
        return verify_box(arguments[2], working_directory, output, error);
      }

      if (arguments.size() == 3 && arguments[1] == "extract")
      {
        return extract_box(arguments[2], working_directory, output, error);
      }

      if (arguments.size() == 3 && arguments[1] == "publish")
      {
        return publish_box(arguments[2], working_directory, output, error);
      }

      error << "forge: usage: forge box list\n";
      error << "forge: usage: forge box create [target]\n";
      error << "forge: usage: forge box <inspect|verify|extract|publish> <path-or-filename>\n";
      return 2;
    }

    if (arguments.front() == "run")
    {
      const auto workspace =
        !std::filesystem::exists(working_directory / "forge.recipe.toml")
        && std::filesystem::exists(working_directory / "forge.workspace.toml");
      Recipe recipe;

      if (!workspace && !read_recipe(working_directory / "forge.recipe.toml", recipe, error))
      {
        return 2;
      }

      std::optional<std::string> profile;
      std::optional<std::string> target;
      std::vector<std::string_view> program_arguments;
      bool forwarding = false;

      for (const auto argument : arguments.subspan(1))
      {
        if (!forwarding && argument == "--")
        {
          forwarding = true;
        }
        else if (!forwarding && argument.starts_with("--profile="))
        {
          if (!read_profile_option(argument, profile, error))
          {
            return 2;
          }
        }
        else if (!forwarding && (workspace || !recipe.targets.empty()) && !target)
        {
          target = std::string { argument };
        }
        else
        {
          program_arguments.push_back(argument);
        }
      }

      if (workspace)
      {
        if (!target)
        {
          error << "forge: workspace run requires <project> or <project>/<target>\n";
          return 2;
        }

        return run_workspace(
          working_directory,
          *target,
          profile,
          program_arguments,
          output,
          error
        );
      }

      if (!recipe.targets.empty() && !target)
      {
        return run_project(working_directory, std::nullopt, profile, program_arguments, output, error);
      }

      return run_project(
        working_directory,
        target,
        profile,
        program_arguments,
        output,
        error
      );
    }

    if (arguments.front() == "test")
    {
      std::optional<std::string> target;
      std::optional<std::string> profile;
      std::vector<std::string_view> test_arguments;
      bool forwarding = false;

      for (const auto argument : arguments.subspan(1))
      {
        if (!forwarding && argument == "--")
        {
          forwarding = true;
        }
        else if (!forwarding && argument.starts_with("--profile="))
        {
          if (!read_profile_option(argument, profile, error))
          {
            return 2;
          }
        }
        else if (!forwarding && !target)
        {
          target = std::string { argument };
        }
        else
        {
          test_arguments.push_back(argument);
        }
      }

      if (!std::filesystem::exists(working_directory / "forge.recipe.toml")
          && std::filesystem::exists(working_directory / "forge.workspace.toml"))
      {
        return test_workspace(
          working_directory,
          target,
          profile,
          test_arguments,
          output,
          error
        );
      }

      return test_project(working_directory, target, profile, test_arguments, output, error);
    }

    if (arguments.front() == "bump")
    {
      if (arguments.size() != 2
          || (arguments[1] != "major" && arguments[1] != "minor" && arguments[1] != "patch"))
      {
        error << "forge: usage: forge bump <major|minor|patch>\n";
        return 2;
      }

      return bump_project(working_directory, arguments[1], output, error);
    }

    if (arguments.front() == "update")
    {
      BuildOptions options;
      options.dependencies_only = true;
      options.update_dependencies = true;

      for (const auto argument : arguments.subspan(1))
      {
        if (argument.starts_with("--profile="))
        {
          if (!read_profile_option(argument, options.profile, error))
          {
            return 2;
          }
        }
        else if (!options.update_dependency)
        {
          options.update_dependency = std::string { argument };
        }
        else
        {
          error << "forge: usage: forge update [dependency] [--profile=<name>]\n";
          return 2;
        }
      }

      return build_project(working_directory, options, output, error);
    }

    if (arguments.front() == "release-git" || arguments.front() == "release-github")
    {
      GitReleaseOptions options;
      options.tag_format = "release-<version>";

      if (arguments.size() == 2 && arguments[1] == "--tag-force")
      {
        options.force_tag = true;
      }
      else if (arguments.size() == 2 && arguments[1].starts_with("--tag="))
      {
        options.tag_format = std::string {
          arguments[1].substr(std::string_view { "--tag=" }.size())
        };

        if (options.tag_format->empty())
        {
          error << "forge: tag format cannot be empty\n";
          return 2;
        }
      }
      else if (arguments.size() == 2 && arguments[1].starts_with("--tag-force="))
      {
        options.tag_format = std::string {
          arguments[1].substr(std::string_view { "--tag-force=" }.size())
        };
        options.force_tag = true;

        if (options.tag_format->empty())
        {
          error << "forge: tag format cannot be empty\n";
          return 2;
        }
      }
      else if (arguments.size() != 1)
      {
        error
          << "forge: usage: forge release-git "
          << "[--tag=<format> | --tag-force[=<format>]]\n";
        return 2;
      }

      return release_git(working_directory, options, output, error);
    }

    if (arguments.front() == "build")
    {
      BuildOptions options;

      for (const auto argument : arguments.subspan(1))
      {
        if (argument.starts_with("--profile="))
        {
          if (!read_profile_option(argument, options.profile, error))
          {
            return 2;
          }
        }
        else if (argument.starts_with("--define="))
        {
          const auto definition = argument.substr(std::string_view { "--define=" }.size());

          if (!is_valid_compile_definition(definition))
          {
            error << "forge: invalid compile definition '" << definition << "'\n";
            return 2;
          }

          options.compile_definitions.emplace_back(definition);
        }
        else if (!options.target)
        {
          options.target = std::string { argument };
        }
        else
        {
          error << "forge: usage: forge build [target] [--profile=<name>] [--define=<symbol> ...]\n";
          return 2;
        }
      }

      if (!std::filesystem::exists(working_directory / "forge.recipe.toml")
          && std::filesystem::exists(working_directory / "forge.workspace.toml"))
      {
        return build_workspace(
          working_directory,
          options.target,
          options.profile,
          options.compile_definitions,
          output,
          error
        );
      }

      return build_project(working_directory, options, output, error);
    }

    if (arguments.front() == "workflow")
    {
      if (arguments.size() == 2 && arguments[1] == "list-features")
      {
        return list_github_workflow_features(output);
      }

      const auto workflow_feature_operation =
        arguments.size() >= 2 && arguments[1] == "add-feature"
          ? std::optional { GithubWorkflowFeatureOperation::add }
        : arguments.size() >= 2 && arguments[1] == "update-feature"
          ? std::optional { GithubWorkflowFeatureOperation::update }
        : arguments.size() >= 2 && arguments[1] == "remove-feature"
          ? std::optional { GithubWorkflowFeatureOperation::remove }
        : std::nullopt;

      if (workflow_feature_operation && arguments.size() < 3)
      {
        error
          << "forge: usage: forge workflow " << arguments[1] << " release-boxes "
          << "--file=<workflow> [--apply]\n";
        return 2;
      }

      if (workflow_feature_operation && arguments.size() >= 3)
      {
        const auto feature = arguments[2];
        std::optional<std::filesystem::path> workflow_file;
        bool apply = false;

        for (const auto argument : arguments.subspan(3))
        {
          if (argument == "--apply" && !apply)
          {
            apply = true;
          }
          else if (argument.starts_with("--file=") && !workflow_file)
          {
            const auto value = argument.substr(std::string_view { "--file=" }.size());

            if (value.empty())
            {
              error << "forge: workflow file cannot be empty\n";
              return 2;
            }

            workflow_file = std::filesystem::path { value };
          }
          else
          {
            error
              << "forge: usage: forge workflow " << arguments[1] << " release-boxes "
              << "--file=<workflow> [--apply]\n";
            return 2;
          }
        }

        if (!workflow_file)
        {
          error
            << "forge: usage: forge workflow " << arguments[1] << " release-boxes "
            << "--file=<workflow> [--apply]\n";
          return 2;
        }

        return change_github_workflow_feature(
          working_directory,
          *workflow_feature_operation,
          feature,
          *workflow_file,
          apply,
          output,
          error
        );
      }

      if (arguments.size() >= 2 && arguments[1] == "status")
      {
        if (arguments.size() != 3 || !arguments[2].starts_with("--file="))
        {
          error << "forge: usage: forge workflow status --file=<workflow>\n";
          return 2;
        }

        const auto value = arguments[2].substr(std::string_view { "--file=" }.size());

        if (value.empty())
        {
          error << "forge: workflow file cannot be empty\n";
          return 2;
        }

        return status_github_workflow_features(
          working_directory,
          std::filesystem::path { value },
          output,
          error
        );
      }

      if (arguments.size() < 2
          || arguments[1] != "prepare-release"
          || arguments.size() > 3
          || (arguments.size() == 3 && arguments[2].starts_with("-")))
      {
        error << "forge: usage: forge workflow prepare-release [target]\n";
        return 2;
      }

      const auto target = arguments.size() == 3
        ? std::optional<std::string> { arguments[2] }
        : std::nullopt;

      return prepare_release(working_directory, target, output, error);
    }

    if (arguments.front() == "release" || arguments.front() == "prepare-release")
    {
      if (arguments.size() == 2 && arguments[1].starts_with("-"))
      {
        error << "forge: commands do not accept arguments yet\n";
        return 2;
      }

      if (arguments.size() > 2)
      {
        error << "forge: usage: forge " << arguments.front() << " [target]\n";
        return 2;
      }

      const auto target = arguments.size() == 2
        ? std::optional<std::string> { arguments[1] }
        : std::nullopt;

      if (arguments.front() == "release")
      {
        return release_project(working_directory, target, output, error);
      }

      error
        << "forge: warning: 'prepare-release' is deprecated; "
        << "use 'forge workflow prepare-release'\n";
      return prepare_release(working_directory, target, output, error);
    }

    if (arguments.front() == "adopt")
    {
      AdoptOptions options;

      for (const auto argument : arguments.subspan(1))
      {
        if (argument == "--github")
        {
          options.github = true;
        }
        else if (argument.starts_with("--library-type="))
        {
          const auto type = argument.substr(std::string_view { "--library-type=" }.size());

          if (options.library_type
              || (type != "header_only"
                  && type != "static_library"
                  && type != "dynamic_library"))
          {
            error << "forge: library type must be header_only, static_library, or dynamic_library\n";
            return 2;
          }

          options.library_type = std::string { type };
        }
        else
        {
          error << "forge: usage: forge adopt [--github] [--library-type=<type>]\n";
          return 2;
        }
      }

      return adopt_project(working_directory, options, run_process, output, error);
    }

    if (arguments.size() != 1)
    {
      error << "forge: commands do not accept arguments yet\n";
      return 2;
    }

    if (arguments.front() == "init")
    {
      return init_project(working_directory, output, error);
    }

    if (arguments.front() == "clean")
    {
      return clean_project(working_directory, output, error);
    }

    error << "forge: '" << arguments.front() << "' is not implemented yet\n";
    return 2;
  }

} // namespace forge::cli
