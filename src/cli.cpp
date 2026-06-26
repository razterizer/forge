#include "cli.h"
#include "box.h"
#include "bump.h"
#include "build.h"
#include "clean.h"
#include "cli_support.h"
#include "github.h"
#include "init.h"
#include "new.h"
#include "recipe.h"
#include "release.h"
#include "run.h"
#include "test.h"
#include "workspace.h"

#include <algorithm>
#include <filesystem>
#include <optional>
#include <ostream>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace forge::cli
{
  namespace
  {

    int list_profiles(const std::filesystem::path& project_directory,
                      std::ostream& output,
                      std::ostream& error)
    {
      Recipe recipe;

      if (!read_recipe(project_directory / "forge.recipe.toml", recipe, error))
      {
        return 2;
      }

      std::set<std::string> profiles;

      for (const auto& [name, _] : recipe.dependency_profiles)
      {
        profiles.insert(name);
      }

      for (const auto& [name, _] : recipe.build_profiles)
      {
        profiles.insert(name);
      }

      for (const auto& variant : recipe.release_variants)
      {
        profiles.insert(variant.profile);
      }

      for (const auto& variant : recipe.box_variants)
      {
        profiles.insert(variant.profile);
      }

      if (profiles.empty())
      {
        output << "No profiles declared\n";
        return 0;
      }

      std::size_t profile_width = std::string_view { "Profile" }.size();

      for (const auto& profile : profiles)
      {
        profile_width = std::max(profile_width, profile.size());
      }

      output << "Profiles:\n";
      output << "  Profile" << std::string(profile_width - std::string_view { "Profile" }.size(), ' ')
             << "  Roles\n";
      output << "  " << std::string(profile_width, '-') << "  -----\n";

      for (const auto& profile : profiles)
      {
        std::vector<std::string> roles;

        const auto add_role =
          [&roles](std::string role)
          {
            roles.push_back(std::move(role));
          };

        if (recipe.dependency_profiles.contains(profile))
        {
          add_role("dependencies");
        }

        if (recipe.build_profiles.contains(profile))
        {
          add_role("build");
        }

        for (const auto& variant : recipe.release_variants)
        {
          if (variant.profile == profile)
          {
            add_role("release variant '" + variant.suffix + "'");
          }
        }

        for (const auto& variant : recipe.box_variants)
        {
          if (variant.profile == profile)
          {
            add_role("box variant '" + variant.suffix + "'");
          }
        }

        output << "  " << profile << std::string(profile_width - profile.size(), ' ') << "  ";

        for (std::size_t index = 0; index < roles.size(); ++index)
        {
          if (index > 0)
          {
            output << ", ";
          }

          output << roles[index];
        }

        output << '\n';
      }

      return 0;
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
        || ((arguments.front() == "box"
             || arguments.front() == "profile"
             || arguments.front() == "workflow")
            && (arguments.back() == "--help" || arguments.back() == "-h")))
    {
      print_command_help(arguments.front(), output);
      return 0;
    }

    if (arguments.front() == "new")
    {
      NewOptions options;
      std::optional<std::string_view> name;

      for (const auto argument : arguments.subspan(1))
      {
        if (const auto value = option_value(argument, "--init-version=");
            value && set_once(options.initial_version, *value))
        {
        }
        else if (const auto value = option_value(argument, "--version-header-path=");
                 value && set_once(options.version_header_path, *value))
        {
        }
        else if (!argument.starts_with("-") && !name)
        {
          name = argument;
        }
        else
        {
          print_new_usage(error);
          return 2;
        }
      }

      if (!name)
      {
        print_new_usage(error);
        return 2;
      }

      return new_project(working_directory, *name, options, output, error);
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

    if (arguments.front() == "run" || arguments.front() == "build-and-run")
    {
      const auto build_first = arguments.front() == "build-and-run";
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
          if (!build_first)
          {
            error << "forge: --profile applies to build commands; use 'forge build-and-run'\n";
            return 2;
          }

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
          error << "forge: workspace " << arguments.front()
                << " requires <project> or <project>/<target>\n";
          return 2;
        }

        if (build_first)
        {
          return build_and_run_workspace(
            working_directory,
            *target,
            profile,
            program_arguments,
            output,
            error
          );
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
        if (build_first)
        {
          return build_and_run_project(
            working_directory,
            std::nullopt,
            profile,
            program_arguments,
            output,
            error
          );
        }

        return run_project(working_directory, std::nullopt, profile, program_arguments, output, error);
      }

      if (build_first)
      {
        return build_and_run_project(
          working_directory,
          target,
          profile,
          program_arguments,
          output,
          error
        );
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

    if (arguments.front() == "profile")
    {
      if (arguments.size() == 2 && arguments[1] == "list")
      {
        return list_profiles(working_directory, output, error);
      }

      error << "forge: usage: forge profile list\n";
      return 2;
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
        else if (argument.starts_with("--target="))
        {
          const auto value = *option_value(argument, "--target=");

          if (!read_required_option(value, "forge: --target requires a value", error))
          {
            return 2;
          }

          options.update_target = std::string { value };
        }
        else if (!options.update_dependency)
        {
          options.update_dependency = std::string { argument };
        }
        else
        {
          print_update_usage(error);
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
      else if (const auto value = arguments.size() == 2
                                    ? option_value(arguments[1], "--tag=")
                                    : std::nullopt)
      {
        options.tag_format = std::string { *value };

        if (options.tag_format->empty())
        {
          error << "forge: tag format cannot be empty\n";
          return 2;
        }
      }
      else if (const auto value = arguments.size() == 2
                                    ? option_value(arguments[1], "--tag-force=")
                                    : std::nullopt)
      {
        options.tag_format = std::string { *value };
        options.force_tag = true;

        if (options.tag_format->empty())
        {
          error << "forge: tag format cannot be empty\n";
          return 2;
        }
      }
      else if (arguments.size() != 1)
      {
        print_release_git_usage(error);
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
          const auto definition = *option_value(argument, "--define=");

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
          print_build_usage(error);
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
        print_workflow_feature_usage(arguments[1], error);
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
          else if (const auto value = option_value(argument, "--file=");
                   value && set_once(workflow_file, *value))
          {
            if (!read_required_option(*value, "forge: workflow file cannot be empty", error))
            {
              return 2;
            }
          }
          else
          {
            print_workflow_feature_usage(arguments[1], error);
            return 2;
          }
        }

        if (!workflow_file)
        {
          print_workflow_feature_usage(arguments[1], error);
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

        const auto value = *option_value(arguments[2], "--file=");

        if (!read_required_option(value, "forge: workflow file cannot be empty", error))
        {
          return 2;
        }

        return status_github_workflow_features(
          working_directory,
          std::filesystem::path { value },
          output,
          error
        );
      }

      if (arguments.size() < 2 || arguments[1] != "prepare-release")
      {
        print_prepare_release_usage(error);
        return 2;
      }

      PrepareReleaseOptions options;

      for (const auto argument : arguments.subspan(2))
      {
        if (argument == "--skip-unsupported")
        {
          if (options.skip_unsupported)
          {
            error << "forge: skip-unsupported may only be specified once\n";
            return 2;
          }

          options.skip_unsupported = true;
        }
        else if (!argument.starts_with("-") && !options.target)
        {
          options.target = std::string { argument };
        }
        else
        {
          print_prepare_release_usage(error);
          return 2;
        }
      }

      return prepare_release(working_directory, options, run_process, output, error);
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
      bool dependency_style_set = false;

      for (const auto argument : arguments.subspan(1))
      {
        if (argument == "--github")
        {
          if (dependency_style_set)
          {
            error << "forge: dependency style specified more than once\n";
            return 2;
          }

          options.dependency_style = DependencyStyle::git;
          dependency_style_set = true;
        }
        else if (const auto style = option_value(argument, "--dependency-style="))
        {
          if (dependency_style_set)
          {
            error << "forge: dependency style specified more than once\n";
            return 2;
          }

          if (*style == "local")
          {
            options.dependency_style = DependencyStyle::local;
          }
          else if (*style == "git")
          {
            options.dependency_style = DependencyStyle::git;
          }
          else
          {
            error << "forge: dependency style must be local or git\n";
            return 2;
          }

          dependency_style_set = true;
        }
        else if (const auto type = option_value(argument, "--library-type="))
        {
          if (options.library_type
              || (*type != "header_only"
                  && *type != "static_library"
                  && *type != "dynamic_library"))
          {
            error
              << "forge: library type must be header_only, static_library, or dynamic_library; "
              << "configure imported_library manually with import profiles\n";
            return 2;
          }

          options.library_type = std::string { *type };
        }
        else if (const auto value = option_value(argument, "--init-version=");
                 value && set_once(options.initial_version, *value))
        {
        }
        else if (const auto value = option_value(argument, "--version-header-path=");
                 value && set_once(options.version_header_path, *value))
        {
        }
        else
        {
          print_adopt_usage(error);
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
      if (!std::filesystem::exists(working_directory / "forge.recipe.toml")
          && std::filesystem::exists(working_directory / "forge.workspace.toml"))
      {
        return clean_workspace(working_directory, output, error);
      }

      return clean_project(working_directory, output, error);
    }

    error << "forge: '" << arguments.front() << "' is not implemented yet\n";
    return 2;
  }

} // namespace forge::cli
