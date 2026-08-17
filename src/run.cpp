#include "run.h"

#include "build.h"
#include "recipe.h"
#include "run_cache.h"
#include "target_support.h"

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
      for (const auto& filename : { "forge-toolchain.toml", "forge-run.toml" })
      {
        if (const auto configuration =
              read_run_cache_metadata(build_directory / filename).configuration)
        {
          return configuration;
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
      EffectiveBuildSelection effective_selection;

      if (!resolve_effective_build_selection(
        recipe,
        {
          .profile = profile,
          .system_profile = system_profile,
          .platform = current_target(),
          .build_configuration = launch_profile.configuration,
          .select_target = false
        },
        effective_selection,
        error
      ))
      {
        return false;
      }

      launch_profile.configuration = std::move(effective_selection.configuration);
      launch_profile.name = !effective_selection.selectors.profile.empty()
        ? effective_selection.selectors.profile
        : effective_selection.legacy_profile
          ? *effective_selection.legacy_profile
          : system_profile.value_or("default");
      return true;
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
      if (recipe.type != "executable")
      {
        error << "forge: target '" << recipe.name << "' is not executable\n";
        return 2;
      }

      auto cached_run = current_cached_run_directory(project_directory);

      if (cached_run)
      {
        auto cached_executable = *cached_run / recipe.name;
#ifdef _WIN32
        cached_executable += ".exe";
#endif
        std::error_code cache_error;

        if (!std::filesystem::is_regular_file(cached_executable, cache_error))
          cached_run.reset();
      }

      if (cached_run)
      {
        const auto metadata = read_run_cache_metadata(*cached_run / "forge-run.toml");

        if (metadata.profile && !metadata.profile->empty())
          launch_profile.name = *metadata.profile;

        if (metadata.configuration)
          launch_profile.configuration = *metadata.configuration;
      }
      else if (!resolve_launch_profile(recipe, profile, system_profile, launch_profile, error))
        return 2;

      auto build_directory = project_directory / ".forge" / "build";

      if (recipe.selected_target)
        build_directory /= *recipe.selected_target;

      const auto legacy_build_directory = build_directory;

      if (cached_run)
        build_directory = *cached_run;

      if (const auto configuration = read_build_configuration(build_directory))
        launch_profile.configuration = *configuration;

      auto executable = build_directory / recipe.name;

#ifdef _WIN32
      executable += ".exe";
#endif

      std::error_code filesystem_error;

      if (build_directory != legacy_build_directory
          && !std::filesystem::is_regular_file(executable, filesystem_error))
      {
        build_directory = legacy_build_directory;
        executable = build_directory / recipe.name;
#ifdef _WIN32
        executable += ".exe";
#endif
        filesystem_error.clear();
      }

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
