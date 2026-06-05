#include "box.h"

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
      path_ = std::filesystem::temp_directory_path() / ("forge-box-test-" + std::to_string(suffix));
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

  void test_create_box_stages_manifest_and_executable()
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
      forge::create_box(directory.path(), runner, output, error) == 0,
      "box create succeeds when build and archive commands succeed"
    );
    expect(commands.size() == 3, "box create configures, builds, and archives");
    expect(commands[2].size() > 6 && commands[2][2] == "tar", "box create uses CMake archive support");

    const auto staging_root = directory.path() / ".forge/boxes/staging";
    std::filesystem::path manifest;

    for (const auto& entry : std::filesystem::directory_iterator { staging_root })
    {
      manifest = entry.path() / "cbox.toml";
    }

    expect(std::filesystem::exists(manifest), "box create stages a manifest");
    expect(contains(output.str(), "Created"), "box create reports its archive");
    expect(error.str().empty(), "successful box create does not write an error");
  }

  void test_inspect_prints_manifest()
  {
    TemporaryDirectory directory;
    const auto box = directory.path() / "hello.cbox";
    std::ofstream box_file { box };
    box_file << "placeholder";
    std::ostringstream output;
    std::ostringstream error;

    const forge::ProcessRunner runner =
      [](const std::vector<std::string>&,
         const std::filesystem::path& working_directory,
         std::ostream&)
      {
        std::ofstream manifest { working_directory / "cbox.toml" };
        manifest << "[cbox]\nformat = 1\n";
        return 0;
      };

    expect(
      forge::inspect_box(box, directory.path(), runner, output, error) == 0,
      "box inspect succeeds"
    );
    expect(contains(output.str(), "format = 1"), "box inspect prints the manifest");
  }

  void test_extract_refuses_existing_destination()
  {
    TemporaryDirectory directory;
    const auto box = directory.path() / "hello.cbox";
    std::ofstream box_file { box };
    box_file << "placeholder";
    std::filesystem::create_directory(directory.path() / "hello");
    int invocations = 0;
    std::ostringstream output;
    std::ostringstream error;

    const forge::ProcessRunner runner =
      [&invocations](const std::vector<std::string>&,
                     const std::filesystem::path&,
                     std::ostream&)
      {
        ++invocations;
        return 0;
      };

    expect(
      forge::extract_box(box, directory.path(), runner, output, error) == 2,
      "box extract refuses an existing destination"
    );
    expect(invocations == 0, "box extract does not invoke tools after validation failure");
  }

} // namespace

int main()
{
  test_create_box_stages_manifest_and_executable();
  test_inspect_prints_manifest();
  test_extract_refuses_existing_destination();

  return failures == 0 ? 0 : 1;
}

