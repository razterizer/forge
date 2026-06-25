#include "workspace.h"

#include <chrono>
#include <array>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace
{

  int failures = 0;

  class TemporaryDirectory
  {
  public:
    TemporaryDirectory()
    {
      const auto suffix = std::chrono::steady_clock::now().time_since_epoch().count();
      path_ = std::filesystem::temp_directory_path() / ("forge-workspace-test-" + std::to_string(suffix));
      std::filesystem::create_directories(path_);
    }

    ~TemporaryDirectory()
    {
      std::error_code error;
      std::filesystem::remove_all(path_, error);
    }

    const std::filesystem::path& path() const
    {
      return path_;
    }

  private:
    std::filesystem::path path_;
  };

  void expect(bool condition, std::string_view message)
  {
    if (!condition)
    {
      std::cerr << "FAIL: " << message << '\n';
      ++failures;
    }
  }

  bool contains(const std::string& text, std::string_view fragment)
  {
    return text.find(fragment) != std::string::npos;
  }

  void write_project(const std::filesystem::path& directory,
                     std::string_view name,
                     std::string_view dependency = {})
  {
    std::filesystem::create_directories(directory);
    std::ofstream recipe { directory / "forge.recipe.toml" };
    recipe
      << "[project]\n"
      << "name = \"" << name << "\"\n"
      << "version = \"0.1.0\"\n"
      << "type = \"executable\"\n"
      << "cpp_std = 20\n\n"
      << "[sources]\n"
      << "paths = [\"main.cpp\"]\n";

    if (!dependency.empty())
    {
      recipe
        << "\n[dependencies]\n"
        << dependency << " = { path = \"../" << dependency << "\" }\n";
    }

    std::ofstream { directory / "main.cpp" } << "int main() {}\n";
  }

  void write_workspace(const std::filesystem::path& directory,
                       std::string_view projects)
  {
    std::ofstream workspace { directory / "forge.workspace.toml" };
    workspace
      << "[workspace]\n"
      << "name = \"suite\"\n"
      << "projects = [" << projects << "]\n";
  }

  void write_named_project(const std::filesystem::path& directory,
                           std::string_view name)
  {
    std::filesystem::create_directories(directory / "Tests");
    std::filesystem::create_directories(directory / "Examples");
    std::ofstream recipe { directory / "forge.recipe.toml" };
    recipe
      << "[project]\n"
      << "name = \"" << name << "\"\n"
      << "version = \"0.1.0\"\n\n"
      << "[target.unit_tests]\n"
      << "type = \"executable\"\n"
      << "cpp_std = 20\n"
      << "sources = [\"Tests/unit_tests.cpp\"]\n"
      << "test = true\n\n"
      << "[target.examples]\n"
      << "type = \"executable\"\n"
      << "cpp_std = 20\n"
      << "sources = [\"Examples/examples.cpp\"]\n";
    std::ofstream { directory / "Tests/unit_tests.cpp" } << "int main() {}\n";
    std::ofstream { directory / "Examples/examples.cpp" } << "int main() {}\n";
  }

  void stage_executable(const std::filesystem::path& directory,
                        std::string_view name,
                        std::string_view target = {})
  {
    auto build_directory = directory / ".forge/build";

    if (!target.empty())
    {
      build_directory /= target;
    }

    std::filesystem::create_directories(build_directory);
#ifdef _WIN32
    std::ofstream { build_directory / (std::string { name } + ".exe") };
#else
    std::ofstream { build_directory / name };
#endif
  }

  forge::ProcessRunner successful_runner(std::vector<std::filesystem::path>& directories)
  {
    return [&directories](const std::vector<std::string>&,
                          const std::filesystem::path& directory,
                          std::ostream&)
    {
      directories.push_back(directory);
      return 0;
    };
  }

  void test_workspace_builds_all_projects()
  {
    TemporaryDirectory directory;
    write_workspace(directory.path(), "\"first\", \"second\"");
    write_project(directory.path() / "first", "first");
    write_project(directory.path() / "second", "second");
    std::vector<std::filesystem::path> directories;
    std::ostringstream output;
    std::ostringstream error;

    expect(
      forge::build_workspace(
        directory.path(),
        std::nullopt,
        std::nullopt,
        successful_runner(directories),
        output,
        error
      ) == 0,
      "workspace build succeeds"
    );
    expect(directories.size() == 4, "workspace configures and builds every project");
    expect(contains(output.str(), "Building workspace suite"), "workspace build reports its start");
    expect(contains(output.str(), "Built workspace suite"), "workspace build reports completion");
    expect(error.str().empty(), "workspace build does not write an error");
  }

  void test_workspace_builds_selected_project()
  {
    TemporaryDirectory directory;
    write_workspace(directory.path(), "\"first\", \"second\"");
    write_project(directory.path() / "first", "first");
    write_project(directory.path() / "second", "second");
    std::vector<std::filesystem::path> directories;
    std::ostringstream output;
    std::ostringstream error;

    expect(
      forge::build_workspace(
        directory.path(),
        std::string { "second" },
        std::nullopt,
        successful_runner(directories),
        output,
        error
      ) == 0,
      "selected workspace project build succeeds"
    );
    expect(directories.size() == 2, "selected workspace build invokes one project");
    expect(
      !directories.empty() && directories.front().filename() == "second",
      "selected workspace build invokes the requested project"
    );
  }

  void test_workspace_rejects_missing_project()
  {
    TemporaryDirectory directory;
    write_workspace(directory.path(), "\"first\"");
    write_project(directory.path() / "first", "first");
    std::vector<std::filesystem::path> directories;
    std::ostringstream output;
    std::ostringstream error;

    expect(
      forge::build_workspace(
        directory.path(),
        std::string { "missing" },
        std::nullopt,
        successful_runner(directories),
        output,
        error
      ) == 2,
      "workspace rejects a missing selected project"
    );
    expect(directories.empty(), "missing workspace project invokes no tools");
    expect(contains(error.str(), "no project named 'missing'"), "missing project is explained");
  }

  void test_workspace_rejects_duplicate_project()
  {
    TemporaryDirectory directory;
    write_workspace(directory.path(), "\"first\", \"first\"");
    write_project(directory.path() / "first", "first");
    std::vector<std::filesystem::path> directories;
    std::ostringstream output;
    std::ostringstream error;

    expect(
      forge::build_workspace(
        directory.path(),
        std::nullopt,
        std::nullopt,
        successful_runner(directories),
        output,
        error
      ) == 2,
      "workspace rejects a duplicate project"
    );
    expect(contains(error.str(), "duplicate project name"), "duplicate project is explained");
  }

  void test_workspace_rejects_dependency_cycle()
  {
    TemporaryDirectory directory;
    write_workspace(directory.path(), "\"first\", \"second\"");
    write_project(directory.path() / "first", "first", "second");
    write_project(directory.path() / "second", "second", "first");
    std::vector<std::filesystem::path> directories;
    std::ostringstream output;
    std::ostringstream error;

    expect(
      forge::build_workspace(
        directory.path(),
        std::nullopt,
        std::nullopt,
        successful_runner(directories),
        output,
        error
      ) == 2,
      "workspace rejects a dependency cycle"
    );
    expect(directories.empty(), "workspace cycle invokes no tools");
    expect(contains(error.str(), "workspace dependency cycle"), "workspace cycle is explained");
  }

  void test_workspace_runs_selected_project()
  {
    TemporaryDirectory directory;
    write_workspace(directory.path(), "\"app\"");
    write_project(directory.path() / "app", "app");
    stage_executable(directory.path() / "app", "app");
    std::vector<std::vector<std::string>> commands;
    std::ostringstream output;
    std::ostringstream error;
    constexpr std::array arguments { std::string_view { "--quick" } };

    const forge::ProcessRunner runner =
      [&commands](const std::vector<std::string>& command,
                  const std::filesystem::path&,
                  std::ostream&)
      {
        commands.push_back(command);
        return 0;
      };

    expect(
      forge::run_workspace(
        directory.path(),
        "app",
        std::nullopt,
        arguments,
        runner,
        output,
        error
      ) == 0,
      "workspace runs a selected project"
    );
    expect(commands.size() == 1, "workspace run launches the existing program without building");
    expect(commands.back().size() == 2 && commands.back()[1] == "--quick", "workspace run forwards arguments");
    expect(contains(output.str(), "Running app"), "workspace run reports the project");
  }

  void test_workspace_runs_selected_named_target()
  {
    TemporaryDirectory directory;
    write_workspace(directory.path(), "\"suite\"");
    write_named_project(directory.path() / "suite", "suite");
    stage_executable(directory.path() / "suite", "examples", "examples");
    std::vector<std::vector<std::string>> commands;
    std::ostringstream output;
    std::ostringstream error;

    const forge::ProcessRunner runner =
      [&commands](const std::vector<std::string>& command,
                  const std::filesystem::path&,
                  std::ostream&)
      {
        commands.push_back(command);
        return 0;
      };

    expect(
      forge::run_workspace(
        directory.path(),
        "suite/examples",
        std::nullopt,
        {},
        runner,
        output,
        error
      ) == 0,
      "workspace runs a selected named target"
    );
    expect(contains(output.str(), "Running examples"), "workspace run reports the named target");
  }

  void test_workspace_tests_all_projects()
  {
    TemporaryDirectory directory;
    write_workspace(directory.path(), "\"library\", \"first\", \"second\"");
    write_project(directory.path() / "library", "library");
    write_named_project(directory.path() / "first", "first");
    write_named_project(directory.path() / "second", "second");
    stage_executable(directory.path() / "first", "unit_tests", "unit_tests");
    stage_executable(directory.path() / "second", "unit_tests", "unit_tests");
    std::vector<std::string> launched;
    std::ostringstream output;
    std::ostringstream error;

    const forge::ProcessRunner runner =
      [&launched](const std::vector<std::string>& command,
                  const std::filesystem::path&,
                  std::ostream&)
      {
        if (!command.empty() && command.front().find("unit_tests") != std::string::npos)
        {
          launched.push_back(command.front());
        }

        return 0;
      };

    expect(
      forge::test_workspace(
        directory.path(),
        std::nullopt,
        std::nullopt,
        {},
        runner,
        output,
        error
      ) == 0,
      "workspace tests all projects with marked tests"
    );
    expect(launched.size() == 2, "workspace skips projects without marked tests");
    expect(
      contains(output.str(), "Workspace tests: 2 projects passed, 0 failed"),
      "workspace reports aggregate project test results"
    );
  }

  void test_workspace_tests_selected_named_target()
  {
    TemporaryDirectory directory;
    write_workspace(directory.path(), "\"suite\"");
    write_named_project(directory.path() / "suite", "suite");
    stage_executable(directory.path() / "suite", "unit_tests", "unit_tests");
    std::ostringstream output;
    std::ostringstream error;
    constexpr std::array arguments { std::string_view { "--quick" } };
    std::vector<std::string> launched;

    const forge::ProcessRunner runner =
      [&launched](const std::vector<std::string>& command,
                  const std::filesystem::path&,
                  std::ostream&)
      {
        if (!command.empty() && command.front().find("unit_tests") != std::string::npos)
        {
          launched = command;
        }

        return 0;
      };

    expect(
      forge::test_workspace(
        directory.path(),
        std::string { "suite/unit_tests" },
        std::nullopt,
        arguments,
        runner,
        output,
        error
      ) == 0,
      "workspace tests a selected named target"
    );
    expect(launched.size() == 2 && launched[1] == "--quick", "workspace test forwards arguments");
  }

} // namespace

int main()
{
  test_workspace_builds_all_projects();
  test_workspace_builds_selected_project();
  test_workspace_rejects_missing_project();
  test_workspace_rejects_duplicate_project();
  test_workspace_rejects_dependency_cycle();
  test_workspace_runs_selected_project();
  test_workspace_runs_selected_named_target();
  test_workspace_tests_all_projects();
  test_workspace_tests_selected_named_target();

  return failures == 0 ? 0 : 1;
}
