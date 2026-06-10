#include "cli.h"
#include "box.h"
#include "bump.h"
#include "build.h"
#include "clean.h"
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
      std::string_view { "prepare-release" },
      std::string_view { "release-git" },
      std::string_view { "release-github" },
    };

    void print_help(std::ostream& output)
    {
      output
        << "Forge - a project workflow system for C++\n\n"
        << "Usage:\n"
        << "  forge <command>\n"
        << "  forge adopt [--github] [--library-type=<type>]\n"
        << "  forge new <name>\n"
        << "  forge box list\n"
        << "  forge box create [target]\n"
        << "  forge box <inspect|verify|extract|publish> <path-or-filename>\n"
        << "  forge update [dependency]\n"
        << "  forge build [target] [--profile=<name>] [--define=<symbol> ...]\n"
        << "  forge bump <major|minor|patch>\n"
        << "  forge release-git [--tag=<format> | --tag-force[=<format>]]\n"
        << "  forge release [target]\n"
        << "  forge prepare-release [target]\n"
        << "  forge run [target|project[/target]] [--profile=<name>] [-- arguments...]\n"
        << "  forge test [target|project[/target]] [--profile=<name>] [-- arguments...]\n"
        << "  forge --help\n"
        << "  forge --version\n\n"
        << "Commands:\n"
        << "  adopt           Adopt the current project\n"
        << "  box             List, create, inspect, verify, publish locally, or extract boxes\n"
        << "  init            Alias for adopt\n"
        << "  new             Create a new project\n"
        << "  build           Build the current project or workspace\n"
        << "  bump            Bump the project version and prepare release notes\n"
        << "  clean           Remove generated project state\n"
        << "  run             Run a project or workspace project\n"
        << "  test            Build and run project or workspace tests\n"
        << "  update          Refresh locked GitHub dependencies\n"
        << "  release         Create a local release artifact\n"
        << "  prepare-release Prepare hosted release assets\n"
        << "  release-git     Create and push a release tag\n";
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
      if (arguments.size() > 2)
      {
        error << "forge: usage: forge update [dependency]\n";
        return 2;
      }

      BuildOptions options;
      options.dependencies_only = true;
      options.update_dependencies = true;

      if (arguments.size() == 2)
      {
        options.update_dependency = std::string { arguments[1] };
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
