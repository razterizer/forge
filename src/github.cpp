#include "github.h"

#include <array>
#include <fstream>
#include <iterator>
#include <sstream>
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

    std::string_view trim(std::string_view value)
    {
      const auto first = value.find_first_not_of(" \t\r");

      if (first == std::string_view::npos)
      {
        return {};
      }

      return value.substr(first, value.find_last_not_of(" \t\r") - first + 1);
    }

    bool is_safe_project_path(const std::filesystem::path& path)
    {
      if (path.empty() || path.is_absolute())
      {
        return false;
      }

      for (const auto& component : path)
      {
        if (component == ".." || component.empty())
        {
          return false;
        }
      }

      return true;
    }

    bool is_inside_project(const std::filesystem::path& project_directory,
                           const std::filesystem::path& path)
    {
      std::error_code filesystem_error;
      const auto project = std::filesystem::weakly_canonical(project_directory, filesystem_error);

      if (filesystem_error)
      {
        return false;
      }

      const auto candidate = std::filesystem::weakly_canonical(path, filesystem_error);

      if (filesystem_error)
      {
        return false;
      }

      auto project_component = project.begin();
      auto candidate_component = candidate.begin();

      for (; project_component != project.end(); ++project_component, ++candidate_component)
      {
        if (candidate_component == candidate.end() || *candidate_component != *project_component)
        {
          return false;
        }
      }

      return candidate_component != candidate.end();
    }

    std::string release_boxes_job()
    {
      return
        "  forge-release-boxes:\n"
        "    # forge-managed: release-boxes@1\n"
        "    name: Publish Forge cboxes\n"
        "    if: startsWith(github.ref, 'refs/tags/')\n"
        "    runs-on: ubuntu-latest\n"
        "    permissions:\n"
        "      contents: write\n"
        "    steps:\n"
        "      - name: Checkout repository\n"
        "        uses: actions/checkout@v4\n"
        "      - name: Checkout Forge\n"
        "        uses: actions/checkout@v4\n"
        "        with:\n"
        "          repository: razterizer/forge\n"
        "          path: .forge-bootstrap\n"
        "      - name: Build Forge\n"
        "        run: >-\n"
        "          cmake -S .forge-bootstrap -B .forge-bootstrap/build -G Ninja\n"
        "          -DCMAKE_BUILD_TYPE=Release -DFORGE_BUILD_TESTS=OFF\n"
        "          && cmake --build .forge-bootstrap/build\n"
        "      - name: Prepare Forge release boxes\n"
        "        run: ./.forge-bootstrap/build/forge workflow prepare-release\n"
        "      - name: Publish Forge release boxes\n"
        "        uses: ncipollo/release-action@v1\n"
        "        with:\n"
        "          allowUpdates: true\n"
        "          artifacts: boxes/*.cbox,boxes/*.sha256\n"
          "          tag: ${{ github.ref_name }}\n";
    }

    bool is_job_key(std::string_view line, std::string_view job_id)
    {
      if (!line.starts_with("  ") || line.starts_with("    "))
      {
        return false;
      }

      const auto key = trim(line.substr(2));
      return key == std::string { job_id } + ":"
        || key == "'" + std::string { job_id } + "':"
        || key == "\"" + std::string { job_id } + "\":";
    }

    bool inject_job(std::string_view workflow,
                    std::string_view job_id,
                    std::string_view managed_marker,
                    std::string_view job,
                    std::string& updated,
                    bool& already_present,
                    std::ostream& error)
    {
      std::size_t jobs_content = std::string_view::npos;
      std::size_t insertion = workflow.size();
      std::size_t offset = 0;
      bool found_jobs = false;
      bool found_job = false;
      bool found_marker = false;

      while (offset < workflow.size())
      {
        const auto newline = workflow.find('\n', offset);
        const auto end = newline == std::string_view::npos ? workflow.size() : newline;
        const auto line = workflow.substr(offset, end - offset);
        const auto content = trim(line);
        const auto top_level = !line.empty()
          && line.front() != ' '
          && line.front() != '\t'
          && line.front() != '#';

        if (top_level && content == "jobs:")
        {
          if (found_jobs)
          {
            error << "forge: workflow contains multiple top-level jobs sections\n";
            return false;
          }

          found_jobs = true;
          jobs_content = newline == std::string_view::npos ? workflow.size() : newline + 1;
        }
        else if (found_jobs && top_level)
        {
          insertion = offset;
          break;
        }
        else if (found_jobs && is_job_key(line, job_id))
        {
          found_job = true;
        }
        else if (found_job
                 && line.starts_with("  ")
                 && !line.starts_with("    ")
                 && content.ends_with(':'))
        {
          found_job = false;
        }
        else if (found_job && content == managed_marker)
        {
          found_marker = true;
        }

        if (newline == std::string_view::npos)
        {
          break;
        }

        offset = newline + 1;
      }

      if (!found_jobs)
      {
        error << "forge: workflow does not contain a top-level jobs section\n";
        return false;
      }

      if (found_job)
      {
        if (!found_marker)
        {
          error << "forge: workflow job '" << job_id << "' exists but is not Forge-managed\n";
          return false;
        }

        already_present = true;
        updated = std::string { workflow };
        return true;
      }

      already_present = false;
      updated = std::string { workflow.substr(0, insertion) };

      if (updated.size() > jobs_content && !updated.ends_with("\n\n"))
      {
        if (!updated.ends_with('\n'))
        {
          updated += '\n';
        }

        updated += '\n';
      }

      updated += job;

      if (insertion < workflow.size())
      {
        updated += '\n';
        updated += workflow.substr(insertion);
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
        "\n";

      if (platform == "windows")
      {
        workflow +=
          "      - name: Set up MSVC\n"
          "        uses: ilammy/msvc-dev-cmd@v1\n"
          "        with:\n"
          "          arch: x64\n"
          "\n";
      }

      workflow +=
        "      - name: Build Forge\n"
        "        run: >-\n"
        "          cmake -S .forge-bootstrap -B .forge-bootstrap/build -G Ninja\n"
        "          -DCMAKE_BUILD_TYPE=Release -DFORGE_BUILD_TESTS=OFF";

      if (platform == "windows")
      {
        workflow += " -DCMAKE_CXX_COMPILER=cl";
      }

      workflow +=
        "\n"
        "          && cmake --build .forge-bootstrap/build\n"
        "\n"
        "      - name: Prepare hosted release assets\n";

      if (!shell.empty())
      {
        workflow += "        shell: " + std::string { shell } + "\n";
      }

      workflow +=
        "        run: |\n"
        "          " + std::string { forge_executable } + " workflow prepare-release\n"
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

    std::string linux_release_job(std::string_view name,
                                  std::string_view runner,
                                  std::string_view compatibility)
    {
      return
        "  " + std::string { name } + ":\n"
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
        "      - name: Prepare hosted release assets\n"
        "        run: |\n"
        "          ./.forge-bootstrap/build/forge workflow prepare-release\n"
        "          mkdir hosted-assets\n"
        "          for archive in .forge/release/*.zip; do\n"
        "            [ -e \"$archive\" ] || continue\n"
        "            filename=$(basename \"${archive%.zip}\")\n"
        "            cp \"$archive\" \"hosted-assets/${filename}-" + std::string { compatibility } + ".zip\"\n"
        "          done\n"
        "          for box in boxes/*.cbox; do\n"
        "            [ -e \"$box\" ] || continue\n"
        "            filename=$(basename \"${box%.cbox}\")-" + std::string { compatibility } + ".cbox\n"
        "            cp \"$box\" \"hosted-assets/$filename\"\n"
        "            (cd hosted-assets && sha256sum \"$filename\" > \"$filename.sha256\")\n"
        "          done\n"
        "          cp .forge/release/RELEASE_NOTES.md hosted-assets/\n"
        "\n"
        "      - name: Upload Linux release assets\n"
        "        uses: actions/upload-artifact@v4\n"
        "        with:\n"
        "          name: " + std::string { compatibility } + "\n"
        "          path: hosted-assets/\n"
        "          retention-days: 1\n"
        "\n";
    }

    std::string linux_release_workflow()
    {
      return
        "name: release linux\n"
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
        + linux_release_job("build-modern", "ubuntu-latest", "linux-modern")
        + linux_release_job("build-legacy", "ubuntu-22.04", "linux-legacy")
        + "  release:\n"
          "    runs-on: ubuntu-latest\n"
          "    needs: [build-modern, build-legacy]\n"
          "\n"
          "    steps:\n"
          "      - name: Download modern Linux assets\n"
          "        uses: actions/download-artifact@v4\n"
          "        with:\n"
          "          name: linux-modern\n"
          "          path: linux-modern\n"
          "\n"
          "      - name: Download legacy Linux assets\n"
          "        uses: actions/download-artifact@v4\n"
          "        with:\n"
          "          name: linux-legacy\n"
          "          path: linux-legacy\n"
          "\n"
          "      - name: Publish GitHub release\n"
          "        uses: ncipollo/release-action@v1\n"
          "        with:\n"
          "          allowUpdates: true\n"
          "          artifacts: linux-modern/*.zip,linux-modern/*.cbox,linux-modern/*.sha256,linux-legacy/*.zip,linux-legacy/*.cbox,linux-legacy/*.sha256\n"
          "          bodyFile: linux-modern/RELEASE_NOTES.md\n"
          "          tag: ${{ github.ref_name }}\n";
    }

  } // namespace

  int add_github_workflow_feature(const std::filesystem::path& project_directory,
                                  std::string_view feature,
                                  const std::filesystem::path& workflow_file,
                                  bool apply,
                                  std::ostream& output,
                                  std::ostream& error)
  {
    if (feature != "release-boxes")
    {
      error << "forge: unknown workflow feature '" << feature << "'\n";
      return 2;
    }

    if (!is_safe_project_path(workflow_file))
    {
      error << "forge: workflow file must stay inside the project\n";
      return 2;
    }

    const auto path = project_directory / workflow_file;

    if (!is_inside_project(project_directory, path))
    {
      error << "forge: workflow file must stay inside the project\n";
      return 2;
    }

    std::ifstream input { path };

    if (!input)
    {
      error << "forge: could not read workflow '" << workflow_file.string() << "'\n";
      return 2;
    }

    const std::string workflow {
      std::istreambuf_iterator<char> { input },
      std::istreambuf_iterator<char> {}
    };
    std::string updated;
    bool already_present = false;

    if (!inject_job(
      workflow,
      "forge-release-boxes",
      "# forge-managed: release-boxes@1",
      release_boxes_job(),
      updated,
      already_present,
      error
    ))
    {
      return 2;
    }

    if (already_present)
    {
      output << "Workflow feature release-boxes is already present in "
             << workflow_file.string() << '\n';
      return 0;
    }

    if (!apply)
    {
      output
        << "Would add workflow feature release-boxes to " << workflow_file.string() << "\n\n"
        << release_boxes_job()
        << "\nRun again with --apply to update the workflow.\n";
      return 0;
    }

    std::ofstream file { path };
    file << updated;

    if (!file)
    {
      error << "forge: could not write workflow '" << workflow_file.string() << "'\n";
      return 2;
    }

    output << "Added workflow feature release-boxes to " << workflow_file.string() << '\n';
    return 0;
  }

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
        linux_release_workflow()
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
