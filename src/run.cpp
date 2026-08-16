#include "run.h"

#include "build.h"
#include "recipe.h"
#include "target_support.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <map>
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
      constexpr std::string_view prefix { "configuration = \"" };

      for (const auto& filename : { "forge-toolchain.toml", "forge-run.toml" })
      {
        std::ifstream file { build_directory / filename };
        std::string line;

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
      }

      return std::nullopt;
    }

    std::optional<std::filesystem::path> current_run_directory(
      const std::filesystem::path& project_directory)
    {
      const auto run_root = project_directory / ".forge" / "run";
      std::ifstream current { run_root / "current.txt" };
      std::string relative;

      if (!std::getline(current, relative) || relative.empty())
        return std::nullopt;

      const auto path = std::filesystem::path { relative };

      if (path.is_absolute()
          || std::ranges::any_of(path, [](const auto& component)
          {
            return component == "..";
          }))
        return std::nullopt;

      std::error_code filesystem_error;
      const auto directory = run_root / path;
      return std::filesystem::is_directory(directory, filesystem_error)
        ? std::optional<std::filesystem::path> { directory }
        : std::nullopt;
    }

    std::optional<std::string> read_run_profile(
      const std::filesystem::path& run_directory)
    {
      std::ifstream file { run_directory / "forge-run.toml" };
      std::string line;
      constexpr std::string_view prefix { "profile = \"" };

      while (std::getline(file, line))
      {
        const std::string_view content { line };

        if (content.starts_with(prefix) && content.size() > prefix.size() && content.back() == '"')
          return std::string { content.substr(prefix.size(), content.size() - prefix.size() - 1) };
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

      auto cached_run = current_run_directory(project_directory);

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
        if (const auto cached_profile = read_run_profile(*cached_run); cached_profile && !cached_profile->empty())
          launch_profile.name = *cached_profile;

        if (const auto cached_configuration = read_build_configuration(*cached_run))
          launch_profile.configuration = *cached_configuration;
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

  bool select_cached_run_variant(
    const std::filesystem::path& project_directory,
    const std::optional<std::string>& target,
    const std::optional<std::string>& configuration,
    const std::optional<std::string>& style,
    const std::optional<std::string>& profile,
    std::ostream& error
  )
  {
    const auto run_root = project_directory / ".forge" / "run";
    std::optional<std::filesystem::path> selected;
    std::filesystem::file_time_type selected_time;
    std::error_code filesystem_error;

    for (const auto& entry : std::filesystem::recursive_directory_iterator { run_root, filesystem_error })
    {
      if (filesystem_error)
        break;

      if (entry.path().filename() != "forge-run.toml")
        continue;

      std::ifstream file { entry.path() };
      std::map<std::string, std::string> metadata;
      std::string line;

      while (std::getline(file, line))
      {
        const auto equals = line.find(" = \"");

        if (equals != std::string::npos && line.size() > equals + 4 && line.back() == '"')
          metadata[line.substr(0, equals)] = line.substr(equals + 4, line.size() - equals - 5);
      }

      const auto matches = [&](const std::optional<std::string>& expected, const char* key)
      {
        if (!expected)
          return true;

        const auto value = metadata.find(key);
        return value != metadata.end() && value->second == *expected;
      };
      auto configuration_matches = matches(configuration, "configuration");

      if (!configuration_matches && configuration && metadata.contains("configuration"))
      {
        auto left = metadata.at("configuration");
        auto right = *configuration;
        const auto lower = [](std::string& value)
        {
          std::ranges::transform(value, value.begin(), [](unsigned char byte)
          {
            return static_cast<char>(std::tolower(byte));
          });
        };
        lower(left);
        lower(right);
        configuration_matches = left == right;
      }

      if (!configuration_matches
          || !matches(target, "target")
          || !matches(style, "style")
          || !matches(profile, "profile"))
        continue;

      const auto modified = std::filesystem::last_write_time(entry.path(), filesystem_error);

      if (!filesystem_error && (!selected || modified > selected_time))
      {
        selected = entry.path().parent_path();
        selected_time = modified;
      }
    }

    if (!selected)
    {
      error << "forge: no cached run variant matches the requested selectors\n";
      return false;
    }

    const auto relative = selected->lexically_relative(run_root);
    std::ofstream current { run_root / "current.txt" };
    current << relative.generic_string() << '\n';

    if (!current)
    {
      error << "forge: could not select cached run variant\n";
      return false;
    }

    return true;
  }


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
