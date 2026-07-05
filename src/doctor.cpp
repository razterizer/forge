#include "doctor.h"
#include "file_support.h"
#include "project_scan.h"
#include "recipe.h"
#include "runtime_assets.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <optional>
#include <ostream>
#include <ranges>
#include <regex>
#include <sstream>
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

    std::string display_project_path(const std::filesystem::path& project_directory,
                                     const std::filesystem::path& path)
    {
      std::error_code filesystem_error;
      const auto relative = std::filesystem::relative(path, project_directory, filesystem_error);

      if (!filesystem_error && !relative.empty() && *relative.begin() != "..")
        return relative.generic_string();

      return path.generic_string();
    }

    void append_include_directories(std::vector<std::string>& include_directories,
                                    const std::vector<std::filesystem::path>& paths)
    {
      for (const auto& path : paths)
        include_directories.push_back(path.generic_string());
    }

    std::vector<std::string> doctor_include_directories(
      const std::filesystem::path& project_directory,
      const ProjectScan& scan,
      const Recipe* recipe)
    {
      auto include_directories =
        infer_include_directories(project_directory, scan.sources, scan.headers);

      if (recipe)
      {
        append_include_directories(include_directories, recipe->include_directories);
        append_include_directories(include_directories, recipe->macos_system_include_directories);
        append_include_directories(include_directories, recipe->linux_system_include_directories);
        append_include_directories(include_directories, recipe->windows_system_include_directories);

        for (const auto& target : recipe->targets)
        {
          append_include_directories(include_directories, target.include_directories);
          append_include_directories(include_directories, target.macos_system_include_directories);
          append_include_directories(include_directories, target.linux_system_include_directories);
          append_include_directories(include_directories, target.windows_system_include_directories);
        }

        for (const auto& target : recipe->internal_targets)
        {
          append_include_directories(include_directories, target.include_directories);
          append_include_directories(include_directories, target.macos_system_include_directories);
          append_include_directories(include_directories, target.linux_system_include_directories);
          append_include_directories(include_directories, target.windows_system_include_directories);
        }

        for (const auto& profile : recipe->build_profiles | std::views::values)
        {
          append_include_directories(include_directories, profile.include_directories);
          append_include_directories(include_directories, profile.macos_system_include_directories);
          append_include_directories(include_directories, profile.linux_system_include_directories);
          append_include_directories(include_directories, profile.windows_system_include_directories);
        }
      }

      std::ranges::sort(include_directories);
      include_directories.erase(
        std::unique(include_directories.begin(), include_directories.end()),
        include_directories.end()
      );
      return include_directories;
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

    std::string plural(std::size_t count,
                       std::string_view singular,
                       std::string_view plural)
    {
      return std::string { count == 1 ? singular : plural };
    }

    std::string runtime_asset_action(std::string_view type)
    {
      return type == "executable" ? "stages" : "exports";
    }

    struct RuntimeFileScope
    {
      std::string label;
      std::string type;
      std::vector<RuntimeFile> files;
    };

    void report_declared_runtime_files(const std::filesystem::path& project_directory,
                                       const Recipe& recipe,
                                       DoctorState& state,
                                       std::ostream& output)
    {
      std::vector<RuntimeFileScope> scopes {
        { "project", recipe.type, recipe.runtime_files }
      };
      std::size_t entry_count = recipe.runtime_files.size();

      for (const auto& target : recipe.targets)
      {
        scopes.push_back({ "target '" + target.name + "'", target.type, target.runtime_files });
        entry_count += target.runtime_files.size();
      }

      for (const auto& target : recipe.internal_targets)
      {
        scopes.push_back({ "internal target '" + target.name + "'", target.type, target.runtime_files });
        entry_count += target.runtime_files.size();
      }

      output << "Declared " << entry_count << " runtime asset "
             << plural(entry_count, "entry", "entries") << '\n';

      for (const auto& scope : scopes)
      {
        if (scope.files.empty())
          continue;

        std::vector<RuntimeAsset> assets;
        std::ostringstream collect_error;
        const auto collected =
          collect_runtime_assets(project_directory, scope.files, assets, collect_error);

        output << "  " << scope.label << ' ' << runtime_asset_action(scope.type) << ' ';

        if (collected)
        {
          output << assets.size() << " runtime "
                 << plural(assets.size(), "asset", "assets")
                 << " from " << scope.files.size() << ' '
                 << plural(scope.files.size(), "entry", "entries") << '\n';
        }
        else
        {
          output << scope.files.size() << " invalid runtime "
                 << plural(scope.files.size(), "entry", "entries") << '\n';

          auto message = std::string { trim(collect_error.str()) };

          if (message.starts_with("forge: "))
            message.erase(0, std::string_view { "forge: " }.size());

          report_error(output, state, "runtime asset declaration is invalid: " + message);
        }

        for (const auto& runtime_file : scope.files)
          output << "    " << format_runtime_file(runtime_file) << '\n';
      }
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

    struct CboxReleaseHint
    {
      std::string tag_version;
      std::string package_version;
      std::string suffix;
    };

    bool is_target_suffix(std::string_view suffix)
    {
      return suffix == "linux-x86_64"
        || suffix == "macos-arm64"
        || suffix == "windows-x86_64";
    }

    std::string cbox_release_pattern(std::string_view repository,
                                     std::string_view package,
                                     const CboxReleaseHint& release)
    {
      const auto suffix =
        release.suffix == "ho" || !is_target_suffix(release.suffix)
          ? release.suffix
          : std::string { "<target>" };

      return repository_url(repository)
        + "/releases/download/release-" + release.tag_version
        + "/" + std::string { package } + "-" + release.package_version
        + "-" + suffix + ".cbox";
    }

    bool consume_digits(std::string_view text, std::size_t& position)
    {
      const auto start = position;

      while (position < text.size() && text[position] >= '0' && text[position] <= '9')
        ++position;

      return position != start;
    }

    bool consume_character(std::string_view text,
                           std::size_t& position,
                           char expected)
    {
      if (position >= text.size() || text[position] != expected)
        return false;

      ++position;
      return true;
    }

    std::optional<CboxReleaseHint> parse_cbox_asset_name(std::string_view tag_version,
                                                         std::string_view package,
                                                         std::string_view asset)
    {
      const auto prefix = std::string { package } + "-";
      const auto extension = std::string_view { ".cbox" };

      if (!asset.starts_with(prefix) || !asset.ends_with(extension))
        return std::nullopt;

      const auto details =
        asset.substr(prefix.size(), asset.size() - prefix.size() - extension.size());
      std::size_t position = 0;

      if (!consume_digits(details, position)
          || !consume_character(details, position, '.')
          || !consume_digits(details, position)
          || !consume_character(details, position, '.')
          || !consume_digits(details, position))
      {
        return std::nullopt;
      }

      if (details.substr(position).starts_with("+build."))
      {
        position += std::string_view { "+build." }.size();

        if (!consume_digits(details, position))
          return std::nullopt;
      }

      const auto version_end = position;

      if (!consume_character(details, position, '-') || position >= details.size())
        return std::nullopt;

      return CboxReleaseHint {
        std::string { tag_version },
        std::string { details.substr(0, version_end) },
        std::string { details.substr(position) }
      };
    }

    bool download_latest_github_release(const std::filesystem::path& project_directory,
                                        std::string_view repository,
                                        const ProcessRunner& process_runner,
                                        std::filesystem::path& destination,
                                        std::ostream& error)
    {
      const auto cache_directory =
        project_directory / ".forge" / "cache" / "github-release"
          / std::filesystem::path { repository };
      std::error_code filesystem_error;
      std::filesystem::create_directories(cache_directory, filesystem_error);

      if (filesystem_error)
        return false;

      destination = cache_directory / "latest.json";
      auto status_path = destination;
      status_path += ".status";
      const auto script = cache_directory / "latest-release.cmake";
      std::ofstream file { script };

      if (!file)
        return false;

      file
        << "file(DOWNLOAD \"${URL}\" \"${DESTINATION}.tmp\" STATUS status TLS_VERIFY ON)\n"
        << "list(GET status 0 code)\n"
        << "file(WRITE \"${STATUS_FILE}\" \"${code}\")\n"
        << "if(NOT code EQUAL 0)\n"
        << "  file(REMOVE \"${DESTINATION}.tmp\")\n"
        << "  return()\n"
        << "endif()\n"
        << "file(REMOVE \"${DESTINATION}\")\n"
        << "file(RENAME \"${DESTINATION}.tmp\" \"${DESTINATION}\")\n";
      file.close();

      std::filesystem::remove(status_path, filesystem_error);
      const auto result = process_runner(
        {
          "cmake",
          "-DURL=https://api.github.com/repos/" + std::string { repository } + "/releases/latest",
          "-DDESTINATION=" + destination.generic_string(),
          "-DSTATUS_FILE=" + status_path.generic_string(),
          "-P",
          script.string()
        },
        project_directory,
        error
      );

      if (result != 0)
        return false;

      std::ifstream status_file { status_path };
      int status = 0;

      if (!(status_file >> status))
        return false;

      std::filesystem::remove(status_path, filesystem_error);
      return status == 0;
    }

    std::optional<CboxReleaseHint> latest_cbox_release(std::string_view json,
                                                       std::string_view package)
    {
      std::cmatch tag_match;
      const std::regex tag_pattern {
        R"json("tag_name"[[:space:]]*:[[:space:]]*"release-([^"]+)")json"
      };
      const auto json_begin = json.data();
      const auto json_end = json.data() + json.size();

      if (!std::regex_search(json_begin, json_end, tag_match, tag_pattern))
        return std::nullopt;

      const auto tag_version = tag_match[1].str();
      const std::regex asset_pattern {
        R"json("name"[[:space:]]*:[[:space:]]*"([^"]+\.cbox)")json"
      };
      const auto begin = std::cregex_iterator { json_begin, json_end, asset_pattern };
      const auto end = std::cregex_iterator {};

      for (auto match = begin; match != end; ++match)
      {
        const auto asset = (*match)[1].str();
        const auto release =
          parse_cbox_asset_name(tag_version, package, asset);

        if (release)
          return release;
      }

      return std::nullopt;
    }

    std::optional<CboxReleaseHint> latest_cbox_release(
      const std::filesystem::path& project_directory,
      std::string_view repository,
      std::string_view package,
      const ProcessRunner& process_runner,
      std::ostream& error)
    {
      std::filesystem::path response_path;

      if (!download_latest_github_release(
        project_directory,
        repository,
        process_runner,
        response_path,
        error
      ))
      {
        return std::nullopt;
      }

      std::ifstream response { response_path };
      return latest_cbox_release(
        std::string {
          std::istreambuf_iterator<char> { response },
          std::istreambuf_iterator<char> {}
        },
        package
      );
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

      auto local_suggestions =
        infer_sibling_dependencies(project_directory, unresolved, options.local_search);
      const auto include_evidence = infer_include_dependency_evidence(
        project_directory,
        unresolved,
        doctor_include_directories(project_directory, scan, recipe),
        scan.sources,
        scan.headers
      );

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

      output << "Resolved " << include_evidence.size()
             << " dependency include"
             << (include_evidence.size() == 1 ? "" : "s")
             << " through include directories\n";

      for (const auto& evidence : include_evidence)
      {
        output << "  " << evidence.include << " from " << evidence.source
               << " -> " << display_project_path(project_directory, evidence.header) << '\n'
               << "    include dir: " << evidence.include_directory.generic_string()
               << '\n';

        if (!evidence.root.empty())
          output << "    root: " << display_project_path(project_directory, evidence.root) << '\n';

        if (evidence.forge_recipe)
        {
          output << "    recipe: " << evidence.name;

          if (!evidence.version.empty())
            output << ' ' << evidence.version;

          output << '\n';
        }

        if (!evidence.github.empty())
          output << "    source: " << repository_url(evidence.github) << '\n';
      }

      const auto github = github_suggestions(project_directory, unresolved);
      std::map<std::string, std::optional<CboxReleaseHint>> latest_cbox_releases;
      const auto resolved_cbox_release =
        [&](std::string_view repository, std::string_view package)
          -> std::optional<CboxReleaseHint>
        {
          if (!options.search_github)
            return std::nullopt;

          const auto key = std::string { repository };
          const auto existing = latest_cbox_releases.find(key);

          if (existing != latest_cbox_releases.end())
            return existing->second;

          auto release = latest_cbox_release(
            project_directory,
            repository,
            package,
            process_runner,
            error
          );
          latest_cbox_releases.emplace(key, release);
          return release;
        };

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
        const auto release = resolved_cbox_release(repository, name);
        output << "    cbox: "
               << (release
                 ? cbox_release_pattern(repository, name, *release)
                 : cbox_release_pattern(repository, name, std::string_view {}))
               << '\n'
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
    report_declared_runtime_files(project_directory, recipe, state, output);
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
