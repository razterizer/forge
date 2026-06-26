#include "run.h"

#include <array>
#include <chrono>
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
      path_ = std::filesystem::temp_directory_path() / ("forge-run-test-" + std::to_string(suffix));
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

  void write_project(const std::filesystem::path& directory)
  {
    std::ofstream recipe { directory / "forge.recipe.toml" };
    recipe
      << "[project]\n"
      << "name = \"hello\"\n"
      << "version = \"0.1.0\"\n"
      << "type = \"executable\"\n"
      << "cpp_std = 20\n\n"
      << "[sources]\n"
      << "paths = [\"main.cpp\"]\n";

    std::ofstream source { directory / "main.cpp" };
    source << "int main() {}\n";
  }

  void write_executable(const std::filesystem::path& directory,
                        const std::filesystem::path& relative_directory = {})
  {
    std::filesystem::create_directories(directory / ".forge/build" / relative_directory);
#ifdef _WIN32
    std::ofstream executable { directory / ".forge/build" / relative_directory / "hello.exe" };
#else
    std::ofstream executable { directory / ".forge/build" / relative_directory / "hello" };
#endif
  }

  void test_run_forwards_arguments()
  {
    TemporaryDirectory directory;
    write_project(directory.path());
    write_executable(directory.path());
    std::vector<std::vector<std::string>> commands;
    std::vector<std::filesystem::path> working_directories;
    std::ostringstream output;
    std::ostringstream error;
    constexpr std::array arguments {
      std::string_view { "--message" },
      std::string_view { "hello world" }
    };

    const forge::ProcessRunner runner =
      [&commands, &working_directories](const std::vector<std::string>& command,
                                        const std::filesystem::path& working_directory,
                                        std::ostream&)
      {
        commands.push_back(command);
        working_directories.push_back(working_directory);
        return 7;
      };

    expect(
      forge::run_project(directory.path(), arguments, runner, output, error) == 7,
      "run returns the program exit status"
    );
    expect(commands.size() == 1, "run launches the existing program without building");
    expect(commands[0].size() == 3, "run forwards program arguments");
    expect(commands[0][1] == "--message", "run forwards the first argument");
    expect(commands[0][2] == "hello world", "run preserves arguments containing spaces");
    expect(
      working_directories[0] == directory.path() / ".forge/build",
      "run launches from the staged runtime directory"
    );
    expect(contains(output.str(), "Running hello"), "run reports the launched project");
    expect(error.str().empty(), "successful run does not write an error");
  }

  void test_build_and_run_builds_and_forwards_arguments()
  {
    TemporaryDirectory directory;
    write_project(directory.path());
    std::vector<std::vector<std::string>> commands;
    std::vector<std::filesystem::path> working_directories;
    std::ostringstream output;
    std::ostringstream error;
    constexpr std::array arguments {
      std::string_view { "--message" },
      std::string_view { "hello world" }
    };

    const forge::ProcessRunner runner =
      [&commands, &working_directories, &directory](const std::vector<std::string>& command,
                                                    const std::filesystem::path& working_directory,
                                                    std::ostream&)
      {
        commands.push_back(command);
        working_directories.push_back(working_directory);

        if (command.size() > 1 && command[1] == "--build")
          write_executable(directory.path());

        if (commands.size() == 3)
          return 7;

        return 0;
      };

    expect(
      forge::build_and_run_project(directory.path(), arguments, runner, output, error) == 7,
      "build-and-run returns the program exit status"
    );
    expect(commands.size() == 3, "build-and-run configures, builds, and launches the program");
    expect(commands[2].size() == 3, "build-and-run forwards program arguments");
    expect(commands[2][1] == "--message", "build-and-run forwards the first argument");
    expect(commands[2][2] == "hello world", "build-and-run preserves arguments containing spaces");
    expect(
      working_directories[2] == directory.path() / ".forge/build",
      "build-and-run launches from the staged runtime directory"
    );
    expect(contains(output.str(), "Running hello"), "build-and-run reports the launched project");
    expect(
      contains(output.str(), "profile default (Debug)"),
      "build-and-run reports the default launch profile"
    );
    expect(error.str().empty(), "successful build-and-run does not write an error");
  }

  void test_build_and_run_reports_selected_profile()
  {
    TemporaryDirectory directory;
    write_project(directory.path());
    {
      std::ofstream recipe { directory.path() / "forge.recipe.toml", std::ios::app };
      recipe
        << "\n[profile.Release.build]\n"
        << "configuration = \"Release\"\n";
    }
    std::ostringstream output;
    std::ostringstream error;

    const forge::ProcessRunner runner =
      [&directory](const std::vector<std::string>& command,
                   const std::filesystem::path&,
                   std::ostream&)
      {
        if (command.size() > 1 && command[1] == "--build")
          write_executable(directory.path());

        return 0;
      };

    expect(
      forge::build_and_run_project(
        directory.path(),
        std::nullopt,
        std::string { "Release" },
        {},
        runner,
        output,
        error
      ) == 0,
      "build-and-run succeeds with a selected build profile"
    );
    expect(
      contains(output.str(), "Running hello with profile Release (Release)"),
      "build-and-run reports the selected launch profile"
    );
    expect(error.str().empty(), "profiled build-and-run does not write an error");
  }

  void test_build_and_run_stops_when_build_fails()
  {
    TemporaryDirectory directory;
    write_project(directory.path());
    int invocations = 0;
    std::ostringstream output;
    std::ostringstream error;

    const forge::ProcessRunner runner =
      [&invocations](const std::vector<std::string>&,
                     const std::filesystem::path&,
                     std::ostream&)
      {
        ++invocations;
        return 1;
      };

    expect(
      forge::build_and_run_project(directory.path(), {}, runner, output, error) == 2,
      "build-and-run reports a build failure"
    );
    expect(invocations == 1, "build-and-run does not launch after a build failure");
  }

  void test_build_and_run_selects_named_target()
  {
    TemporaryDirectory directory;
    std::filesystem::create_directories(directory.path() / "Examples");
    std::filesystem::create_directories(directory.path() / "Tests");
    std::ofstream recipe { directory.path() / "forge.recipe.toml" };
    recipe
      << "[project]\n"
      << "name = \"hello-suite\"\n"
      << "version = \"0.1.0\"\n\n"
      << "[target.examples]\n"
      << "type = \"executable\"\n"
      << "cpp_std = 20\n"
      << "sources = [\"Examples/examples.cpp\"]\n\n"
      << "[target.unit_tests]\n"
      << "type = \"executable\"\n"
      << "cpp_std = 20\n"
      << "sources = [\"Tests/unit_tests.cpp\"]\n";
    recipe.close();
    std::ofstream { directory.path() / "Examples/examples.cpp" } << "int main() {}\n";
    std::ofstream { directory.path() / "Tests/unit_tests.cpp" } << "int main() {}\n";
    std::vector<std::vector<std::string>> commands;
    std::ostringstream output;
    std::ostringstream error;
    constexpr std::array arguments { std::string_view { "--message" } };

    const forge::ProcessRunner runner =
      [&commands, &directory](const std::vector<std::string>& command,
                             const std::filesystem::path&,
                             std::ostream&)
      {
        commands.push_back(command);

        if (command.size() > 1 && command[1] == "--build")
        {
          std::filesystem::create_directories(directory.path() / ".forge/build/examples");
#ifdef _WIN32
          std::ofstream { directory.path() / ".forge/build/examples/examples.exe" };
#else
          std::ofstream { directory.path() / ".forge/build/examples/examples" };
#endif
        }

        return 0;
      };

    expect(
      forge::build_and_run_project(
        directory.path(),
        std::string { "examples" },
        arguments,
        runner,
        output,
        error
      ) == 0,
      "build-and-run succeeds for a selected named target"
    );
    expect(commands.size() == 3, "named target build-and-run configures, builds, and launches");
    expect(commands[2][1] == "--message", "named target build-and-run forwards arguments");
    expect(contains(output.str(), "Running examples"), "build-and-run reports the selected named target");
    expect(error.str().empty(), "selected named target build-and-run does not write an error");
  }

} // namespace

int main()
{
  test_run_forwards_arguments();
  test_build_and_run_builds_and_forwards_arguments();
  test_build_and_run_reports_selected_profile();
  test_build_and_run_stops_when_build_fails();
  test_build_and_run_selects_named_target();

  return failures == 0 ? 0 : 1;
}
