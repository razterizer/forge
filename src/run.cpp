#include "run.h"

#include "build.h"
#include "recipe.h"

#include <string>
#include <system_error>
#include <vector>

namespace forge
{

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
                  std::span<const std::string_view> arguments,
                  const ProcessRunner& process_runner,
                  std::ostream& output,
                  std::ostream& error)
  {
    return run_project(
      project_directory,
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
    BuildOptions options;
    options.target = target;
    options.profile = profile;

    if (build_project(project_directory, options, process_runner, output, error) != 0)
    {
      return 2;
    }

    Recipe recipe;

    if (!read_recipe(project_directory / "forge.recipe.toml", recipe, error))
    {
      return 2;
    }

    if (!select_recipe_target(recipe, target, error))
    {
      return 2;
    }

    if (recipe.type != "executable")
    {
      error << "forge: target '" << recipe.name << "' is not executable\n";
      return 2;
    }

    auto build_directory = project_directory / ".forge" / "build";

    if (recipe.selected_target)
    {
      build_directory /= *recipe.selected_target;
    }

    auto executable = build_directory / recipe.name;

#ifdef _WIN32
    executable += ".exe";
#endif

    std::error_code filesystem_error;

    if (!std::filesystem::is_regular_file(executable, filesystem_error))
    {
      error << "forge: built executable '" << executable.string() << "' does not exist\n";
      return 2;
    }

    std::vector<std::string> process_arguments;
    process_arguments.reserve(arguments.size() + 1);
    process_arguments.push_back(executable.string());

    for (const auto argument : arguments)
    {
      process_arguments.emplace_back(argument);
    }

    output << "Running " << recipe.name << '\n' << std::flush;
    return process_runner(process_arguments, executable.parent_path(), error);
  }

} // namespace forge
