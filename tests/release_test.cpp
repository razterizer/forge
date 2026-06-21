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

  std::string hosted_platform()
  {
#ifdef _WIN32
    return "windows";
#elif __APPLE__
    return "macos";
#elif __linux__
    return "linux";
#else
    return "unknown";
#endif
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
      << "[runtime]\n"
      << "files = [\"config/default.toml\"]\n\n"
      << "[release]\n"
      << "files = [\"assets\", \"RELEASE_NOTES.md\"]\n"
      << "readme = { linux = \"release/README_LINUX.md\", "
      << "macos = \"release/README_MACOS.md\", "
      << "windows = \"release/README_WINDOWS.md\" }\n";

    std::ofstream source { directory / "main.cpp" };
    source << "int main() {}\n";
    std::ofstream readme { directory / "README.md" };
    readme << "# hello\n";
    std::filesystem::create_directories(directory / "release");
    std::ofstream { directory / "release/README_LINUX.md" } << "linux release notes\n";
    std::ofstream { directory / "release/README_MACOS.md" } << "macos release notes\n";
    std::ofstream { directory / "release/README_WINDOWS.md" } << "windows release notes\n";
    std::filesystem::create_directories(directory / "assets/nested");
    std::filesystem::create_directories(directory / "assets/.forge");
    std::ofstream { directory / "assets/background.tx" } << "background\n";
    std::ofstream { directory / "assets/nested/colors.tx" } << "colors\n";
    std::ofstream { directory / "assets/.forge/generated.txt" } << "generated\n";
    std::filesystem::create_directories(directory / "config");
    std::ofstream { directory / "config/default.toml" } << "color = \"blue\"\n";
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
    std::ifstream release_readme { directory.path() / ".forge/release/hello-0.1.0/README.txt" };
    std::ostringstream release_readme_text;
    release_readme_text << release_readme.rdbuf();
    const auto expected_release_readme_text = hosted_platform() + " release notes";
    expect(
      contains(release_readme_text.str(), expected_release_readme_text),
      "release stages the host platform README as README.txt"
    );
    expect(
      std::filesystem::exists(
        directory.path() / ".forge/release/hello-0.1.0/assets/nested/colors.tx"
      ),
      "release stages a declared directory recursively"
    );
    expect(
      std::filesystem::exists(
        directory.path() / ".forge/release/hello-0.1.0/config/default.toml"
      ),
      "release stages declared runtime assets"
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

  void test_release_extracts_build_qualified_notes()
  {
    TemporaryDirectory directory;
    write_project(directory.path());
    std::ofstream recipe { directory.path() / "forge.recipe.toml", std::ios::app };
    recipe
      << "\n[build]\nnumber = 7\n"
      << "\n[release]\nbuild_number_format = \"dotted\"\n";
    recipe.close();
    std::ofstream { directory.path() / "RELEASE_NOTES.md" }
      << "# Release notes\n\n"
      << "## 0.1.0.7\n\n"
      << "- Build-qualified release.\n\n"
      << "## 0.1.0\n\n"
      << "- Unqualified release.\n";
    std::ostringstream output;
    std::ostringstream error;
    const forge::ProcessRunner runner =
      [&directory](const std::vector<std::string>& command,
                   const std::filesystem::path&,
                   std::ostream&)
      {
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
      "release accepts a build-qualified release-note heading"
    );
    std::ifstream notes { directory.path() / ".forge/release/RELEASE_NOTES.md" };
    std::ostringstream notes_text;
    notes_text << notes.rdbuf();
    expect(
      contains(notes_text.str(), "Build-qualified release.")
      && !contains(notes_text.str(), "Unqualified release."),
      "release extracts only the matching build-qualified notes"
    );
    expect(
      std::filesystem::is_directory(directory.path() / ".forge/release/hello-0.1.0+build.7"),
      "build-qualified executable release uses a distinct package name"
    );
    expect(error.str().empty(), "build-qualified release notes do not write an error");
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
    forge::GitReleaseOptions options;
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
      forge::release_git(directory.path(), options, runner, output, error) == 0,
      "GitHub release succeeds"
    );
    expect(commands.size() == 6, "GitHub release preflights, tags, and pushes");
    expect(
      command_contains(commands[4], "hello-0.1.0+build.7-release"),
      "GitHub release expands custom placeholders"
    );
    expect(
      command_contains(commands[4], "\n- First release.\n\n"),
      "tag annotation uses extracted release notes"
    );
    expect(
      command_contains(commands[5], "refs/tags/hello-0.1.0+build.7-release"),
      "GitHub release pushes the exact tag"
    );
    expect(contains(output.str(), "Tagged and pushed"), "GitHub release reports the pushed tag");
    expect(error.str().empty(), "successful GitHub release does not write an error");
  }

  void test_release_uses_build_qualified_default_tag()
  {
    TemporaryDirectory directory;
    write_project(directory.path());
    std::ofstream recipe { directory.path() / "forge.recipe.toml", std::ios::app };
    recipe
      << "\n[build]\nnumber = 7\n"
      << "\n[release]\nbuild_number_format = \"dotted\"\n";
    recipe.close();
    std::ofstream { directory.path() / "RELEASE_NOTES.md" }
      << "# Release notes\n\n## 0.1.0.7\n\n- Dotted release.\n";
    std::vector<std::vector<std::string>> commands;
    std::ostringstream output;
    std::ostringstream error;
    forge::GitReleaseOptions options;
    options.tag_format = "release-<version>";

    const forge::ProcessRunner runner =
      [&commands](const std::vector<std::string>& command,
                  const std::filesystem::path&,
                  std::ostream&)
      {
        commands.push_back(command);
        return command_contains(command, "show-ref") ? 1 : 0;
      };

    expect(
      forge::release_git(directory.path(), options, runner, output, error) == 0,
      "Git release accepts a build-qualified default tag"
    );
    expect(
      command_contains(commands[4], "release-0.1.0.7")
      && command_contains(commands[5], "refs/tags/release-0.1.0.7"),
      "default Git release tag uses the configured dotted version"
    );
    expect(error.str().empty(), "build-qualified default tag does not write an error");
  }

  void test_release_tag_rejects_dirty_tree_before_build()
  {
    TemporaryDirectory directory;
    write_project(directory.path());
    int invocations = 0;
    std::ostringstream output;
    std::ostringstream error;
    forge::GitReleaseOptions options;
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
      forge::release_git(directory.path(), options, runner, output, error) == 2,
      "GitHub release rejects a dirty tree"
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
    forge::GitReleaseOptions options;
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
      forge::release_git(directory.path(), options, runner, output, error) == 2,
      "GitHub release requires a declared build number"
    );
    expect(invocations == 0, "invalid tag format runs no processes");
    expect(contains(error.str(), "no build number"), "tagged release explains missing build number");
  }

  void test_release_tag_explains_existing_release()
  {
    TemporaryDirectory directory;
    write_project(directory.path());
    std::ostringstream output;
    std::ostringstream error;
    forge::GitReleaseOptions options;
    options.tag_format = "release-<version>";

    const forge::ProcessRunner runner =
      [](const std::vector<std::string>&,
         const std::filesystem::path&,
         std::ostream&)
      {
        return 0;
      };

    expect(
      forge::release_git(directory.path(), options, runner, output, error) == 2,
      "GitHub release rejects an existing local tag"
    );
    expect(contains(error.str(), "already exists locally"), "GitHub release explains existing tag");
    expect(contains(error.str(), "bump the recipe version"), "GitHub release suggests a new version");
  }

  void test_release_tag_force_replaces_existing_release()
  {
    TemporaryDirectory directory;
    write_project(directory.path());
    std::vector<std::vector<std::string>> commands;
    std::ostringstream output;
    std::ostringstream error;
    forge::GitReleaseOptions options;
    options.tag_format = "release-<version>";
    options.force_tag = true;

    const forge::ProcessRunner runner =
      [&commands](const std::vector<std::string>& command,
                  const std::filesystem::path&,
                  std::ostream&)
      {
        commands.push_back(command);
        return 0;
      };

    expect(
      forge::release_git(directory.path(), options, runner, output, error) == 0,
      "forced Git release replaces an existing tag"
    );
    expect(command_contains(commands[4], "--force"), "forced Git release replaces the local tag");
    expect(command_contains(commands[5], "--force"), "forced Git release replaces the remote tag");
    expect(contains(output.str(), "Force-tagged and pushed"), "forced Git release reports replacement");
    expect(error.str().empty(), "successful forced Git release does not write an error");
  }

} // namespace

int main()
{
  test_release_stages_files_and_creates_archive_command();
  test_release_reports_archive_failure();
  test_release_rejects_file_outside_project();
  test_release_rejects_missing_version_notes();
  test_release_extracts_build_qualified_notes();
  test_release_creates_and_pushes_custom_tag();
  test_release_uses_build_qualified_default_tag();
  test_release_tag_rejects_dirty_tree_before_build();
  test_release_tag_requires_declared_build_number();
  test_release_tag_explains_existing_release();
  test_release_tag_force_replaces_existing_release();

  return failures == 0 ? 0 : 1;
}
