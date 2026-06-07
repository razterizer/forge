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

#include <array>
#include <filesystem>
#include <ostream>

namespace forge::cli
{
  namespace
  {

    constexpr std::array commands {
      std::string_view { "box" },
      std::string_view { "init" },
      std::string_view { "new" },
      std::string_view { "build" },
      std::string_view { "bump" },
      std::string_view { "clean" },
      std::string_view { "run" },
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
        << "  forge new <name>\n"
        << "  forge box <create|inspect|verify|extract|publish> [path]\n"
        << "  forge update [dependency]\n"
        << "  forge build [target]\n"
        << "  forge bump <major|minor|patch>\n"
        << "  forge release-git [--tag=<format> | --tag-force[=<format>]]\n"
        << "  forge run [target] [-- arguments...]\n"
        << "  forge --help\n"
        << "  forge --version\n\n"
        << "Commands:\n"
        << "  box             Create, inspect, verify, publish locally, or extract boxes\n"
        << "  init            Adopt the current project\n"
        << "  new             Create a new project\n"
        << "  build           Build the current project\n"
        << "  bump            Bump the project version and prepare release notes\n"
        << "  clean           Remove generated project state\n"
        << "  run             Run the current project\n"
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
      if (arguments.size() == 2 && arguments[1] == "create")
      {
        return create_box(working_directory, output, error);
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

      error << "forge: usage: forge box <create|inspect|verify|extract|publish> [path]\n";
      return 2;
    }

    if (arguments.front() == "run")
    {
      Recipe recipe;

      if (!read_recipe(working_directory / "forge.recipe.toml", recipe, error))
      {
        return 2;
      }

      if (recipe.targets.empty())
      {
        return run_project(working_directory, arguments.subspan(1), output, error);
      }

      if (arguments.size() < 2)
      {
        return run_project(working_directory, std::nullopt, {}, output, error);
      }

      auto program_arguments = arguments.subspan(2);

      if (!program_arguments.empty() && program_arguments.front() == "--")
      {
        program_arguments = program_arguments.subspan(1);
      }

      return run_project(
        working_directory,
        std::string { arguments[1] },
        program_arguments,
        output,
        error
      );
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
      if (arguments.size() > 2)
      {
        error << "forge: usage: forge build [target]\n";
        return 2;
      }

      BuildOptions options;

      if (arguments.size() == 2)
      {
        options.target = std::string { arguments[1] };
      }

      return build_project(working_directory, options, output, error);
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

    if (arguments.front() == "release")
    {
      return release_project(working_directory, output, error);
    }

    if (arguments.front() == "prepare-release")
    {
      return prepare_release(working_directory, output, error);
    }

    error << "forge: '" << arguments.front() << "' is not implemented yet\n";
    return 2;
  }

} // namespace forge::cli
