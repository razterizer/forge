#include "test.h"

#include "recipe.h"
#include "run.h"

#include <algorithm>
#include <vector>

namespace forge
{

  int test_project(const std::filesystem::path& project_directory,
                   const std::optional<std::string>& target,
                   std::span<const std::string_view> arguments,
                   std::ostream& output,
                   std::ostream& error)
  {
    return test_project(project_directory, target, std::nullopt, arguments, run_process, output, error);
  }

  int test_project(const std::filesystem::path& project_directory,
                   const std::optional<std::string>& target,
                   const std::optional<std::string>& profile,
                   std::span<const std::string_view> arguments,
                   std::ostream& output,
                   std::ostream& error)
  {
    return test_project(project_directory, target, profile, arguments, run_process, output, error);
  }

  int test_project(const std::filesystem::path& project_directory,
                   const std::optional<std::string>& target,
                   std::span<const std::string_view> arguments,
                   const ProcessRunner& process_runner,
                   std::ostream& output,
                   std::ostream& error)
  {
    return test_project(
      project_directory,
      target,
      std::nullopt,
      arguments,
      process_runner,
      output,
      error
    );
  }

  int test_project(const std::filesystem::path& project_directory,
                   const std::optional<std::string>& target,
                   const std::optional<std::string>& profile,
                   std::span<const std::string_view> arguments,
                   const ProcessRunner& process_runner,
                   std::ostream& output,
                   std::ostream& error)
  {
    Recipe recipe;

    if (!read_recipe(project_directory / "forge.recipe.toml", recipe, error))
      return 2;

    std::vector<std::string> tests;

    if (target)
    {
      const auto selected = std::find_if(
        recipe.targets.begin(),
        recipe.targets.end(),
        [&target](const RecipeTarget& candidate)
        {
          return candidate.name == *target;
        }
      );

      if (selected == recipe.targets.end())
      {
        error << "forge: recipe has no target named '" << *target << "'\n";
        return 2;
      }

      if (!selected->test)
      {
        error << "forge: target '" << *target << "' is not marked as a test\n";
        return 2;
      }

      if (selected->type != "executable")
      {
        error << "forge: test target '" << *target << "' is not executable\n";
        return 2;
      }

      tests.push_back(*target);
    }
    else
    {
      for (const auto& candidate : recipe.targets)
      {
        if (candidate.test)
        {
          if (candidate.type != "executable")
          {
            error << "forge: test target '" << candidate.name << "' is not executable\n";
            return 2;
          }

          tests.push_back(candidate.name);
        }
      }
    }

    if (tests.empty())
    {
      error << "forge: recipe contains no named test targets\n";
      return 2;
    }

    std::size_t passed = 0;
    std::size_t failed = 0;
    bool command_failed = false;

    for (const auto& test : tests)
    {
      output << "Testing " << test << '\n';
      const auto result = build_and_run_project(
        project_directory,
        test,
        profile,
        arguments,
        process_runner,
        output,
        error
      );

      if (result == 0)
      {
        ++passed;
        output << "Passed " << test << '\n';
      }
      else
      {
        ++failed;
        command_failed = command_failed || result == 2;
        output << "Failed " << test << " (exit " << result << ")\n";
      }
    }

    output << "Tests: " << passed << " passed, " << failed << " failed\n";

    if (failed == 0)
      return 0;

    return command_failed ? 2 : 1;
  }

} // namespace forge
