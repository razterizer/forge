#include "doctor.h"
#include "file_support.h"
#include "project_scan.h"
#include "recipe.h"

#include <filesystem>
#include <fstream>
#include <ostream>
#include <string>
#include <string_view>
#include <vector>

namespace forge
{
  namespace
  {

    struct DoctorState
    {
      int errors = 0;
      int warnings = 0;
      int checked_paths = 0;
      int github_dependencies = 0;
      int local_dependencies = 0;
      int box_dependencies = 0;
    };

    std::string_view trim(std::string_view value)
    {
      const auto first = value.find_first_not_of(" \t\r\n");

      if (first == std::string_view::npos)
        return {};

      const auto last = value.find_last_not_of(" \t\r\n");
      return value.substr(first, last - first + 1);
    }

    bool is_release_heading(std::string_view line, std::string_view version)
    {
      line = trim(line);

      if (!line.starts_with("##"))
        return false;

      line.remove_prefix(2);
      return trim(line) == version;
    }

    std::string release_notes_heading(const Recipe& recipe)
    {
      auto heading = recipe.version;

      if (recipe.release_notes_build_number_format && recipe.build_number)
      {
        heading += *recipe.release_notes_build_number_format == "dotted"
          ? "." + std::to_string(*recipe.build_number)
          : "+build." + std::to_string(*recipe.build_number);
      }

      return heading;
    }

    bool release_notes_contain_heading(const std::filesystem::path& path,
                                       std::string_view heading)
    {
      std::ifstream file { path };

      if (!file)
        return false;

      std::string line;

      while (std::getline(file, line))
      {
        if (is_release_heading(line, heading))
          return true;
      }

      return false;
    }

    void report_error(std::ostream& output,
                      DoctorState& state,
                      std::string_view message)
    {
      ++state.errors;
      output << "error: " << message << '\n';
    }

    void report_warning(std::ostream& output,
                        DoctorState& state,
                        std::string_view message)
    {
      ++state.warnings;
      output << "warning: " << message << '\n';
    }

    int doctor_unadopted_project(const std::filesystem::path& project_directory,
                                 std::ostream& output,
                                 std::ostream& error)
    {
      DoctorState state;
      ProjectScan scan;

      if (!scan_project(project_directory, scan, error))
        return 2;

      output << "Checking unadopted project " << project_directory.filename().string() << '\n';
      report_warning(output, state, "forge.recipe.toml is missing; run 'forge adopt' to create Forge metadata");
      output
        << "Found " << scan.sources.size() << " C++ source file"
        << (scan.sources.size() == 1 ? "" : "s") << '\n'
        << "Found " << scan.headers.size() << " C++ header file"
        << (scan.headers.size() == 1 ? "" : "s") << '\n'
        << "Found " << scan.public_headers.size() << " public header"
        << (scan.public_headers.size() == 1 ? "" : "s") << " under include/\n"
        << "Found " << scan.entry_points.size() << " entry point"
        << (scan.entry_points.size() == 1 ? "" : "s") << '\n';

      if (scan.sources.empty() && scan.headers.empty())
        report_error(output, state, "no C++ sources or headers were found to adopt");

      output
        << "Forge doctor found " << state.errors << " errors and "
        << state.warnings << " warnings\n";

      return state.errors == 0 ? 0 : 2;
    }

    void check_project_path(const std::filesystem::path& project_directory,
                            const std::filesystem::path& path,
                            std::string_view label,
                            bool require_directory,
                            DoctorState& state,
                            std::ostream& output)
    {
      if (path.empty())
        return;

      ++state.checked_paths;

      if (!is_safe_project_path(path))
      {
        report_error(output, state, std::string { label } + " must stay inside the project: "
          + path.generic_string());
        return;
      }

      const auto full_path = project_directory / path;
      const auto exists = require_directory
        ? std::filesystem::is_directory(full_path)
        : std::filesystem::exists(full_path);

      if (!exists)
      {
        report_error(output, state, std::string { label } + " does not exist: "
          + path.generic_string());
      }
    }

    void check_project_paths(const std::filesystem::path& project_directory,
                             const std::vector<std::filesystem::path>& paths,
                             std::string_view label,
                             bool require_directory,
                             DoctorState& state,
                             std::ostream& output)
    {
      for (const auto& path : paths)
        check_project_path(project_directory, path, label, require_directory, state, output);
    }

    void check_runtime_files(const std::filesystem::path& project_directory,
                             const std::vector<RuntimeFile>& files,
                             DoctorState& state,
                             std::ostream& output)
    {
      for (const auto& file : files)
      {
        check_project_path(project_directory, file.source, "runtime file", false, state, output);

        if (!file.destination.empty() && !is_safe_project_path(file.destination))
        {
          report_error(output, state, "runtime destination must stay inside the release layout: "
            + file.destination.generic_string());
        }
      }
    }

