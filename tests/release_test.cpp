#include "release.h"

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
      path_ = std::filesystem::temp_directory_path() / ("forge-release-test-" + std::to_string(suffix));
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
      << "paths = [\"main.cpp\"]\n\n"
      << "[release]\n"
      << "files = [\"assets\", \"RELEASE_NOTES.md\"]\n";

    std::ofstream source { directory / "main.cpp" };
    source << "int main() {}\n";
    std::ofstream readme { directory / "README.md" };
    readme << "# hello\n";
    std::filesystem::create_directories(directory / "assets/nested");
    std::filesystem::create_directories(directory / "assets/.forge");
    std::ofstream { directory / "assets/background.tx" } << "background\n";
    std::ofstream { directory / "assets/nested/colors.tx" } << "colors\n";
    std::ofstream { directory / "assets/.forge/generated.txt" } << "generated\n";
    std::ofstream { directory / "RELEASE_NOTES.md" } << "# Release notes\n";
  }

  void test_release_stages_files_and_creates_archive_command()
  {
    TemporaryDirectory directory;
    write_project(directory.path());
    std::vector<std::vector<std::string>> commands;
    std::ostringstream output;
    std::ostringstream error;

    const forge::ProcessRunner runner =
      [&commands, &directory](const std::vector<std::string>& command,
                             const std::filesystem::path&,
                             std::ostream&)
      {
        commands.push_back(command);

        if (command.size() > 1 && command[1] == "--build")
        {
          std::filesystem::create_directories(directory.path() / ".forge/build");
#ifdef _WIN32
          std::ofstream executable { directory.path() / ".forge/build/hello.exe" };
#else
          std::ofstream executable { directory.path() / ".forge/build/hello" };
#endif
        }

        return 0;
      };

    expect(
      forge::release_project(directory.path(), runner, output, error) == 0,
      "release succeeds when build and archive commands succeed"
    );
    expect(commands.size() == 3, "release configures, builds, and archives");
    expect(commands[2].size() > 6 && commands[2][2] == "tar", "release uses CMake archive support");
#ifdef _WIN32
    expect(
      std::filesystem::exists(directory.path() / ".forge/release/hello-0.1.0/hello.exe"),
      "release stages the executable"
    );
#else
    expect(
      std::filesystem::exists(directory.path() / ".forge/release/hello-0.1.0/hello"),
      "release stages the executable"
    );
#endif
    expect(
      std::filesystem::exists(directory.path() / ".forge/release/hello-0.1.0/README.md"),
      "release stages an optional readme"
    );
    expect(
      std::filesystem::exists(
        directory.path() / ".forge/release/hello-0.1.0/assets/nested/colors.tx"
      ),
      "release stages a declared directory recursively"
    );
    expect(
      std::filesystem::exists(
        directory.path() / ".forge/release/hello-0.1.0/RELEASE_NOTES.md"
      ),
      "release stages a declared file"
    );
    expect(
      !std::filesystem::exists(
        directory.path() / ".forge/release/hello-0.1.0/assets/.forge"
      ),
      "release excludes nested Forge state"
    );
    expect(contains(output.str(), "Released"), "release reports the archive");
    expect(error.str().empty(), "successful release does not write an error");
  }

  void test_release_reports_archive_failure()
  {
    TemporaryDirectory directory;
    write_project(directory.path());
    int invocations = 0;
    std::ostringstream output;
    std::ostringstream error;

    const forge::ProcessRunner runner =
      [&invocations, &directory](const std::vector<std::string>& command,
                                 const std::filesystem::path&,
                                 std::ostream&)
      {
        ++invocations;

        if (command.size() > 1 && command[1] == "--build")
        {
          std::filesystem::create_directories(directory.path() / ".forge/build");
#ifdef _WIN32
          std::ofstream executable { directory.path() / ".forge/build/hello.exe" };
#else
          std::ofstream executable { directory.path() / ".forge/build/hello" };
#endif
        }

        return invocations == 3 ? 1 : 0;
      };

    expect(
      forge::release_project(directory.path(), runner, output, error) == 2,
      "release reports archive failure"
    );
    expect(contains(error.str(), "archive creation failed"), "release explains archive failure");
  }

  void test_release_rejects_file_outside_project()
  {
    TemporaryDirectory directory;
    write_project(directory.path());
    std::ofstream recipe { directory.path() / "forge.recipe.toml", std::ios::app };
    recipe << "files = [\"../secret.txt\"]\n";
    recipe.close();
    int invocations = 0;
    std::ostringstream output;
    std::ostringstream error;

    const forge::ProcessRunner runner =
      [&invocations, &directory](const std::vector<std::string>& command,
                                 const std::filesystem::path&,
                                 std::ostream&)
      {
        ++invocations;

        if (command.size() > 1 && command[1] == "--build")
        {
          std::filesystem::create_directories(directory.path() / ".forge/build");
#ifdef _WIN32
          std::ofstream executable { directory.path() / ".forge/build/hello.exe" };
#else
          std::ofstream executable { directory.path() / ".forge/build/hello" };
#endif
        }

        return 0;
      };

    expect(
      forge::release_project(directory.path(), runner, output, error) == 2,
      "release rejects a file outside the project"
    );
    expect(invocations == 2, "unsafe release files are rejected before archiving");
    expect(contains(error.str(), "must stay inside"), "release explains unsafe file paths");
  }

} // namespace

int main()
{
  test_release_stages_files_and_creates_archive_command();
  test_release_reports_archive_failure();
  test_release_rejects_file_outside_project();

  return failures == 0 ? 0 : 1;
}
