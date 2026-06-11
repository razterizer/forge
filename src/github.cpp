#include "github.h"

#include <array>
#include <fstream>
#include <iterator>
#include <optional>
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
        "    # forge-managed: release-boxes@2\n"
        "    name: Publish Forge cboxes\n"
        "    if: startsWith(github.ref, 'refs/tags/')\n"
        "    runs-on: ubuntu-latest\n"
        "    permissions:\n"
        "      contents: write\n"
        "    steps:\n"
        "      - name: Checkout repository\n"
        "        uses: actions/checkout@v4\n"
        "      - name: Resolve latest Forge release\n"
        "        id: forge-release\n"
        "        uses: actions/github-script@v7\n"
        "        with:\n"
        "          result-encoding: string\n"
        "          script: >-\n"
        "            return (await github.rest.repos.getLatestRelease({\n"
        "            owner: 'razterizer', repo: 'forge' })).data.tag_name\n"
        "      - name: Checkout Forge\n"
        "        uses: actions/checkout@v4\n"
        "        with:\n"
        "          repository: razterizer/forge\n"
        "          ref: ${{ steps.forge-release.outputs.result }}\n"
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

    struct WorkflowFeature
    {
      std::string_view name;
      std::string_view description;
      std::string_view job_id;
      std::string_view marker;
      std::string (*job)();
    };

    const std::array workflow_features {
      WorkflowFeature {
        "release-boxes",
        "Publish Forge cboxes and checksums from Git tag workflows",
        "forge-release-boxes",
        "# forge-managed: release-boxes@2",
        release_boxes_job
      }
    };

    const WorkflowFeature* find_workflow_feature(std::string_view name)
    {
      for (const auto& feature : workflow_features)
      {
        if (feature.name == name)
        {
          return &feature;
        }
      }

      return nullptr;
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

    bool is_any_job_key(std::string_view line)
    {
      if (!line.starts_with("  ") || line.starts_with("    "))
      {
        return false;
      }

      const auto key = trim(line.substr(2));
      return !key.empty() && !key.starts_with('#') && key.ends_with(':');
    }

    bool is_job_boundary_comment(std::string_view line)
    {
      return line.starts_with('#')
        || (line.starts_with("  #") && !line.starts_with("    #"));
    }

    struct WorkflowInspection
    {
      std::size_t jobs_content = std::string_view::npos;
      std::size_t insertion = 0;
      std::size_t job_start = std::string_view::npos;
      std::size_t job_end = std::string_view::npos;
      bool managed = false;
      bool current_marker = false;
      bool current_contents = false;
    };

    bool inspect_workflow(std::string_view workflow,
                          std::string_view job_id,
                          std::string_view feature,
                          std::string_view current_marker,
                          std::string_view current_job,
                          WorkflowInspection& inspection,
                          std::ostream& error)
    {
      inspection.insertion = workflow.size();
      std::size_t offset = 0;
      bool found_jobs = false;
      bool inside_job = false;

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
          inspection.jobs_content = newline == std::string_view::npos ? workflow.size() : newline + 1;
        }
        else if (found_jobs && top_level)
        {
          inspection.insertion = offset;

          if (inside_job)
          {
            inspection.job_end = offset;
          }

          break;
        }
        else if (found_jobs && is_job_key(line, job_id))
        {
          if (inspection.job_start != std::string_view::npos)
          {
            error << "forge: workflow contains multiple jobs named '" << job_id << "'\n";
            return false;
          }

          inspection.job_start = offset;
          inside_job = true;
        }
        else if (inside_job && is_any_job_key(line))
        {
          inspection.job_end = offset;
          inside_job = false;
        }
        else if (inside_job && is_job_boundary_comment(line))
        {
          inspection.job_end = offset;
          inside_job = false;
        }
        else if (inside_job
                 && content.starts_with("# forge-managed: " + std::string { feature } + "@"))
        {
          inspection.managed = true;
          inspection.current_marker = content == current_marker;
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

      if (inside_job)
      {
        inspection.job_end = inspection.insertion;
      }

      if (inspection.job_start != std::string_view::npos)
      {
        auto existing = workflow.substr(
          inspection.job_start,
          inspection.job_end - inspection.job_start
        );

        while (existing.ends_with('\n'))
        {
          existing.remove_suffix(1);
        }

        auto expected = current_job;

        while (expected.ends_with('\n'))
        {
          expected.remove_suffix(1);
        }

        inspection.current_contents = existing == expected;
      }

      return true;
    }

    bool validate_workflow_path(const std::filesystem::path& project_directory,
                                const std::filesystem::path& workflow_file,
                                std::filesystem::path& path,
                                std::ostream& error)
    {
      if (!is_safe_project_path(workflow_file))
      {
        error << "forge: workflow file must stay inside the project\n";
        return false;
      }

      path = project_directory / workflow_file;

      if (!is_inside_project(project_directory, path))
      {
        error << "forge: workflow file must stay inside the project\n";
        return false;
      }

      return true;
    }

    std::optional<std::string> read_workflow(const std::filesystem::path& path,
                                             const std::filesystem::path& workflow_file,
                                             std::ostream& error)
    {
      std::ifstream input { path };

      if (!input)
      {
        error << "forge: could not read workflow '" << workflow_file.string() << "'\n";
        return std::nullopt;
      }

      return std::string {
        std::istreambuf_iterator<char> { input },
        std::istreambuf_iterator<char> {}
      };
    }

    bool write_workflow(const std::filesystem::path& path,
                        const std::filesystem::path& workflow_file,
                        std::string_view workflow,
                        std::ostream& error)
    {
      std::ofstream file { path };
      file << workflow;

      if (!file)
      {
        error << "forge: could not write workflow '" << workflow_file.string() << "'\n";
        return false;
      }

      return true;
    }

    std::string add_job(std::string_view workflow,
                        const WorkflowInspection& inspection,
                        std::string_view job)
    {
      std::string updated { workflow.substr(0, inspection.insertion) };

      if (updated.size() > inspection.jobs_content && !updated.ends_with("\n\n"))
      {
        if (!updated.ends_with('\n'))
        {
          updated += '\n';
        }

        updated += '\n';
      }

      updated += job;

      if (inspection.insertion < workflow.size())
      {
        updated += '\n';
        updated += workflow.substr(inspection.insertion);
      }

      return updated;
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
        "      - name: Resolve latest Forge release\n"
        "        id: forge-release\n"
        "        uses: actions/github-script@v7\n"
        "        with:\n"
        "          result-encoding: string\n"
        "          script: >-\n"
        "            return (await github.rest.repos.getLatestRelease({\n"
        "            owner: 'razterizer', repo: 'forge' })).data.tag_name\n"
        "\n"
        "      - name: Checkout Forge\n"
        "        uses: actions/checkout@v4\n"
        "        with:\n"
        "          repository: razterizer/forge\n"
        "          ref: ${{ steps.forge-release.outputs.result }}\n"
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
        "      - name: Resolve latest Forge release\n"
        "        id: forge-release\n"
        "        uses: actions/github-script@v7\n"
        "        with:\n"
        "          result-encoding: string\n"
        "          script: >-\n"
        "            return (await github.rest.repos.getLatestRelease({\n"
        "            owner: 'razterizer', repo: 'forge' })).data.tag_name\n"
        "\n"
        "      - name: Checkout Forge\n"
        "        uses: actions/checkout@v4\n"
        "        with:\n"
        "          repository: razterizer/forge\n"
        "          ref: ${{ steps.forge-release.outputs.result }}\n"
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

  int list_github_workflow_features(std::ostream& output)
  {
    output << "Available workflow features:\n";

    for (const auto& feature : workflow_features)
    {
      output << "  " << feature.name << "  " << feature.description << '\n';
    }

    return 0;
  }

  int status_github_workflow_features(const std::filesystem::path& project_directory,
                                      const std::filesystem::path& workflow_file,
                                      std::ostream& output,
                                      std::ostream& error)
  {
    std::filesystem::path path;

    if (!validate_workflow_path(project_directory, workflow_file, path, error))
    {
      return 2;
    }

    const auto workflow = read_workflow(path, workflow_file, error);

    if (!workflow)
    {
      return 2;
    }

    output << "Workflow feature status for " << workflow_file.string() << ":\n";

    for (const auto& feature : workflow_features)
    {
      WorkflowInspection inspection;

      if (!inspect_workflow(
        *workflow,
        feature.job_id,
        feature.name,
        feature.marker,
        feature.job(),
        inspection,
        error
      ))
      {
        return 2;
      }

      output << "  " << feature.name << "  ";

      if (inspection.job_start == std::string_view::npos)
      {
        output << "missing\n";
      }
      else if (!inspection.managed)
      {
        output << "unmanaged collision\n";
      }
      else if (inspection.current_marker && inspection.current_contents)
      {
        output << "current\n";
      }
      else
      {
        output << "outdated\n";
      }
    }

    return 0;
  }

  int change_github_workflow_feature(const std::filesystem::path& project_directory,
                                     GithubWorkflowFeatureOperation operation,
                                     std::string_view feature,
                                     const std::filesystem::path& workflow_file,
                                     bool apply,
                                     std::ostream& output,
                                     std::ostream& error)
  {
    const auto definition = find_workflow_feature(feature);

    if (!definition)
    {
      error << "forge: unknown workflow feature '" << feature << "'\n";
      return 2;
    }

    std::filesystem::path path;

    if (!validate_workflow_path(project_directory, workflow_file, path, error))
    {
      return 2;
    }

    const auto workflow = read_workflow(path, workflow_file, error);

    if (!workflow)
    {
      return 2;
    }

    const auto job = definition->job();
    WorkflowInspection inspection;

    if (!inspect_workflow(
      *workflow,
      definition->job_id,
      definition->name,
      definition->marker,
      job,
      inspection,
      error
    ))
    {
      return 2;
    }

    std::string updated;

    if (operation == GithubWorkflowFeatureOperation::add)
    {
      if (inspection.job_start != std::string_view::npos)
      {
        if (!inspection.managed)
        {
          error << "forge: workflow job '" << definition->job_id
                << "' exists but is not Forge-managed\n";
          return 2;
        }

        output << "Workflow feature " << feature << " is already present in "
               << workflow_file.string();

        if (!inspection.current_marker || !inspection.current_contents)
        {
          output << " but is outdated; use workflow update-feature " << feature;
        }

        output << '\n';
        return 0;
      }

      updated = add_job(*workflow, inspection, job);
    }
    else if (operation == GithubWorkflowFeatureOperation::update)
    {
      if (inspection.job_start == std::string_view::npos)
      {
        error << "forge: workflow feature " << feature
              << " is not present; use workflow add-feature\n";
        return 2;
      }

      if (!inspection.managed)
      {
        error << "forge: workflow job '" << definition->job_id
              << "' exists but is not Forge-managed\n";
        return 2;
      }

      if (inspection.current_marker && inspection.current_contents)
      {
        output << "Workflow feature " << feature << " is current in "
               << workflow_file.string() << '\n';
        return 0;
      }

      updated = std::string { workflow->substr(0, inspection.job_start) };
      updated += job;

      if (inspection.job_end < workflow->size())
      {
        updated += '\n';
      }

      updated += workflow->substr(inspection.job_end);
    }
    else
    {
      if (inspection.job_start == std::string_view::npos)
      {
        output << "Workflow feature " << feature << " is not present in "
               << workflow_file.string() << '\n';
        return 0;
      }

      if (!inspection.managed)
      {
        error << "forge: workflow job '" << definition->job_id
              << "' exists but is not Forge-managed\n";
        return 2;
      }

      updated = std::string { workflow->substr(0, inspection.job_start) };
      updated += workflow->substr(inspection.job_end);
    }

    const auto operation_name =
      operation == GithubWorkflowFeatureOperation::add ? "add"
      : operation == GithubWorkflowFeatureOperation::update ? "update"
      : "remove";
    const auto completed_name =
      operation == GithubWorkflowFeatureOperation::add ? "Added"
      : operation == GithubWorkflowFeatureOperation::update ? "Updated"
      : "Removed";

    if (!apply)
    {
      const auto preposition =
        operation == GithubWorkflowFeatureOperation::add ? "to "
        : operation == GithubWorkflowFeatureOperation::remove ? "from "
        : "in ";
      output << "Would " << operation_name << " workflow feature " << feature << ' '
             << preposition
             << workflow_file.string() << '\n';

      if (operation != GithubWorkflowFeatureOperation::remove)
      {
        output << '\n' << job;
      }

      output << "\nRun again with --apply to update the workflow.\n";
      return 0;
    }

    if (!write_workflow(path, workflow_file, updated, error))
    {
      return 2;
    }

    const auto preposition =
      operation == GithubWorkflowFeatureOperation::add ? "to "
      : operation == GithubWorkflowFeatureOperation::remove ? "from "
      : "in ";
    output << completed_name << " workflow feature " << feature << ' '
           << preposition
           << workflow_file.string() << '\n';
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
