#include "run.h"

#include "build.h"
#include "recipe.h"

#include <fstream>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace forge
{
  namespace
  {
    struct LaunchProfile
    {
      std::string name = "default";
      std::string configuration = "Debug";
    };

    std::optional<std::string> read_build_configuration(
      const std::filesystem::path& build_directory)
    {
      std::ifstream file { build_directory / "forge-toolchain.toml" };
      std::string line;
      constexpr std::string_view prefix { "configuration = \"" };

      while (std::getline(file, line))
      {
        const std::string_view content { line };

        if (content.starts_with(prefix)
            && content.size() > prefix.size()
            && content.back() == '"')
        {
          return std::string {
            content.substr(prefix.size(), content.size() - prefix.size() - 1)
          };
        }
      }

      return std::nullopt;
    }

    bool resolve_launch_profile(Recipe recipe,
                                const std::optional<std::string>& profile,
                                const std::optional<std::string>& system_profile,
                                LaunchProfile& launch_profile,
                                std::ostream& error)
    {
      if (profile)
        launch_profile.name = *profile;
      else if (system_profile)
        launch_profile.name = *system_profile;

      return select_dependency_profile(recipe, profile, true, error)
        && select_system_dependency_profile(recipe, system_profile, true, error)
        && select_build_profile(recipe, profile, true, launch_profile.configuration, error)
        && select_system_build_profile(
          recipe,
          system_profile,
          true,
          launch_profile.configuration,
          error
        );
    }

    int launch_project(const std::filesystem::path& project_directory,
                       const std::optional<std::string>& target,
                       const std::optional<std::string>& profile,
                       const std::optional<std::string>& system_profile,
                       std::span<const std::string_view> arguments,
                       const ProcessRunner& process_runner,
                       std::ostream& output,
                       std::ostream& error)
    {
      Recipe recipe;

      if (!read_recipe(project_directory / "forge.recipe.toml", recipe, error))
        return 2;

      if (!select_recipe_target(recipe, target, error))
        return 2;

      LaunchProfile launch_profile;
      if (!resolve_launch_profile(recipe, profile, system_profile, launch_profile, error))
        return 2;

      if (recipe.type != "executable")
      {
        error << "forge: target '" << recipe.name << "' is not executable\n";
        return 2;
      }

      auto build_directory = project_directory / ".forge" / "build";

      if (recipe.selected_target)
        build_directory /= *recipe.selected_target;

      if (const auto configuration = read_build_configuration(build_directory))
        launch_profile.configuration = *configuration;

      auto executable = build_directory / recipe.name;

#ifdef _WIN32
      executable += ".exe";
#endif

      std::error_code filesystem_error;

      if (!std::filesystem::is_regular_file(executable, filesystem_error))
      {
        error << "forge: executable '" << executable.string()
              << "' does not exist; run 'forge build' or 'forge build-and-run' first\n";
        return 2;
      }

      std::vector<std::string> process_arguments;
      process_arguments.reserve(arguments.size() + 1);
      process_arguments.push_back(executable.string());

      for (const auto argument : arguments)
        process_arguments.emplace_back(argument);

      output << "Running " << recipe.name << " with profile " << launch_profile.name
             << " (" << launch_profile.configuration << ")\n" << std::flush;
      return process_runner(process_arguments, executable.parent_path(), error);
    }

  } // namespace


  int run_project(const std::filesystem::path& project_directory,
                  std::span<const std::string_view> arguments,
                  std::ostream& output,
                  std::ostream& error)
  {
    return run_project(project_directory, std::nullopt, arguments, run_process, output, error);
  }

  int run_project(const std::filesystem::path& project_directory,
                  const std::optional<std::string>& target,
                  std::span<const std::string_view> arguments,
                  std::ostream& output,
                  std::ostream& error)
  {
    return run_project(project_directory, target, std::nullopt, arguments, run_process, output, error);
  }

  int run_project(const std::filesystem::path& project_directory,
                  const std::optional<std::string>& target,
                  const std::optional<std::string>& profile,
                  std::span<const std::string_view> arguments,
                  std::ostream& output,
                  std::ostream& error)
  {
    return run_project(project_directory, target, profile, arguments, run_process, output, error);
  }

  int run_project(const std::filesystem::path& project_directory,
                  const std::optional<std::string>& target,
                  const std::optional<std::string>& profile,
                  const std::optional<std::string>& system_profile,
                  std::span<const std::string_view> arguments,
                  std::ostream& output,
                  std::ostream& error)
  {
    return run_project(
      project_directory,
      target,
      profile,
      system_profile,
      arguments,
      run_process,
      output,
      error
    );
  }

  int run_project(const std::filesystem::path& project_directory,
                  std::span<const std::string_view> arguments,
                  const ProcessRunner& process_runner,
                  std::ostream& output,
                  std::ostream& error)
  {
    return run_project(
      project_directory,
      std::nullopt,
      std::nullopt,
      std::nullopt,
      arguments,
      process_runner,
      output,
      error
    );
  }

  int run_project(const std::filesystem::path& project_directory,
                  const std::optional<std::string>& target,
                  std::span<const std::string_view> arguments,
                  const ProcessRunner& process_runner,
                  std::ostream& output,
                  std::ostream& error)
  {
    return run_project(
      project_directory,
      target,
      std::nullopt,
      std::nullopt,
      arguments,
      process_runner,
      output,
      error
    );
  }

  int run_project(const std::filesystem::path& project_directory,
                  const std::optional<std::string>& target,
                  const std::optional<std::string>& profile,
                  std::span<const std::string_view> arguments,
                  const ProcessRunner& process_runner,
                  std::ostream& output,
                  std::ostream& error)
  {
    return run_project(
      project_directory,
      target,
      profile,
      std::nullopt,
      arguments,
      process_runner,
      output,
      error
    );
  }

  int run_project(const std::filesystem::path& project_directory,
                  const std::optional<std::string>& target,
                  const std::optional<std::string>& profile,
                  const std::optional<std::string>& system_profile,
                  std::span<const std::string_view> arguments,
                  const ProcessRunner& process_runner,
                  std::ostream& output,
                  std::ostream& error)
  {
    return launch_project(
      project_directory,
      target,
      profile,
      system_profile,
      arguments,
      process_runner,
      output,
      error
    );
  }

  int build_and_run_project(const std::filesystem::path& project_directory,
                            std::span<const std::string_view> arguments,
                            std::ostream& output,
                            std::ostream& error)
  {
    return build_and_run_project(project_directory, std::nullopt, arguments, run_process, output, error);
  }

  int build_and_run_project(const std::filesystem::path& project_directory,
                            const std::optional<std::string>& target,
                            std::span<const std::string_view> arguments,
                            std::ostream& output,
                            std::ostream& error)
  {
    return build_and_run_project(project_directory, target, std::nullopt, arguments, run_process, output, error);
  }

  int build_and_run_project(const std::filesystem::path& project_directory,
                            const std::optional<std::string>& target,
                            const std::optional<std::string>& profile,
                            std::span<const std::string_view> arguments,
                            std::ostream& output,
                            std::ostream& error)
  {
    return build_and_run_project(project_directory, target, profile, arguments, run_process, output, error);
  }

  int build_and_run_project(const std::filesystem::path& project_directory,
                            const std::optional<std::string>& target,
                            const std::optional<std::string>& profile,
                            const std::optional<std::string>& system_profile,
                            std::span<const std::string_view> arguments,
                            std::ostream& output,
                            std::ostream& error)
  {
    return build_and_run_project(
      project_directory,
      target,
      profile,
      system_profile,
      arguments,
      run_process,
      output,
      error
    );
  }

  int build_and_run_project(const std::filesystem::path& project_directory,
                            std::span<const std::string_view> arguments,
                            const ProcessRunner& process_runner,
                            std::ostream& output,
                            std::ostream& error)
  {
    return build_and_run_project(
      project_directory,
      std::nullopt,
      std::nullopt,
      std::nullopt,
      arguments,
      process_runner,
      output,
      error
    );
  }

  int build_and_run_project(const std::filesystem::path& project_directory,
                            const std::optional<std::string>& target,
                            std::span<const std::string_view> arguments,
                            const ProcessRunner& process_runner,
                            std::ostream& output,
                            std::ostream& error)
  {
    return build_and_run_project(
      project_directory,
      target,
      std::nullopt,
      std::nullopt,
      arguments,
      process_runner,
      output,
      error
    );
  }

  int build_and_run_project(const std::filesystem::path& project_directory,
                            const std::optional<std::string>& target,
                            const std::optional<std::string>& profile,
                            std::span<const std::string_view> arguments,
                            const ProcessRunner& process_runner,
                            std::ostream& output,
                            std::ostream& error)
  {
    return build_and_run_project(
      project_directory,
      target,
      profile,
      std::nullopt,
      arguments,
      process_runner,
      output,
      error
    );
  }

  int build_and_run_project(const std::filesystem::path& project_directory,
                            const std::optional<std::string>& target,
                            const std::optional<std::string>& profile,
                            const std::optional<std::string>& system_profile,
                            std::span<const std::string_view> arguments,
                            const ProcessRunner& process_runner,
                            std::ostream& output,
                            std::ostream& error)
  {
    BuildOptions options;
    options.target = target;
    options.profile = profile;
    options.system_profile = system_profile;

    if (build_project(project_directory, options, process_runner, output, error) != 0)
      return 2;

    return launch_project(
      project_directory,
      target,
      profile,
      system_profile,
      arguments,
      process_runner,
      output,
      error
    );
  }

  int build_and_run_project(const std::filesystem::path& project_directory,
                            const BuildOptions& options,
                            std::span<const std::string_view> arguments,
                            std::ostream& output,
                            std::ostream& error)
  {
    if (build_project(project_directory, options, run_process, output, error) != 0)
      return 2;

    return launch_project(
      project_directory,
      options.target,
      std::nullopt,
      std::nullopt,
      arguments,
      run_process,
      output,
      error
    );
  }

} // namespace forge
