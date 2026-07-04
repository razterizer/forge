#include "doctor.h"
#include "file_support.h"
#include "project_scan.h"
#include "recipe.h"

#include <filesystem>
#include <fstream>
#include <map>
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

    std::string format_runtime_file(const RuntimeFile& file)
    {
      if (file.source == file.destination)
        return file.source.generic_string();

      return file.source.generic_string() + " -> " + file.destination.generic_string();
    }

    bool path_contains(const std::filesystem::path& directory,
                       const std::filesystem::path& path)
    {
      const auto relative = path.lexically_relative(directory);
      return !relative.empty()
        && relative != "."
        && !relative.is_absolute()
        && *relative.begin() != "..";
    }

    bool runtime_file_covers(const std::filesystem::path& project_directory,
                             const RuntimeFile& declared,
                             const RuntimeFile& inferred)
    {
      if (declared.source == inferred.source || declared.destination == inferred.destination)
        return true;

      return std::filesystem::is_directory(project_directory / declared.source)
        && path_contains(declared.source, inferred.source);
    }

    bool runtime_file_is_declared(const std::filesystem::path& project_directory,
                                  const std::vector<RuntimeFile>& declared,
                                  const RuntimeFile& inferred)
    {
      for (const auto& runtime_file : declared)
      {
        if (runtime_file_covers(project_directory, runtime_file, inferred))
          return true;
      }

      return false;
    }

    void append_runtime_files(std::vector<RuntimeFile>& runtime_files,
                              const std::vector<RuntimeFile>& additional)
    {
      runtime_files.insert(runtime_files.end(), additional.begin(), additional.end());
    }

    std::vector<std::string> generic_strings(const std::vector<std::filesystem::path>& paths)
    {
      std::vector<std::string> strings;
      strings.reserve(paths.size());

      for (const auto& path : paths)
        strings.push_back(path.generic_string());

      return strings;
    }

    std::vector<RuntimeFile> declared_runtime_files(const Recipe& recipe)
    {
      std::vector<RuntimeFile> runtime_files = recipe.runtime_files;

      for (const auto& target : recipe.targets)
        append_runtime_files(runtime_files, target.runtime_files);

      for (const auto& target : recipe.internal_targets)
        append_runtime_files(runtime_files, target.runtime_files);

      return runtime_files;
    }

    std::vector<RuntimeFile> inferred_runtime_files(
      const std::filesystem::path& project_directory,
      const Recipe& recipe,
      const std::vector<std::string>& headers)
    {
      std::vector<RuntimeFile> runtime_files =
        infer_runtime_files(project_directory, generic_strings(recipe.sources), headers);

      for (const auto& target : recipe.targets)
      {
        const auto inferred =
          infer_runtime_files(project_directory, generic_strings(target.sources), headers);
        append_runtime_files(runtime_files, inferred);
      }

      for (const auto& target : recipe.internal_targets)
      {
        const auto inferred =
          infer_runtime_files(project_directory, generic_strings(target.sources), headers);
        append_runtime_files(runtime_files, inferred);
      }

      return runtime_files;
    }

    void report_inferred_runtime_files(const std::vector<RuntimeFile>& runtime_files,
                                       std::ostream& output)
    {
      output << "Detected " << runtime_files.size() << " inferred runtime asset"
             << (runtime_files.size() == 1 ? "" : "s") << '\n';

      for (const auto& runtime_file : runtime_files)
        output << "  " << format_runtime_file(runtime_file) << '\n';
    }

    bool dependency_is_declared(const Recipe& recipe,
                                std::string_view name,
                                std::string_view path_or_repository)
    {
      const auto declared = [name, path_or_repository](const Dependency& dependency)
      {
        return dependency.name == name
          || dependency.path.generic_string() == path_or_repository
          || dependency.github == path_or_repository
          || dependency.package == path_or_repository;
      };

      for (const auto& dependency : recipe.dependencies)
      {
        if (declared(dependency))
          return true;
      }

      for (const auto& [profile, dependencies] : recipe.dependency_profiles)
      {
        for (const auto& dependency : dependencies)
        {
          if (declared(dependency))
            return true;
        }
      }

      return false;
    }

    std::string repository_url(std::string_view repository)
    {
      return "https://github.com/" + std::string { repository };
    }

    std::string repository_package(std::string_view repository)
    {
      const auto slash = repository.find('/');
      return slash == std::string_view::npos
        ? std::string { repository }
        : std::string { repository.substr(slash + 1) };
    }

    std::string cbox_release_pattern(std::string_view repository,
                                     std::string_view package,
                                     std::string_view version)
    {
      const auto resolved_version = version.empty() ? std::string { "<version>" } : std::string { version };
      return repository_url(repository)
        + "/releases/download/release-" + resolved_version
        + "/" + std::string { package } + "-" + resolved_version + "-<target>.cbox";
    }

    void report_dependency_suggestions(const std::filesystem::path& project_directory,
                                       const ProjectScan& scan,
                                       const Recipe* recipe,
                                       const DoctorOptions& options,
                                       const ProcessRunner& process_runner,
                                       DoctorState& state,
                                       std::ostream& output,
                                       std::ostream& error)
    {
      auto unresolved = unresolved_includes(project_directory, scan.sources, scan.headers);

      output << "Detected " << unresolved.size() << " unresolved dependency include"
             << (unresolved.size() == 1 ? "" : "s") << '\n';

      for (const auto& [include, source] : unresolved)
        output << "  " << include << " from " << source << '\n';

      auto local_suggestions = infer_sibling_dependencies(project_directory, unresolved);

      output << "Suggested " << local_suggestions.size() << " local dependenc"
             << (local_suggestions.size() == 1 ? "y" : "ies") << '\n';

      for (const auto& dependency : local_suggestions)
      {
        output << "  " << dependency.name << " at " << dependency.path << '\n';

        if (!dependency.github.empty())
        {
          const auto package = repository_package(dependency.github);
          output << "    cbox: "
                 << cbox_release_pattern(dependency.github, package, dependency.version)
                 << '\n'
                 << "    source: " << repository_url(dependency.github) << '\n';
        }

        if (recipe && !dependency_is_declared(*recipe, dependency.name, dependency.path))
        {
          report_warning(output, state, "dependency appears to be local but is not declared: "
            + dependency.name + " at " + dependency.path);
        }
      }

      const auto github = github_suggestions(project_directory, unresolved);

      output << "Suggested " << github.size() << " GitHub dependenc"
             << (github.size() == 1 ? "y" : "ies") << '\n';

      for (const auto& [repository, includes] : github)
      {
        output << "  " << repository << " for ";

        for (std::size_t index = 0; index < includes.size(); ++index)
        {
          if (index != 0)
            output << ", ";

          output << includes[index];
        }

        output << '\n';

        const auto name = repository_package(repository);
        output << "    cbox: " << cbox_release_pattern(repository, name, {}) << '\n'
               << "    source: " << repository_url(repository) << '\n';

        if (recipe && !dependency_is_declared(*recipe, name, repository))
        {
          report_warning(output, state, "dependency may be available on GitHub but is not declared: "
            + repository);
        }
      }

      if (!options.search_github || unresolved.empty())
        return;

      const auto searched = github_search_candidates(
        project_directory,
        unresolved,
        process_runner,
        error
      );

      output << "Found " << searched.size() << " GitHub search candidate"
             << (searched.size() == 1 ? "" : "s") << '\n';

      for (const auto& [repository, includes] : searched)
      {
        output << "  " << repository << " for ";

        for (std::size_t index = 0; index < includes.size(); ++index)
        {
          if (index != 0)
            output << ", ";

          output << includes[index];
        }

        output << '\n'
               << "    source: " << repository_url(repository) << '\n';
      }
    }

    int doctor_unadopted_project(const std::filesystem::path& project_directory,
                                 const DoctorOptions& options,
                                 const ProcessRunner& process_runner,
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

      report_inferred_runtime_files(
        infer_runtime_files(project_directory, scan.sources, scan.headers),
        output
      );
      report_dependency_suggestions(
        project_directory,
        scan,
        nullptr,
        options,
        process_runner,
        state,
        output,
        error
      );

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
    return doctor_project(project_directory, DoctorOptions {}, run_process, output, error);
  }

  int doctor_project(const std::filesystem::path& project_directory,
                     const DoctorOptions& options,
                     const ProcessRunner& process_runner,
                     std::ostream& output,
                     std::ostream& error)
  {
    if (!std::filesystem::is_regular_file(project_directory / "forge.recipe.toml"))
      return doctor_unadopted_project(project_directory, options, process_runner, output, error);

    Recipe recipe;

    if (!read_recipe(project_directory / "forge.recipe.toml", recipe, error))
      return 2;

    DoctorState state;
    ProjectScan scan;
    const auto release_heading = release_notes_heading(recipe);

    if (!scan_project(project_directory, scan, error))
      return 2;

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

    const auto declared_runtime = declared_runtime_files(recipe);
    const auto inferred_runtime = inferred_runtime_files(project_directory, recipe, scan.headers);
    report_inferred_runtime_files(inferred_runtime, output);

    for (const auto& runtime_file : inferred_runtime)
    {
      if (!runtime_file_is_declared(project_directory, declared_runtime, runtime_file))
      {
        report_warning(output, state, "runtime asset appears to be used but is not declared: "
          + format_runtime_file(runtime_file));
      }
    }

    report_dependency_suggestions(
      project_directory,
      scan,
      &recipe,
      options,
      process_runner,
      state,
      output,
      error
    );

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
