#include "test.h"

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
      path_ = std::filesystem::temp_directory_path()
        / ("forge-test-command-test-" + std::to_string(suffix));
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
    std::filesystem::create_directories(directory / "Tests");
    std::ofstream recipe { directory / "forge.recipe.toml" };
    recipe
      << "[project]\n"
      << "name = \"suite\"\n"
      << "version = \"0.1.0\"\n\n"
      << "[target.first]\n"
      << "type = \"executable\"\n"
      << "cpp_std = 20\n"
      << "sources = [\"Tests/first.cpp\"]\n"
      << "test = true\n\n"
      << "[target.second]\n"
      << "type = \"executable\"\n"
      << "cpp_std = 20\n"
      << "sources = [\"Tests/second.cpp\"]\n"
      << "test = true\n\n"
      << "[target.example]\n"
      << "type = \"executable\"\n"
      << "cpp_std = 20\n"
      << "sources = [\"Tests/example.cpp\"]\n";
    recipe.close();
    std::ofstream { directory / "Tests/first.cpp" } << "int main() {}\n";
    std::ofstream { directory / "Tests/second.cpp" } << "int main() {}\n";
    std::ofstream { directory / "Tests/example.cpp" } << "int main() {}\n";

    for (const auto name : { "first", "second" })
    {
      std::filesystem::create_directories(directory / ".forge/build" / name);
#ifdef _WIN32
      std::ofstream { directory / ".forge/build" / name / (std::string { name } + ".exe") };
#else
      std::ofstream { directory / ".forge/build" / name / name };
#endif
    }
  }

  void test_runs_all_marked_targets_and_aggregates_failures()
  {
    TemporaryDirectory directory;
    write_project(directory.path());
    std::vector<std::string> launched;
    std::ostringstream output;
    std::ostringstream error;

    const forge::ProcessRunner runner =
      [&launched](const std::vector<std::string>& command,
                  const std::filesystem::path&,
                  std::ostream&)
      {
        if (!command.empty()
            && command.front().find("first") != std::string::npos
            && command.front().find("cmake") == std::string::npos)
        {
          launched.push_back("first");
          return 5;
        }

        if (!command.empty()
            && command.front().find("second") != std::string::npos
            && command.front().find("cmake") == std::string::npos)
        {
          launched.push_back("second");
        }

        return 0;
      };

    expect(
      forge::test_project(directory.path(), std::nullopt, {}, runner, output, error) == 1,
      "test returns failure when a test target fails"
    );
    expect(launched.size() == 2, "test continues after a target fails");
    expect(contains(output.str(), "1 passed, 1 failed"), "test reports an aggregate summary");
  }

  void test_selects_target_and_forwards_arguments()
  {
    TemporaryDirectory directory;
    write_project(directory.path());
    std::vector<std::string> launched_command;
    std::ostringstream output;
    std::ostringstream error;
    constexpr std::array arguments { std::string_view { "--quick" } };

    const forge::ProcessRunner runner =
      [&launched_command](const std::vector<std::string>& command,
                          const std::filesystem::path&,
                          std::ostream&)
      {
        if (!command.empty()
            && command.front().find("second") != std::string::npos
            && command.front().find("cmake") == std::string::npos)
        {
          launched_command = command;
        }

        return 0;
      };

    expect(
      forge::test_project(
        directory.path(),
        std::string { "second" },
        arguments,
        runner,
        output,
        error
      ) == 0,
      "test succeeds for a selected marked target"
    );
    expect(launched_command.size() == 2, "test launches only the selected target with arguments");

    if (launched_command.size() == 2)
      expect(launched_command[1] == "--quick", "test forwards arguments to the selected target");

    expect(contains(output.str(), "1 passed, 0 failed"), "selected test reports its summary");
  }

  void test_rejects_unmarked_target()
  {
    TemporaryDirectory directory;
    write_project(directory.path());
    std::ostringstream output;
    std::ostringstream error;

    expect(
      forge::test_project(
        directory.path(),
        std::string { "example" },
        {},
        [](const auto&, const auto&, auto&) { return 0; },
        output,
        error
      ) == 2,
      "test rejects an unmarked target"
    );
    expect(contains(error.str(), "not marked as a test"), "unmarked target rejection is explained");
  }

  void test_rejects_non_executable_test_target()
  {
    TemporaryDirectory directory;
    std::filesystem::create_directories(directory.path() / "include/suite");
    std::ofstream recipe { directory.path() / "forge.recipe.toml" };
    recipe
      << "[project]\n"
      << "name = \"suite\"\n"
      << "version = \"0.1.0\"\n\n"
      << "[target.headers]\n"
      << "type = \"header_only\"\n"
      << "cpp_std = 20\n"
      << "sources = []\n"
      << "public_headers = [\"include/suite/suite.h\"]\n"
      << "test = true\n";
    recipe.close();
    std::ofstream { directory.path() / "include/suite/suite.h" } << "#pragma once\n";
    std::ostringstream output;
    std::ostringstream error;

    const forge::ProcessRunner runner =
      [](const std::vector<std::string>&,
         const std::filesystem::path&,
         std::ostream&)
      {
        return 0;
      };

    expect(
      forge::test_project(directory.path(), std::nullopt, {}, runner, output, error) == 2,
      "test rejects a marked non-executable target"
    );
    expect(contains(error.str(), "is not executable"), "non-executable test target is explained");
  }

} // namespace

int main()
{
  test_runs_all_marked_targets_and_aggregates_failures();
  test_selects_target_and_forwards_arguments();
  test_rejects_unmarked_target();
  test_rejects_non_executable_test_target();

  return failures == 0 ? 0 : 1;
}
