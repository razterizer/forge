#include "github.h"

#include <array>
#include <fstream>
#include <string>
#include <system_error>

namespace forge
{
  namespace
  {

    bool write_new_file(const std::filesystem::path& path,
                        std::string_view contents,
                        std::ostream& error)
    {
      if (std::filesystem::exists(path))
      {
        return true;
      }

      std::ofstream file { path };

      if (!file)
      {
        error << "forge: could not create '" << path.string() << "'\n";
        return false;
      }

      file << contents;

      if (!file)
      {
        error << "forge: could not write '" << path.string() << "'\n";
        return false;
      }

      return true;
    }

    std::string release_workflow(std::string_view platform,
                                 std::string_view runner,
                                 std::string_view forge_executable,
                                 std::string_view shell = {})
    {
      std::string workflow =
        "name: release " + std::string { platform } + "\n"
        "\n"
        "on:\n"
        "  push:\n"
        "    tags:\n"
        "      - \"release-*\"\n"
        "      - \"v*\"\n"
        "\n"
        "permissions:\n"
        "  contents: write\n"
        "\n"
        "jobs:\n"
        "  release:\n"
        "    runs-on: " + std::string { runner } + "\n"
        "\n"
        "    steps:\n"
        "      - name: Checkout repository\n"
        "        uses: actions/checkout@v4\n"
        "\n"
        "      - name: Checkout Forge\n"
        "        uses: actions/checkout@v4\n"
        "        with:\n"
        "          repository: razterizer/forge\n"
        "          path: .forge-bootstrap\n"
        "\n"
        "      - name: Build Forge\n"
        "        run: >-\n"
        "          cmake -S .forge-bootstrap -B .forge-bootstrap/build -G Ninja\n"
        "          -DCMAKE_BUILD_TYPE=Release -DFORGE_BUILD_TESTS=OFF\n"
        "          && cmake --build .forge-bootstrap/build\n"
        "\n"
        "      - name: Prepare hosted release assets\n";

      if (!shell.empty())
      {
        workflow += "        shell: " + std::string { shell } + "\n";
      }

      workflow +=
        "        run: |\n"
        "          " + std::string { forge_executable } + " prepare-release\n"
        "\n"
        "      - name: Publish GitHub release\n"
        "        uses: ncipollo/release-action@v1\n"
        "        with:\n"
        "          allowUpdates: true\n"
        "          artifacts: .forge/release/*.zip,boxes/*.cbox,boxes/*.sha256\n"
        "          bodyFile: .forge/release/RELEASE_NOTES.md\n"
        "          tag: ${{ github.ref_name }}\n";

      return workflow;
    }

  } // namespace

  bool generate_github_release_support(const std::filesystem::path& project_directory,
                                       std::ostream& error)
  {
    const auto workflows_directory = project_directory / ".github" / "workflows";
    std::error_code filesystem_error;
    std::filesystem::create_directories(workflows_directory, filesystem_error);

    if (filesystem_error)
    {
      error << "forge: could not create '" << workflows_directory.string() << "'\n";
      return false;
    }

    const std::array workflows {
      std::pair {
        "release-linux.yml",
        release_workflow(
          "linux",
          "ubuntu-latest",
          "./.forge-bootstrap/build/forge"
        )
      },
      std::pair {
        "release-macos.yml",
        release_workflow(
          "macos",
          "macos-latest",
          "./.forge-bootstrap/build/forge"
        )
      },
      std::pair {
        "release-windows.yml",
        release_workflow(
          "windows",
          "windows-latest",
          "./.forge-bootstrap/build/forge.exe",
          "pwsh"
        )
      }
    };

    for (const auto& [filename, contents] : workflows)
    {
      if (!write_new_file(workflows_directory / filename, contents, error))
      {
        return false;
      }
    }

    const std::string release_notes =
      "# Release notes\n"
      "\n"
      "## 0.1.0\n"
      "\n"
      "- Initial release.\n";

    constexpr std::string_view gitignore =
      "**/.forge/\n"
      "/build/\n"
      "/out/\n";

    return
      write_new_file(project_directory / "RELEASE_NOTES.md", release_notes, error)
      && write_new_file(project_directory / ".gitignore", gitignore, error);
  }

} // namespace forge
