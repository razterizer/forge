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

  bool command_contains(const std::vector<std::string>& command, std::string_view argument)
  {
    for (const auto& value : command)
    {
      if (value == argument)
      {
        return true;
      }
    }

    return false;
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
    std::ofstream { directory / "RELEASE_NOTES.md" }
      << "# Release notes\n\n"
      << "## 0.1.0\n\n"
      << "- First release.\n\n"
      << "## 0.0.0\n\n"
      << "- Older release.\n";
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
      std::filesystem::exists(directory.path() / ".forge/release/RELEASE_NOTES.md"),
      "release writes notes for hosted release publication"
    );
    std::ifstream notes { directory.path() / ".forge/release/RELEASE_NOTES.md" };
    std::ostringstream notes_text;
    notes_text << notes.rdbuf();
    expect(contains(notes_text.str(), "First release."), "release extracts current notes");
    expect(!contains(notes_text.str(), "Older release."), "release excludes older notes");
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

  void test_release_rejects_missing_version_notes()
  {
    TemporaryDirectory directory;
    write_project(directory.path());
    std::ofstream { directory.path() / "RELEASE_NOTES.md" }
      << "# Release notes\n\n"
      << "## 0.0.0\n\n"
      << "- Older release.\n";
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
      "release rejects missing version notes"
    );
    expect(invocations == 2, "missing version notes are rejected before archiving");
    expect(contains(error.str(), "were not found"), "release explains missing version notes");
  }

  void test_release_creates_and_pushes_custom_tag()
  {
    TemporaryDirectory directory;
    write_project(directory.path());
    std::ofstream recipe { directory.path() / "forge.recipe.toml", std::ios::app };
    recipe << "\n[build]\nnumber = 7\n";
    recipe.close();
    std::vector<std::vector<std::string>> commands;
    std::ostringstream output;
    std::ostringstream error;
    forge::ReleaseOptions options;
    options.tag_format = "<name>-<version>+build.<build-nr>-<configuration>";

    const forge::ProcessRunner runner =
      [&commands, &directory](const std::vector<std::string>& command,
                             const std::filesystem::path&,
                             std::ostream&)
      {
        commands.push_back(command);

        if (command_contains(command, "show-ref"))
        {
          return 1;
        }

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
      forge::release_project(directory.path(), options, runner, output, error) == 0,
      "tagged release succeeds"
    );
    expect(commands.size() == 10, "tagged release preflights, releases, rechecks, tags, and pushes");
    expect(
      command_contains(commands[8], "hello-0.1.0+build.7-release"),
      "tagged release expands custom placeholders"
    );
    expect(command_contains(commands[8], "-F"), "tag annotation uses extracted release notes");
    expect(
      command_contains(commands[9], "refs/tags/hello-0.1.0+build.7-release"),
      "tagged release pushes the exact tag"
    );
    expect(contains(output.str(), "Tagged and pushed"), "tagged release reports the pushed tag");
    expect(error.str().empty(), "successful tagged release does not write an error");
  }

  void test_release_tag_rejects_dirty_tree_before_build()
  {
    TemporaryDirectory directory;
    write_project(directory.path());
    int invocations = 0;
    std::ostringstream output;
    std::ostringstream error;
    forge::ReleaseOptions options;
    options.tag_format = "release-<version>";

    const forge::ProcessRunner runner =
      [&invocations](const std::vector<std::string>& command,
                     const std::filesystem::path&,
                     std::ostream&)
      {
        ++invocations;
        return command_contains(command, "diff-index") ? 1 : 0;
      };

    expect(
      forge::release_project(directory.path(), options, runner, output, error) == 2,
      "tagged release rejects a dirty tree"
    );
    expect(invocations == 3, "dirty tree is rejected before building");
    expect(contains(error.str(), "clean working tree"), "tagged release explains dirty tree");
  }

  void test_release_tag_requires_declared_build_number()
  {
    TemporaryDirectory directory;
    write_project(directory.path());
    int invocations = 0;
    std::ostringstream output;
    std::ostringstream error;
    forge::ReleaseOptions options;
    options.tag_format = "release-<version>-<build-nr>";

    const forge::ProcessRunner runner =
      [&invocations](const std::vector<std::string>&,
                     const std::filesystem::path&,
                     std::ostream&)
      {
        ++invocations;
        return 0;
      };

    expect(
      forge::release_project(directory.path(), options, runner, output, error) == 2,
      "tagged release requires a declared build number"
    );
    expect(invocations == 0, "invalid tag format runs no processes");
    expect(contains(error.str(), "no build number"), "tagged release explains missing build number");
  }

} // namespace

int main()
{
  test_release_stages_files_and_creates_archive_command();
  test_release_reports_archive_failure();
  test_release_rejects_file_outside_project();
  test_release_rejects_missing_version_notes();
  test_release_creates_and_pushes_custom_tag();
  test_release_tag_rejects_dirty_tree_before_build();
  test_release_tag_requires_declared_build_number();

  return failures == 0 ? 0 : 1;
}