    void check_target_paths(const std::filesystem::path& project_directory,
                            const RecipeTarget& target,
                            DoctorState& state,
                            std::ostream& output)
    {
      check_project_paths(project_directory, target.sources, "target source", false, state, output);
      check_project_paths(project_directory, target.public_headers, "target public header", false, state, output);
      check_project_paths(
        project_directory,
        target.include_directories,
        "target include directory",
        true,
        state,
        output
      );
      check_runtime_files(project_directory, target.runtime_files, state, output);
    }

    void count_dependencies(const std::vector<Dependency>& dependencies,
                            DoctorState& state)
    {
      for (const auto& dependency : dependencies)
      {
        if (!dependency.github.empty())
          ++state.github_dependencies;
        else if (!dependency.path.empty())
          ++state.local_dependencies;
        else if (!dependency.box.empty())
          ++state.box_dependencies;
      }
    }

    void check_dependencies(const std::filesystem::path& project_directory,
                            const std::vector<Dependency>& dependencies,
                            std::string_view label,
                            DoctorState& state,
                            std::ostream& output)
    {
      for (const auto& dependency : dependencies)
      {
        if (!dependency.path.empty()
            && !std::filesystem::exists(project_directory / dependency.path))
        {
          report_warning(output, state, std::string { label } + " dependency '"
            + dependency.name + "' path does not exist locally: "
            + dependency.path.generic_string());
        }

        if (!dependency.box.empty())
          check_project_path(project_directory, dependency.box, "box dependency", false, state, output);
      }
    }

  } // namespace

  int doctor_project(const std::filesystem::path& project_directory,
                     std::ostream& output,
                     std::ostream& error)
  {
    if (!std::filesystem::is_regular_file(project_directory / "forge.recipe.toml"))
      return doctor_unadopted_project(project_directory, output, error);

    Recipe recipe;

    if (!read_recipe(project_directory / "forge.recipe.toml", recipe, error))
      return 2;

    DoctorState state;
    const auto release_heading = release_notes_heading(recipe);

    output << "Checking " << recipe.name << " " << release_heading << '\n';

    check_project_paths(project_directory, recipe.sources, "source", false, state, output);
    check_project_paths(project_directory, recipe.public_headers, "public header", false, state, output);
    check_project_paths(
      project_directory,
      recipe.include_directories,
      "include directory",
      true,
      state,
      output
    );
    check_runtime_files(project_directory, recipe.runtime_files, state, output);
    check_project_paths(project_directory, recipe.release_files, "release file", false, state, output);
    check_project_path(project_directory, recipe.version_header_path, "version header", false, state, output);
    check_project_path(project_directory, recipe.release_readme.linux_path, "Linux release README", false, state, output);
    check_project_path(project_directory, recipe.release_readme.macos_path, "macOS release README", false, state, output);
    check_project_path(project_directory, recipe.release_readme.windows_path, "Windows release README", false, state, output);

    for (const auto& target : recipe.targets)
      check_target_paths(project_directory, target, state, output);

    for (const auto& target : recipe.internal_targets)
      check_target_paths(project_directory, target, state, output);

    count_dependencies(recipe.dependencies, state);
    check_dependencies(project_directory, recipe.dependencies, "default", state, output);

    for (const auto& [profile, dependencies] : recipe.dependency_profiles)
    {
      count_dependencies(dependencies, state);
      check_dependencies(project_directory, dependencies, "profile '" + profile + "'", state, output);
    }

    if (!std::filesystem::is_regular_file(project_directory / "RELEASE_NOTES.md"))
    {
      report_warning(output, state, "RELEASE_NOTES.md is missing");
    }
    else if (!release_notes_contain_heading(project_directory / "RELEASE_NOTES.md", release_heading))
    {
      report_warning(output, state, "RELEASE_NOTES.md has no section for " + release_heading);
    }

    output
      << "Checked " << state.checked_paths << " project paths\n"
      << "Dependencies: " << state.github_dependencies << " GitHub, "
      << state.local_dependencies << " local, "
      << state.box_dependencies << " box\n";

    if (state.errors == 0 && state.warnings == 0)
    {
      output << "Forge doctor found no problems\n";
      return 0;
    }

    output
      << "Forge doctor found " << state.errors << " errors and "
      << state.warnings << " warnings\n";

    return state.errors == 0 ? 0 : 2;
  }

} // namespace forge
