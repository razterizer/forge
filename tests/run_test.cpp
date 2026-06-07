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

  void test_run_builds_and_forwards_arguments()
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
        {
          std::filesystem::create_directories(directory.path() / ".forge/build");
#ifdef _WIN32
          std::ofstream executable { directory.path() / ".forge/build/hello.exe" };
#else
          std::ofstream executable { directory.path() / ".forge/build/hello" };
#endif
        }

        if (commands.size() == 3)
        {
          return 7;
        }

        return 0;
      };

    expect(
      forge::run_project(directory.path(), arguments, runner, output, error) == 7,
      "run returns the program exit status"
    );
    expect(commands.size() == 3, "run configures, builds, and launches the program");
    expect(commands[2].size() == 3, "run forwards program arguments");
    expect(commands[2][1] == "--message", "run forwards the first argument");
    expect(commands[2][2] == "hello world", "run preserves arguments containing spaces");
    expect(
      working_directories[2] == directory.path() / ".forge/build",
      "run launches from the staged runtime directory"
    );
    expect(contains(output.str(), "Running hello"), "run reports the launched project");
    expect(error.str().empty(), "successful run does not write an error");
  }

  void test_run_stops_when_build_fails()
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
      forge::run_project(directory.path(), {}, runner, output, error) == 2,
      "run reports a build failure"
    );
    expect(invocations == 1, "run does not launch after a build failure");
  }

} // namespace

int main()
{
  test_run_builds_and_forwards_arguments();
  test_run_stops_when_build_fails();

  return failures == 0 ? 0 : 1;
}
