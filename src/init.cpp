#include "init.h"

#include "init_cmake.h"
#include "init_support.h"
#include "init_xcode.h"
#include "init_visual_studio.h"

#include "file_support.h"
#include "github.h"
#include "project_scan.h"
#include "recipe.h"
#include "versioning.h"
#include "workspace.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <fstream>
#include <iterator>
#include <map>
#include <optional>
#include <regex>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace forge
{
  static int adopt_project_impl(const std::filesystem::path& project_directory,
                                const AdoptOptions& options,
                                const ProcessRunner& process_runner,
                                bool show_progress,
                                std::ostream& output,
                                std::ostream& error);

  namespace
  {

    std::string escape_toml_string(std::string_view value)
    {
      std::string escaped;
      escaped.reserve(value.size());

      for (const char character : value)
      {
        if (character == '\\' || character == '"')
          escaped += '\\';

        escaped += character;
      }

      return escaped;
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

    std::string_view trim(std::string_view value)
    {
      const auto first = value.find_first_not_of(" \t\r");

      if (first == std::string_view::npos)
        return {};

      return value.substr(first, value.find_last_not_of(" \t\r") - first + 1);
    }

    struct ReleaseNotesInitialVersion
    {
      InitialVersion version;
      std::optional<std::string> build_number_format;
    };

    std::optional<ReleaseNotesInitialVersion> parse_release_notes_version_heading(
      std::string_view line
    )
    {
      line = trim(line);

      if (!line.starts_with("##") || line.starts_with("###"))
        return std::nullopt;

      auto heading = trim(line.substr(2));
      auto build_number_format = std::optional<std::string> {};
      auto parsed = parse_initial_version(heading);

      if (!parsed)
      {
        constexpr std::string_view semver_build = "+build.";
        const auto build_separator = heading.find(semver_build);

        if (build_separator != std::string_view::npos)
        {
          auto normalized = std::string { heading.substr(0, build_separator) };
          normalized += '.';
          normalized += heading.substr(build_separator + semver_build.size());
          parsed = parse_initial_version(normalized);

          if (parsed && parsed->build_number)
            build_number_format = "semver";
        }
      }
      else if (parsed->build_number)
      {
        build_number_format = "dotted";
      }

      if (!parsed)
        return std::nullopt;

      return ReleaseNotesInitialVersion { *parsed, build_number_format };
    }

    bool infer_release_notes_initial_version(
      const std::filesystem::path& project_directory,
      std::optional<ReleaseNotesInitialVersion>& version,
      std::ostream& error
    )
    {
      const auto notes_path = project_directory / "RELEASE_NOTES.md";

      if (!std::filesystem::exists(notes_path))
        return true;

      std::ifstream file { notes_path };

      if (!file)
      {
        error << "forge: could not read '" << notes_path.string() << "'\n";
        return false;
      }

      std::string line;

      while (std::getline(file, line))
      {
        if (const auto parsed = parse_release_notes_version_heading(line))
        {
          version = *parsed;
          return true;
        }
      }

      return true;
    }

    void report_progress(std::ostream& output,
                         std::size_t current,
                         std::size_t total,
                         std::string_view description)
    {
      output << '[' << current << '/' << total << "] " << description << '\n' << std::flush;
    }

    void report_subprogress(std::ostream& output,
                            std::size_t current,
                            std::size_t total,
                            std::string_view description)
    {
      output << "      [" << current << '/' << total << "] " << description << '\n' << std::flush;
    }

    bool is_cmake_generated_xcode_project(const std::filesystem::path& path)
    {
      std::ifstream file { path / "project.pbxproj" };
      const std::string contents {
        std::istreambuf_iterator<char> { file },
        std::istreambuf_iterator<char> {}
      };
      return contents.find("CMakeFiles") != std::string::npos
        || contents.find("ZERO_CHECK") != std::string::npos
        || contents.find("CMAKE_") != std::string::npos;
    }

    std::vector<std::filesystem::path> files_with_extension(
      const std::filesystem::path& directory,
      std::string_view extension)
    {
      std::vector<std::filesystem::path> paths;
      std::error_code filesystem_error;

      for (const auto& entry : std::filesystem::directory_iterator { directory, filesystem_error })
      {
        if (!filesystem_error
            && entry.is_regular_file(filesystem_error)
            && entry.path().extension() == extension)
        {
          paths.push_back(entry.path());
        }
      }

      std::ranges::sort(paths);
      return paths;
    }

    std::vector<std::filesystem::path> directories_with_extension(
      const std::filesystem::path& directory,
      std::string_view extension)
    {
      std::vector<std::filesystem::path> paths;
      std::error_code filesystem_error;

      for (const auto& entry : std::filesystem::directory_iterator { directory, filesystem_error })
      {
        if (!filesystem_error
            && entry.is_directory(filesystem_error)
            && entry.path().extension() == extension)
        {
          paths.push_back(entry.path());
        }
      }

      std::ranges::sort(paths);
      return paths;
    }

    void merge_project_metadata(VisualStudioProject& project,
                                const VisualStudioProject& additional)
    {
      if (project.name.empty())
        project.name = additional.name;

      if (project.version.empty())
        project.version = additional.version;

      if (project.type.empty())
        project.type = additional.type;

      project.python_extension = project.python_extension || additional.python_extension;

      project.cpp_standard = std::max(project.cpp_standard, additional.cpp_standard);
      project.c_standard = std::max(project.c_standard, additional.c_standard);

      project.include_directories.insert(
        project.include_directories.end(),
        additional.include_directories.begin(),
        additional.include_directories.end()
      );
      project.definitions.insert(
        project.definitions.end(),
        additional.definitions.begin(),
        additional.definitions.end()
      );
      project.macos_frameworks.insert(
        project.macos_frameworks.end(),
        additional.macos_frameworks.begin(),
        additional.macos_frameworks.end()
      );
      project.macos_libraries.insert(
        project.macos_libraries.end(),
        additional.macos_libraries.begin(),
        additional.macos_libraries.end()
      );
      project.macos_brew_packages.insert(
        project.macos_brew_packages.end(),
        additional.macos_brew_packages.begin(),
        additional.macos_brew_packages.end()
      );
      project.linux_libraries.insert(
        project.linux_libraries.end(),
        additional.linux_libraries.begin(),
        additional.linux_libraries.end()
      );
      project.linux_apt_packages.insert(
        project.linux_apt_packages.end(),
        additional.linux_apt_packages.begin(),
        additional.linux_apt_packages.end()
      );
      project.windows_libraries.insert(
        project.windows_libraries.end(),
        additional.windows_libraries.begin(),
        additional.windows_libraries.end()
      );
      project.unresolved_properties.insert(
        project.unresolved_properties.end(),
        additional.unresolved_properties.begin(),
        additional.unresolved_properties.end()
      );

      for (const auto& [name, profile] : additional.profiles)
        project.profiles.try_emplace(name, profile);

      for (auto& [name, profile] : project.profiles)
      {
        profile.cpp_standard = std::max(profile.cpp_standard, project.cpp_standard);
        profile.c_standard = std::max(profile.c_standard, project.c_standard);
      }

      for (auto* values : {
        &project.include_directories,
        &project.definitions,
        &project.macos_frameworks,
        &project.macos_libraries,
        &project.linux_libraries,
        &project.windows_libraries,
        &project.unresolved_properties
      })
      {
        std::ranges::sort(*values);
        values->erase(std::unique(values->begin(), values->end()), values->end());
      }
    }

    struct VersionHeaderCandidate
    {
      std::string path;
      std::string prefix;
    };

    std::vector<VersionHeaderCandidate> infer_version_headers(
      const std::filesystem::path& project_directory,
      const std::vector<std::string>& headers)
    {
      static const std::regex definition {
        R"regex(^\s*#\s*define\s+([A-Z_][A-Z0-9_]*)_VERSION_(STR|MAJOR|MINOR|PATCH|BUILD)\b)regex"
      };
      std::vector<VersionHeaderCandidate> candidates;

      for (const auto& header : headers)
      {
        std::ifstream file { project_directory / header };
        std::map<std::string, std::set<std::string>> definitions;
        std::string line;

        while (std::getline(file, line))
        {
          std::smatch match;

          if (std::regex_search(line, match, definition))
            definitions[match[1].str()].insert(match[2].str());
        }

        for (const auto& [prefix, suffixes] : definitions)
        {
          if (suffixes.size() == 5)
          {
            candidates.push_back({ header, prefix });
          }
        }
      }

      std::ranges::sort(candidates, {}, &VersionHeaderCandidate::path);
      return candidates;
    }

    std::optional<std::string> infer_packed_macro_version(
      const std::filesystem::path& project_directory,
      const std::vector<std::string>& headers,
      std::string_view project_name)
    {
      static const std::regex definition {
        R"regex(^\s*#\s*define\s+([A-Z_][A-Z0-9_]*)_VERSION\s+([0-9]+)\b)regex"
      };
      const auto expected_prefix = version_macro_prefix(project_name);

      for (const auto& header : headers)
      {
        std::ifstream file { project_directory / header };
        std::string line;

        while (std::getline(file, line))
        {
          std::smatch match;

          if (!std::regex_search(line, match, definition)
              || match[1].str() != expected_prefix)
          {
            continue;
          }

          int packed = 0;
          const auto value = match[2].str();
          const auto parsed = std::from_chars(value.data(), value.data() + value.size(), packed);

          if (parsed.ec != std::errc {}
              || parsed.ptr != value.data() + value.size()
              || packed < 10000)
          {
            continue;
          }

          return std::to_string(packed / 10000) + "."
            + std::to_string((packed / 100) % 100) + "."
            + std::to_string(packed % 100);
        }
      }

      return std::nullopt;
    }

    std::optional<std::string> infer_component_macro_version(
      const std::filesystem::path& project_directory,
      const std::vector<std::string>& headers,
      std::string_view project_name)
    {
      static const std::regex definition {
        R"regex(^\s*#\s*define\s+([A-Z_][A-Z0-9_]*)_VER_(MAJOR|MINOR|PATCH)\s+([0-9]+)\b)regex"
      };
      const auto expected_prefix = version_macro_prefix(project_name);

      for (const auto& header : headers)
      {
        std::ifstream file { project_directory / header };
        std::map<std::string, std::string> components;
        std::string line;

        while (std::getline(file, line))
        {
          std::smatch match;

          if (std::regex_search(line, match, definition)
              && match[1].str() == expected_prefix)
          {
            components[match[2].str()] = match[3].str();
          }
        }

        if (components.contains("MAJOR")
            && components.contains("MINOR")
            && components.contains("PATCH"))
        {
          return components["MAJOR"] + "." + components["MINOR"] + "." + components["PATCH"];
        }
      }

      return std::nullopt;
    }

    std::vector<SiblingDependency> visual_studio_dependencies(
      const std::filesystem::path& project_directory,
      const VisualStudioProject& project)
    {
      std::vector<SiblingDependency> dependencies;

      for (const auto& reference : project.references)
      {
        std::ostringstream ignored_error;
        const auto referenced = read_visual_studio_project(reference, ignored_error);

        if (!referenced)
          continue;

        std::error_code filesystem_error;
        const auto relative = std::filesystem::relative(
          reference.parent_path(),
          project_directory,
          filesystem_error
        );

        if (!filesystem_error)
        {
          dependencies.push_back({
            referenced->name,
            relative.generic_string(),
            referenced->version,
            {}
          });
        }
      }

      std::ranges::sort(
        dependencies,
        {},
        [](const SiblingDependency& dependency)
        {
          return dependency.name;
        }
      );
      dependencies.erase(
        std::unique(
          dependencies.begin(),
          dependencies.end(),
          [](const SiblingDependency& left, const SiblingDependency& right)
          {
            return left.name == right.name;
          }
        ),
        dependencies.end()
      );
      return dependencies;
    }

    struct GitHubDependency
    {
      std::string name;
      std::string repository;
      std::string git;
      std::string commit;
    };


    std::optional<std::string> git_head(const std::filesystem::path& repository)
    {
      std::ifstream head_file { repository / ".git" / "HEAD" };
      std::string head;
      std::getline(head_file, head);

      if (!head.starts_with("ref: "))
      {
        return head.empty() ? std::nullopt : std::optional<std::string> { head };
      }

      const auto reference = head.substr(5);
      std::ifstream reference_file { repository / ".git" / reference };
      std::string commit;
      std::getline(reference_file, commit);

      if (!commit.empty())
        return commit;

      std::ifstream packed { repository / ".git" / "packed-refs" };
      std::string line;

      while (std::getline(packed, line))
      {
        const auto separator = line.find(' ');

        if (separator != std::string::npos && line.substr(separator + 1) == reference)
          return line.substr(0, separator);
      }

      return std::nullopt;
    }

    bool is_exact_commit(std::string_view commit)
    {
      return
        (commit.size() == 40 || commit.size() == 64)
        && std::ranges::all_of(
          commit,
          [](unsigned char character)
          {
            return std::isxdigit(character);
          }
        );
    }

    std::vector<GitHubDependency> resolve_github_dependencies(
      const std::filesystem::path& project_directory,
      const std::map<std::string, std::vector<std::string>>& suggestions,
      std::map<std::string, std::string>& unresolved,
      const ProcessRunner& process_runner,
      std::ostream& output)
    {
      std::map<std::string, GitHubDependency> dependencies;
      std::set<std::string> conflicting_names;
      const auto cache = project_directory / ".forge" / "adopt" / "github";
      std::error_code filesystem_error;
      std::filesystem::create_directories(cache, filesystem_error);

      for (const auto& [repository, includes] : suggestions)
      {
        const auto slash = repository.find('/');
        const auto name = repository.substr(slash + 1);
        const auto checkout = cache / name;
        std::filesystem::remove_all(checkout, filesystem_error);
        std::ostringstream clone_error;
        const auto git = "https://github.com/" + repository + ".git";

        if (process_runner({ "git", "clone", "--quiet", "--depth", "1", git, checkout.string() },
                           project_directory,
                           clone_error) != 0)
        {
          continue;
        }

        Recipe recipe;
        std::ostringstream recipe_error;

        if (!read_recipe(checkout / "forge.recipe.toml", recipe, recipe_error))
          continue;

        const auto verified = std::ranges::all_of(
          includes,
          [&recipe](const std::string& include)
          {
            return provides_include(recipe, include);
          }
        );
        const auto commit = git_head(checkout);

        if (!verified || !commit || !is_exact_commit(*commit))
          continue;

        const GitHubDependency dependency { name, repository, git, *commit };
        const auto existing = dependencies.find(dependency.name);

        if (existing != dependencies.end() && existing->second.repository != repository)
        {
          conflicting_names.insert(dependency.name);
          continue;
        }

        dependencies[dependency.name] = dependency;
      }

      std::vector<GitHubDependency> result;

      for (auto& [name, dependency] : dependencies)
      {
        if (!conflicting_names.contains(name))
        {
          for (const auto& include : suggestions.at(dependency.repository))
            unresolved.erase(include);

          output << "Pinned GitHub dependency " << dependency.repository
                 << " at " << dependency.commit << '\n';
          result.push_back(std::move(dependency));
        }
      }

      return result;
    }

    std::vector<std::vector<std::string>> infer_target_sources(
      const std::filesystem::path& project_directory,
      const std::vector<std::string>& sources,
      const std::vector<std::string>& headers,
      const std::vector<std::string>& entry_points)
    {
      std::map<std::string, std::set<std::string>> reachable;

      for (const auto& source : sources)
        reachable[source] = reachable_local_headers(project_directory, source, headers);

      std::vector<std::vector<std::string>> target_sources(entry_points.size());

      for (std::size_t target_index = 0; target_index < entry_points.size(); ++target_index)
        target_sources[target_index].push_back(entry_points[target_index]);

      for (const auto& source : sources)
      {
        if (std::binary_search(entry_points.begin(), entry_points.end(), source))
          continue;

        std::vector<std::size_t> owners;

        for (std::size_t target_index = 0; target_index < entry_points.size(); ++target_index)
        {
          const auto& target_headers = reachable.at(entry_points[target_index]);
          const auto& source_headers = reachable.at(source);

          if (std::ranges::any_of(
            source_headers,
            [&target_headers](const std::string& header)
            {
              return target_headers.contains(header);
            }
          ))
          {
            owners.push_back(target_index);
          }
        }

        if (owners.empty())
        {
          for (std::size_t target_index = 0; target_index < entry_points.size(); ++target_index)
            target_sources[target_index].push_back(source);
        }
        else
        {
          for (const auto owner : owners)
            target_sources[owner].push_back(source);
        }
      }

      for (auto& target : target_sources)
        std::ranges::sort(target);

      return target_sources;
    }

    void expand_public_header_closure(const std::filesystem::path& project_directory,
                                      std::vector<std::string>& public_headers,
                                      const std::vector<std::string>& headers)
    {
      std::set<std::string> exported_headers { public_headers.begin(), public_headers.end() };

      for (const auto& header : public_headers)
      {
        const auto reachable = reachable_local_headers(project_directory, header, headers);
        exported_headers.insert(reachable.begin(), reachable.end());
      }

      public_headers.assign(exported_headers.begin(), exported_headers.end());
    }

    void export_headers_below_public_include_roots(
      std::vector<std::string>& public_headers,
      const std::vector<std::string>& headers,
      const std::vector<std::string>& public_include_directories)
    {
      std::set<std::string> exported_headers { public_headers.begin(), public_headers.end() };

      for (const auto& header : headers)
      {
        const auto path = std::filesystem::path { header };
        const auto below_public_root = std::ranges::any_of(
          public_include_directories,
          [&path](const std::string& include_directory)
          {
            const auto relative = path.lexically_relative(include_directory);
            return !relative.empty()
              && relative != "."
              && !relative.is_absolute()
              && relative.begin()->string() != "..";
          }
        );

        if (below_public_root)
          exported_headers.insert(header);
      }

      public_headers.assign(exported_headers.begin(), exported_headers.end());
    }

    std::string format_sources(const std::vector<std::string>& sources)
    {
      if (sources.empty())
        return "[]";

      std::string formatted = "[";

      for (std::size_t index = 0; index < sources.size(); ++index)
      {
        if (index != 0)
          formatted += ", ";

        formatted += '"' + escape_toml_string(sources[index]) + '"';
      }

      formatted += ']';
      return formatted;
    }

    std::string format_system_links(const VisualStudioProject& project)
    {
      std::string result;

      for (const auto& [key, values] : {
        std::pair { std::string_view { "macos_frameworks" }, &project.macos_frameworks },
        std::pair { std::string_view { "macos_libraries" }, &project.macos_libraries },
        std::pair { std::string_view { "macos_brew_packages" }, &project.macos_brew_packages },
        std::pair { std::string_view { "linux_libraries" }, &project.linux_libraries },
        std::pair { std::string_view { "linux_apt_packages" }, &project.linux_apt_packages },
        std::pair { std::string_view { "windows_libraries" }, &project.windows_libraries }
      })
      {
        if (!values->empty())
        {
          result += std::string { key } + " = " + format_sources(*values) + "\n";
        }
      }

      return result;
    }

    std::string format_runtime_files(const std::vector<RuntimeFile>& runtime_files)
    {
      std::string formatted = "[";

      for (std::size_t index = 0; index < runtime_files.size(); ++index)
      {
        if (index != 0)
          formatted += ", ";

        const auto& runtime_file = runtime_files[index];

        if (runtime_file.source == runtime_file.destination)
          formatted += '"' + escape_toml_string(runtime_file.source.generic_string()) + '"';
        else
        {
          formatted += "{ source = \""
            + escape_toml_string(runtime_file.source.generic_string())
            + "\", destination = \""
            + escape_toml_string(runtime_file.destination.generic_string())
            + "\" }";
        }
      }

      formatted += ']';
      return formatted;
    }

    void merge_runtime_files(std::vector<RuntimeFile>& runtime_files,
                             const std::vector<RuntimeFile>& additional)
    {
      runtime_files.insert(runtime_files.end(), additional.begin(), additional.end());
      std::ranges::sort(
        runtime_files,
        {},
        [](const RuntimeFile& runtime_file)
        {
          return std::pair {
            runtime_file.source.generic_string(),
            runtime_file.destination.generic_string()
          };
        }
      );
      runtime_files.erase(
        std::unique(
          runtime_files.begin(),
          runtime_files.end(),
          [](const RuntimeFile& left, const RuntimeFile& right)
          {
            return left.source == right.source && left.destination == right.destination;
          }
        ),
        runtime_files.end()
      );
    }

    std::string target_name(const std::filesystem::path& source, std::size_t index)
    {
      auto name = source.stem().string();

      for (char& character : name)
      {
        if (!std::isalnum(static_cast<unsigned char>(character))
            && character != '-'
            && character != '_')
        {
          character = '-';
        }
      }

      return name.empty() ? "executable-" + std::to_string(index + 1) : name;
    }

    int adopt_solution(const std::filesystem::path& workspace_directory,
                       const std::filesystem::path& solution_path,
                       const AdoptOptions& options,
                       const ProcessRunner& process_runner,
                       std::ostream& output,
                       std::ostream& error)
    {
      const auto workspace_path = workspace_directory / "forge.workspace.toml";

      if (std::filesystem::exists(workspace_path))
      {
        error << "forge: '" << workspace_path.string() << "' already exists\n";
        return 2;
      }

      const auto projects = read_solution_projects(solution_path, error);

      if (projects.empty())
      {
        error << "forge: solution contains no C++ projects\n";
        return 2;
      }

      std::set<std::filesystem::path> directories;
      std::vector<std::string> relative_directories;
      const auto progress_total = projects.size() + 2;

      report_progress(output, 1, progress_total, "Reading Visual Studio solution");

      for (const auto& project : projects)
      {
        const auto directory = project.parent_path();
        const auto relative = directory.lexically_relative(workspace_directory);

        if (relative.empty()
            || relative == "."
            || relative.is_absolute()
            || *relative.begin() == "..")
        {
          error << "forge: solution projects must live in distinct subdirectories\n";
          return 2;
        }

        if (!directories.insert(directory).second)
        {
          error << "forge: multiple solution projects share directory '" << directory.string()
                << "'\n";
          return 2;
        }

        relative_directories.push_back(relative.generic_string());

        if (std::filesystem::exists(directory / "forge.recipe.toml"))
        {
          error << "forge: '" << (directory / "forge.recipe.toml").string() << "' already exists\n";
          return 2;
        }
      }

      for (std::size_t index = 0; index < projects.size(); ++index)
      {
        const auto& project = projects[index];
        report_progress(
          output,
          index + 2,
          progress_total,
          "Adopting project " + project.stem().string()
        );

        if (adopt_project_impl(
          project.parent_path(),
          options,
          process_runner,
          false,
          output,
          error
        ) != 0)
        {
          return 2;
        }
      }

      report_progress(output, progress_total, progress_total, "Writing workspace");

      const std::string workspace =
        "#:schema " + std::string { workspace_schema_url } + "\n"
        "\n"
        "[workspace]\n"
        "name = \"" + escape_toml_string(solution_path.stem().string()) + "\"\n"
        "projects = " + format_sources(relative_directories) + "\n";

      if (!write_file(workspace_path, workspace, error))
        return 2;

      output << "Created " << workspace_path.string() << '\n'
             << "Adopted " << projects.size() << " Visual Studio project";
      output << (projects.size() == 1 ? "\n" : "s\n");
      return 0;
    }

    int adopt_cmake_workspace(const std::filesystem::path& workspace_directory,
                              const std::filesystem::path& cmake_path,
                              const std::vector<std::filesystem::path>& projects,
                              const AdoptOptions& options,
                              const ProcessRunner& process_runner,
                              std::ostream& output,
                              std::ostream& error)
    {
      const auto workspace_path = workspace_directory / "forge.workspace.toml";

      if (std::filesystem::exists(workspace_path))
      {
        error << "forge: '" << workspace_path.string() << "' already exists\n";
        return 2;
      }

      std::vector<std::string> relative_directories;
      const auto progress_total = projects.size() + 2;
      report_progress(output, 1, progress_total, "Reading CMake superproject");

      for (const auto& project : projects)
      {
        const auto relative = project.lexically_relative(workspace_directory);

        if (relative.empty()
            || relative == "."
            || relative.is_absolute()
            || *relative.begin() == "..")
        {
          error << "forge: CMake subprojects must live inside the workspace\n";
          return 2;
        }

        if (std::filesystem::exists(project / "forge.recipe.toml"))
        {
          error << "forge: '" << (project / "forge.recipe.toml").string() << "' already exists\n";
          return 2;
        }

        relative_directories.push_back(relative.generic_string());
      }

      for (std::size_t index = 0; index < projects.size(); ++index)
      {
        report_progress(
          output,
          index + 2,
          progress_total,
          "Adopting project " + projects[index].filename().string()
        );

        if (adopt_project_impl(
          projects[index],
          options,
          process_runner,
          false,
          output,
          error
        ) != 0)
        {
          return 2;
        }
      }

      report_progress(output, progress_total, progress_total, "Writing workspace");
      auto name = cmake_path.parent_path().filename().string();
      const auto cmake_project = read_cmake_project(cmake_path, error);

      if (cmake_project && !cmake_project->name.empty())
        name = cmake_project->name;

      const std::string workspace =
        "#:schema " + std::string { workspace_schema_url } + "\n"
        "\n"
        "[workspace]\n"
        "name = \"" + escape_toml_string(name) + "\"\n"
        "projects = " + format_sources(relative_directories) + "\n";

      if (!write_file(workspace_path, workspace, error))
        return 2;

      output << "Created " << workspace_path.string() << '\n'
             << "Adopted " << projects.size() << " CMake project";
      output << (projects.size() == 1 ? "\n" : "s\n");
      return 0;
    }

  } // namespace

  int adopt_project(const std::filesystem::path& project_directory,
                    std::ostream& output,
                    std::ostream& error)
  {
    return adopt_project(project_directory, AdoptOptions {}, run_process, output, error);
  }

  int adopt_project(const std::filesystem::path& project_directory,
                    const AdoptOptions& options,
                    const ProcessRunner& process_runner,
                    std::ostream& output,
                    std::ostream& error)
  {
    return adopt_project_impl(
      project_directory,
      options,
      process_runner,
      true,
      output,
      error
    );
  }

  static int adopt_project_impl(const std::filesystem::path& project_directory,
                                const AdoptOptions& options,
                                const ProcessRunner& process_runner,
                                bool show_progress,
                                std::ostream& output,
                                std::ostream& error)
  {
    const auto explicit_version = options.initial_version
      ? parse_initial_version(*options.initial_version)
      : std::optional<InitialVersion> {};

    if (options.initial_version && !explicit_version)
    {
      error << "forge: initial version must use <major>.<minor>.<patch>[.<build>]\n";
      return 2;
    }

    if (options.version_header_path && !is_safe_project_path(*options.version_header_path))
    {
      error << "forge: version header path must stay inside the project\n";
      return 2;
    }

    const auto solutions = files_with_extension(project_directory, ".sln");
    const auto visual_studio_projects = files_with_extension(project_directory, ".vcxproj");
    const auto xcode_projects = directories_with_extension(project_directory, ".xcodeproj");
    const auto cmake_path = project_directory / "CMakeLists.txt";
    const auto has_cmake_project = std::filesystem::is_regular_file(cmake_path);
    const auto cmake_subdirectories = has_cmake_project
      ? read_cmake_subdirectories(cmake_path)
      : std::vector<std::filesystem::path> {};

    if (has_cmake_project
        && !cmake_subdirectories.empty()
        && !cmake_defines_target(cmake_path)
        && !options.library_type)
    {
      if (options.initial_version || options.version_header_path)
      {
        error << "forge: explicit version initialization applies to a single project, not a workspace\n";
        return 2;
      }

      return adopt_cmake_workspace(
        project_directory,
        cmake_path,
        cmake_subdirectories,
        options,
        process_runner,
        output,
        error
      );
    }

    if (solutions.size() == 1
        && visual_studio_projects.empty()
        && xcode_projects.empty()
        && !has_cmake_project)
    {
      if (options.initial_version || options.version_header_path)
      {
        error << "forge: explicit version initialization applies to a single project, not a workspace\n";
        return 2;
      }

      return adopt_solution(
        project_directory,
        solutions.front(),
        options,
        process_runner,
        output,
        error
      );
    }

    if (show_progress)
      report_progress(output, 1, 6, "Inspecting project");

    const auto recipe_path = project_directory / "forge.recipe.toml";

    std::error_code filesystem_error;

    if (std::filesystem::exists(recipe_path, filesystem_error))
    {
      Recipe existing;

      if (!read_recipe(recipe_path, existing, error))
        return 2;

      if (options.initial_version || options.version_header_path)
      {
        error << "forge: explicit version initialization cannot modify an existing recipe\n";
        return 2;
      }

      output << "Already adopted " << recipe_path.string() << "; recipe preserved\n";
      return 0;
    }

    if (filesystem_error)
    {
      error << "forge: could not inspect '" << recipe_path.string() << "'\n";
      return 2;
    }

    std::vector<std::string> sources;
    std::vector<std::string> public_headers;
    std::vector<std::string> headers;
    std::vector<std::string> discovered_headers;
    std::vector<std::string> entry_points;
    std::optional<VisualStudioProject> visual_studio_project;
    std::vector<VisualStudioProject> cmake_internal_targets;
    std::vector<std::string> merged_project_formats;

    if (show_progress)
      report_progress(output, 2, 6, "Scanning sources and headers");

    ProjectScan scan;

    if (!scan_project(project_directory, scan, error))
      return 2;

    sources = scan.sources;
    public_headers = scan.public_headers;
    headers = scan.headers;
    discovered_headers = scan.headers;
    entry_points = scan.entry_points;

    auto runtime_headers = headers;

    if (show_progress)
      report_progress(output, 3, 6, "Reading project metadata");

    if (visual_studio_projects.size() == 1
        && !(has_cmake_project
             && is_cmake_generated_visual_studio_project(visual_studio_projects.front())))
    {
      visual_studio_project = read_visual_studio_project(visual_studio_projects.front(), error);

      if (!visual_studio_project)
        return 2;
    }
    else if (xcode_projects.size() == 1
             && !(has_cmake_project && is_cmake_generated_xcode_project(xcode_projects.front())))
    {
      visual_studio_project = read_xcode_project(xcode_projects.front(), error);

      if (!visual_studio_project)
        return 2;
    }
    else if (has_cmake_project)
    {
      visual_studio_project = read_cmake_project(cmake_path, error);

      if (!visual_studio_project)
        return 2;
    }

    std::optional<ReleaseNotesInitialVersion> release_notes_version;

    if (!infer_release_notes_initial_version(project_directory, release_notes_version, error))
      return 2;

    if (visual_studio_project && has_cmake_project && visual_studio_project->format != "CMake")
    {
      const auto cmake_project = read_cmake_project(cmake_path, error);

      if (!cmake_project)
        return 2;

      merge_project_metadata(*visual_studio_project, *cmake_project);
      merged_project_formats.push_back("CMake");
    }

    // A static CMake target can privately link a library provided by one of
    // its add_subdirectory() children.  Keep that library as a named Forge
    // target, so build and box can carry the link closure instead of emitting
    // an archive with unresolved vendor symbols.
    if (visual_studio_project && visual_studio_project->format == "CMake")
    {
      const auto rebase_paths = [&](auto& paths, const std::filesystem::path& prefix)
      {
        for (auto& path : paths)
          path = (prefix / path).lexically_normal();
      };
      const auto under_directory = [](const std::filesystem::path& path,
                                      const std::filesystem::path& directory)
      {
        const auto relative = path.lexically_relative(directory);
        return !relative.empty() && relative != "." && !relative.is_absolute()
          && relative.begin()->string() != "..";
      };

      for (const auto& subdirectory : visual_studio_project->cmake_subproject_directories)
      {
        for (const auto& linked : visual_studio_project->cmake_link_dependencies)
        {
          const auto separator = linked.rfind("::");
          const auto target_name = separator == std::string::npos
            ? linked
            : linked.substr(separator + 2);

          if (target_name.empty())
            continue;

          auto target = read_cmake_project(
            project_directory / subdirectory / "CMakeLists.txt",
            error,
            target_name
          );

          if (!target || target->cmake_target.empty()
              || (target->type != "static_library" && target->type != "dynamic_library"
                  && target->type != "header_only"))
          {
            continue;
          }

          if (std::ranges::any_of(
                cmake_internal_targets,
                [&target](const VisualStudioProject& existing)
                {
                  return existing.cmake_target == target->cmake_target;
                }
              ))
          {
            continue;
          }

          rebase_paths(target->sources, subdirectory);
          rebase_paths(target->headers, subdirectory);
          rebase_paths(target->include_directories, subdirectory);
          rebase_paths(target->public_include_directories, subdirectory);

          for (const auto& header : discovered_headers)
          {
            const auto header_path = std::filesystem::path { header };
            const auto is_public = std::ranges::any_of(
              target->public_include_directories,
              [&header_path, &under_directory](const std::filesystem::path& include_directory)
              {
                return under_directory(header_path, include_directory);
              }
            );

            if (is_public)
              target->headers.push_back(header_path.generic_string());
          }

          std::ranges::sort(target->headers);
          target->headers.erase(std::unique(target->headers.begin(), target->headers.end()), target->headers.end());
          cmake_internal_targets.push_back(std::move(*target));
        }
      }
    }

    if (visual_studio_project)
    {
      const auto is_cmake_interface_library_with_subprojects =
        visual_studio_project->format == "CMake"
        && visual_studio_project->type == "header_only"
        && visual_studio_project->has_cmake_subprojects;

      if (is_cmake_interface_library_with_subprojects)
      {
        sources = visual_studio_project->sources;
        const auto scanned_headers = std::move(headers);
        headers.clear();

        for (const auto& header : scanned_headers)
        {
          const auto path = std::filesystem::path { header };
          const auto is_public = std::ranges::any_of(
            visual_studio_project->include_directories,
            [&path](const std::string& include_directory)
            {
              const auto relative = path.lexically_relative(include_directory);
              return !relative.empty()
                && relative.begin()->string() != "..";
            }
          );

          if (is_public)
            headers.push_back(header);
        }

        public_headers = headers;
        entry_points.clear();
      }
      else
      {
        if (!visual_studio_project->sources.empty())
          sources = visual_studio_project->sources;

        if (!visual_studio_project->headers.empty())
          headers = visual_studio_project->headers;

        // CMake target_sources() commonly lists only implementation headers.
        // Keep the discovered public include tree as the library interface
        // even when the selected target supplied a narrower private list.
        if (visual_studio_project->format == "CMake")
        {
          for (const auto& header : discovered_headers)
          {
            const auto path = std::filesystem::path { header };

            if (path.empty() || path.begin()->string() != "include")
              continue;

            if (!std::ranges::binary_search(headers, header))
              headers.push_back(header);
          }

          std::ranges::sort(headers);
          headers.erase(std::unique(headers.begin(), headers.end()), headers.end());
        }

        public_headers.clear();
        entry_points.clear();

        for (const auto& header : headers)
        {
          const auto path = std::filesystem::path { header };

          const auto is_cmake_public_header =
            visual_studio_project->format == "CMake"
            && std::ranges::any_of(
              visual_studio_project->public_include_directories,
              [&path](const std::string& include_directory)
              {
                const auto relative = path.lexically_relative(include_directory);
                return !relative.empty() && relative.begin()->string() != "..";
              }
            );

          if ((!path.empty() && path.begin()->string() == "include") || is_cmake_public_header)
            public_headers.push_back(header);
        }

        for (const auto& source : sources)
        {
          if (contains_main_function(project_directory / source))
            entry_points.push_back(source);
        }
        }
      }

      if (visual_studio_project
          && visual_studio_project->format == "CMake"
          && !visual_studio_project->type.empty())
      {
        const auto is_test_path = [](const std::string& header)
        {
          const auto path = std::filesystem::path { header };

          if (path.empty())
            return false;

          auto first = path.begin()->string();
          std::ranges::transform(first, first.begin(), [](unsigned char character)
          {
            return static_cast<char>(std::tolower(character));
          });
          return first == "test" || first == "tests" || first == "testing";
        };

        std::erase_if(headers, is_test_path);
      }

    runtime_headers.insert(runtime_headers.end(), headers.begin(), headers.end());
    std::ranges::sort(runtime_headers);
    runtime_headers.erase(
      std::unique(runtime_headers.begin(), runtime_headers.end()),
      runtime_headers.end()
    );

    if (show_progress)
      report_progress(output, 4, 6, "Resolving dependencies");

    const auto verify_git_dependencies = options.dependency_style == DependencyStyle::git;
    const std::size_t dependency_progress_total = verify_git_dependencies ? 8 : 7;

    if (show_progress)
      report_subprogress(output, 1, dependency_progress_total, "Inferring include directories");

    std::vector<std::string> reachable_cmake_headers;

    if (visual_studio_project
        && visual_studio_project->format == "CMake"
        && !sources.empty())
    {
      std::set<std::string> reachable;

      for (const auto& source : sources)
      {
        const auto source_headers = reachable_local_headers(project_directory, source, headers);
        reachable.insert(source_headers.begin(), source_headers.end());
      }

      reachable_cmake_headers.assign(reachable.begin(), reachable.end());
    }

    const auto& include_inference_headers = reachable_cmake_headers.empty()
      ? headers
      : reachable_cmake_headers;
    auto include_directories =
      infer_include_directories(
        project_directory,
        sources,
        include_inference_headers,
        visual_studio_project && visual_studio_project->format == "CMake"
          ? &discovered_headers
          : nullptr
      );

    auto dependency_headers = headers;

    if (visual_studio_project
        && visual_studio_project->format == "CMake"
        && (visual_studio_project->type == "static_library"
            || visual_studio_project->type == "dynamic_library"))
    {
      dependency_headers = reachable_cmake_headers;
    }

    if (visual_studio_project)
    {
      const auto cmake_include_directories = visual_studio_project->include_directories;
      include_directories.insert(
        include_directories.end(),
        cmake_include_directories.begin(),
        cmake_include_directories.end()
      );
      std::ranges::sort(include_directories);
      include_directories.erase(
        std::unique(include_directories.begin(), include_directories.end()),
        include_directories.end()
      );

      if (visual_studio_project->format == "CMake")
      {
        std::erase_if(include_directories, [&cmake_include_directories](const std::string& include)
        {
          if (std::ranges::binary_search(cmake_include_directories, include))
            return false;

          const auto candidate = std::filesystem::path { include };

          return std::ranges::any_of(cmake_include_directories, [&candidate](const std::string& parent)
          {
            const auto relative = candidate.lexically_relative(parent);
            return !relative.empty() && relative != "."
              && relative.begin()->string() != "..";
          });
        });
      }
    }

    if (show_progress)
      report_subprogress(output, 2, dependency_progress_total, "Scanning unresolved includes");

    auto unresolved = unresolved_includes(project_directory, sources, dependency_headers);

    if (show_progress)
      report_subprogress(output, 3, dependency_progress_total, "Matching sibling Forge projects");

    auto sibling_dependencies =
      infer_sibling_dependencies(project_directory, unresolved, options.local_search);

    if (show_progress)
      report_subprogress(output, 4, dependency_progress_total, "Checking include directories");

    const auto include_dependency_evidence = infer_include_dependency_evidence(
      project_directory,
      unresolved,
      include_directories,
      sources,
      dependency_headers
    );

    if (show_progress)
      report_subprogress(output, 5, dependency_progress_total, "Reading project references");

    if (visual_studio_project)
    {
      const auto referenced = visual_studio_dependencies(project_directory, *visual_studio_project);
      sibling_dependencies.insert(
        sibling_dependencies.end(),
        referenced.begin(),
        referenced.end()
      );
      std::ranges::sort(
        sibling_dependencies,
        {},
        [](const SiblingDependency& dependency)
        {
          return dependency.name;
        }
      );
      sibling_dependencies.erase(
        std::unique(
          sibling_dependencies.begin(),
          sibling_dependencies.end(),
          [](const SiblingDependency& left, const SiblingDependency& right)
          {
            return left.name == right.name;
          }
        ),
        sibling_dependencies.end()
      );
    }

    if (show_progress)
      report_subprogress(output, 6, dependency_progress_total, "Preparing GitHub suggestions");

    const auto suggestions = github_suggestions(project_directory, unresolved);
    const auto searched_suggestions = options.search_github
      ? github_search_candidates(project_directory, unresolved, process_runner, output)
      : std::map<std::string, std::vector<std::string>> {};

    if (show_progress && verify_git_dependencies)
      report_subprogress(output, 7, dependency_progress_total, "Verifying GitHub candidates");

    const auto github_dependencies = verify_git_dependencies
      ? resolve_github_dependencies(
        project_directory,
        suggestions,
        unresolved,
        process_runner,
        output
      )
      : std::vector<GitHubDependency> {};

    if (show_progress)
    {
      report_subprogress(
        output,
        dependency_progress_total,
        dependency_progress_total,
        "Dependency resolution complete"
      );
    }

    const auto project_name = visual_studio_project
      ? visual_studio_project->name
      : project_directory.filename().string();
    const auto inferred_header_version = infer_packed_macro_version(
      project_directory,
      headers,
      project_name
    );
    const auto metadata_version =
      visual_studio_project && !visual_studio_project->version.empty()
        ? std::optional<std::string> { visual_studio_project->version }
        : inferred_header_version
        ? inferred_header_version
        : infer_component_macro_version(project_directory, headers, project_name);
    const auto project_version =
      explicit_version
        ? explicit_version->version
        : metadata_version
        ? *metadata_version
        : release_notes_version
        ? release_notes_version->version.version
        : "0.1.0";
    const auto initial_build_number = explicit_version
      ? explicit_version->build_number
      : metadata_version
      ? std::optional<int> {}
      : release_notes_version
      ? release_notes_version->version.build_number
      : std::optional<int> {};
    const auto initial_build_number_format = explicit_version
      ? std::optional<std::string> { "dotted" }
      : metadata_version
      ? std::optional<std::string> {}
      : release_notes_version
      ? release_notes_version->build_number_format
      : std::optional<std::string> {};
    const auto escaped_project_name = escape_toml_string(project_name);
    const auto formatted_sources = format_sources(sources);
    const auto formatted_include_directories = format_sources(include_directories);
    std::vector<std::string> header_validation_headers;

    if (visual_studio_project
        && visual_studio_project->format == "CMake"
        && visual_studio_project->type == "header_only"
        && visual_studio_project->has_cmake_subprojects)
    {
      auto lowercase = [](std::string value)
      {
        std::ranges::transform(value, value.begin(), [](unsigned char character)
        {
          return static_cast<char>(std::tolower(character));
        });
        return value;
      };
      const auto project_stem = lowercase(visual_studio_project->name);

      for (const auto& header : public_headers)
      {
        if (lowercase(std::filesystem::path { header }.stem().string()) == project_stem)
        {
          header_validation_headers.push_back(header);
          break;
        }
      }
    }

    const auto formatted_header_validation_headers = format_sources(header_validation_headers);
    const auto has_c_sources = std::ranges::any_of(
      sources,
      [](const std::string& source)
      {
        return is_c_source(std::filesystem::path { source });
      }
    );
    const auto c_standard = visual_studio_project && visual_studio_project->c_standard != 0
      ? visual_studio_project->c_standard
      : 11;
    const auto c_standard_property = has_c_sources
      ? "c_std = " + std::to_string(c_standard) + "\n"
      : std::string {};
    auto version_headers = infer_version_headers(project_directory, headers);
    bool initialize_version_header = explicit_version && version_headers.size() == 1;

    if (options.version_header_path)
    {
      const auto requested = options.version_header_path->lexically_normal().generic_string();
      const auto existing =
        std::ranges::find(version_headers, requested, &VersionHeaderCandidate::path);

      if (std::filesystem::is_regular_file(project_directory / *options.version_header_path))
      {
        if (existing == version_headers.end())
        {
          error << "forge: existing version header does not declare the supported five-macro format\n";
          return 2;
        }

        version_headers = { *existing };
      }
      else
      {
        version_headers = { { requested, version_macro_prefix(project_name) } };
        initialize_version_header = true;

        const auto path = std::filesystem::path { requested };

        if (!path.empty() && path.begin()->string() == "include")
        {
          headers.push_back(requested);
          public_headers.push_back(requested);
          std::ranges::sort(headers);
          std::ranges::sort(public_headers);
        }
      }

      initialize_version_header = initialize_version_header || explicit_version.has_value();
    }

    discovered_headers.insert(discovered_headers.end(), headers.begin(), headers.end());
    std::ranges::sort(discovered_headers);
    discovered_headers.erase(
      std::unique(discovered_headers.begin(), discovered_headers.end()),
      discovered_headers.end()
    );

    if (visual_studio_project && visual_studio_project->format == "CMake")
    {
      export_headers_below_public_include_roots(
        public_headers,
        discovered_headers,
        visual_studio_project->public_include_directories
      );
    }

    expand_public_header_closure(project_directory, public_headers, discovered_headers);

    const auto formatted_headers = format_sources(public_headers);
    const auto inferred_library_type =
      options.library_type
        ? *options.library_type
        : visual_studio_project
          && (visual_studio_project->type == "header_only"
              || visual_studio_project->type == "static_library"
              || visual_studio_project->type == "dynamic_library")
        ? visual_studio_project->type
        : std::string {};
    const auto inferred_library_with_program =
      entry_points.size() == 1 && !inferred_library_type.empty();
    const auto inferred_target_count =
      entry_points.size() > 1 || inferred_library_with_program
        ? entry_points.size() + (inferred_library_type.empty() ? 0 : 1)
        : 0;
    std::vector<std::pair<std::string, RuntimeFile>> inferred_runtime_files;
    std::string recipe =
      "#:schema " + std::string { recipe_schema_url } + "\n"
      "\n"
      "[project]\n"
      "name = \"" + escaped_project_name + "\"\n"
      "version = \"" + escape_toml_string(project_version) + "\"\n";

    if (entry_points.size() > 1 || inferred_library_with_program)
    {
      std::set<std::string> target_names;
      const auto inferred_target_sources =
        infer_target_sources(project_directory, sources, headers, entry_points);
      std::string library_target;

      if (!inferred_library_type.empty())
      {
        library_target = target_name(project_name, 0);
        target_names.insert(library_target);
        std::vector<std::string> library_sources;

        for (const auto& source : sources)
        {
          if (!std::binary_search(entry_points.begin(), entry_points.end(), source))
            library_sources.push_back(source);
        }

        recipe
          += "\n[target." + library_target + "]\n"
          "type = \"" + inferred_library_type + "\"\n"
          "cpp_std = " + std::to_string(
            visual_studio_project ? visual_studio_project->cpp_standard : 20
          ) + "\n"
          + c_standard_property
          + "sources = " + format_sources(library_sources) + "\n";

        if (!public_headers.empty())
          recipe += "public_headers = " + formatted_headers + "\n";

        if (!include_directories.empty())
          recipe += "include_dirs = " + formatted_include_directories + "\n";

        if (visual_studio_project && !visual_studio_project->definitions.empty())
          recipe += "defines = " + format_sources(visual_studio_project->definitions) + "\n";

        if (visual_studio_project)
          recipe += format_system_links(*visual_studio_project);
      }

      for (std::size_t index = 0; index < entry_points.size(); ++index)
      {
        auto name = target_name(entry_points[index], index);

        if (!target_names.insert(name).second)
        {
          name += '-' + std::to_string(index + 1);
          target_names.insert(name);
        }

        recipe
          += "\n[target." + name + "]\n"
          "type = \"executable\"\n"
          "cpp_std = " + std::to_string(
            visual_studio_project ? visual_studio_project->cpp_standard : 20
          ) + "\n"
          + c_standard_property
          + "sources = " + format_sources(inferred_target_sources[index]) + "\n";

        auto runtime_files =
          infer_runtime_files(project_directory, inferred_target_sources[index], runtime_headers);

        if (visual_studio_project)
          merge_runtime_files(runtime_files, visual_studio_project->runtime_files);

        if (!runtime_files.empty())
        {
          recipe += "runtime_files = " + format_runtime_files(runtime_files) + "\n";

          for (const auto& runtime_file : runtime_files)
            inferred_runtime_files.emplace_back(name, runtime_file);
        }

        if (!library_target.empty())
          recipe += "dependencies = [\"" + escape_toml_string(library_target) + "\"]\n";
        else
        {
          if (!public_headers.empty())
            recipe += "public_headers = " + formatted_headers + "\n";

          if (!include_directories.empty())
            recipe += "include_dirs = " + formatted_include_directories + "\n";

          if (visual_studio_project && !visual_studio_project->definitions.empty())
            recipe += "defines = " + format_sources(visual_studio_project->definitions) + "\n";

          if (visual_studio_project)
            recipe += format_system_links(*visual_studio_project);
        }

        auto first_directory = std::filesystem::path { entry_points[index] }.begin()->string();
        std::ranges::transform(
          first_directory,
          first_directory.begin(),
          [](unsigned char character)
          {
            return static_cast<char>(std::tolower(character));
          }
        );

        if (first_directory == "test" || first_directory == "tests")
          recipe += "test = true\n";
      }

      if (initial_build_number)
        recipe += "\n[build]\nnumber = " + std::to_string(*initial_build_number) + "\n";
    }
    else
    {
      const auto type =
        options.library_type
          ? *options.library_type
          : visual_studio_project && !visual_studio_project->type.empty()
          ? visual_studio_project->type
          : sources.empty() && !public_headers.empty()
          ? "header_only"
          : entry_points.empty() && !sources.empty() && !public_headers.empty()
          ? "static_library"
          : "executable";
      if (!cmake_internal_targets.empty())
      {
        recipe
          += "\n[defaults]\n"
          "target = \"" + escape_toml_string(project_name) + "\"\n"
          "\n[target." + project_name + "]\n"
          "type = \"" + std::string { type } + "\"\n"
          "cpp_std = " + std::to_string(
            visual_studio_project ? visual_studio_project->cpp_standard : 20
          ) + "\n"
          + c_standard_property
          + "sources = " + formatted_sources + "\n";

        if (!public_headers.empty())
          recipe += "public_headers = " + formatted_headers + "\n";

        if (!include_directories.empty())
          recipe += "include_dirs = " + formatted_include_directories + "\n";

        if (visual_studio_project && !visual_studio_project->definitions.empty())
          recipe += "defines = " + format_sources(visual_studio_project->definitions) + "\n";

        if (visual_studio_project)
          recipe += format_system_links(*visual_studio_project);

        recipe += "dependencies = [";

        for (std::size_t index = 0; index < cmake_internal_targets.size(); ++index)
        {
          if (index != 0)
            recipe += ", ";

          recipe += "\"" + escape_toml_string(cmake_internal_targets[index].cmake_target) + "\"";
        }

        recipe += "]\n";

        for (const auto& target : cmake_internal_targets)
        {
          const auto target_has_c_sources = std::ranges::any_of(
            target.sources,
            [](const std::string& source)
            {
              return is_c_source(std::filesystem::path { source });
            }
          );
          recipe
            += "\n[target." + target.cmake_target + "]\n"
            "type = \"" + target.type + "\"\n"
            "cpp_std = " + std::to_string(target.cpp_standard) + "\n";

          if (target_has_c_sources)
            recipe += "c_std = " + std::to_string(target.c_standard == 0 ? 11 : target.c_standard) + "\n";

          recipe += "sources = " + format_sources(target.sources) + "\n";

          if (!target.headers.empty())
            recipe += "public_headers = " + format_sources(target.headers) + "\n";

          if (!target.include_directories.empty())
            recipe += "include_dirs = " + format_sources(target.include_directories) + "\n";

          if (!target.definitions.empty())
            recipe += "defines = " + format_sources(target.definitions) + "\n";

          recipe += format_system_links(target);
        }

        if (initial_build_number)
          recipe += "\n[build]\nnumber = " + std::to_string(*initial_build_number) + "\n";
      }
      else
      {
        recipe
          += "type = \"" + std::string { type } + "\"\n"
          "cpp_std = " + std::to_string(
            visual_studio_project ? visual_studio_project->cpp_standard : 20
          ) + "\n"
          + c_standard_property
          + "\n"
          "[sources]\n"
          "paths = " + formatted_sources + "\n";

        if (!public_headers.empty())
          recipe += "public_headers = " + formatted_headers + "\n";

        if (!header_validation_headers.empty())
          recipe += "validation_headers = " + formatted_header_validation_headers + "\n";

        if (!include_directories.empty())
          recipe += "include_dirs = " + formatted_include_directories + "\n";

        if (initial_build_number
            || (visual_studio_project
                && (visual_studio_project->python_extension
                    || !visual_studio_project->definitions.empty()
                    || !format_system_links(*visual_studio_project).empty())))
        {
          recipe += "\n[build]\n";

          if (initial_build_number)
            recipe += "number = " + std::to_string(*initial_build_number) + "\n";

          if (visual_studio_project && visual_studio_project->python_extension)
            recipe += "python_extension = true\n";

          if (visual_studio_project && !visual_studio_project->definitions.empty())
            recipe += "defines = " + format_sources(visual_studio_project->definitions) + "\n";

          if (visual_studio_project)
            recipe += format_system_links(*visual_studio_project);
        }
      }

      if (type == "executable")
      {
        auto runtime_files = infer_runtime_files(project_directory, sources, runtime_headers);

        if (visual_studio_project)
          merge_runtime_files(runtime_files, visual_studio_project->runtime_files);

        if (!runtime_files.empty())
        {
          recipe += "\n[runtime]\nfiles = " + format_runtime_files(runtime_files) + "\n";

          for (const auto& runtime_file : runtime_files)
            inferred_runtime_files.emplace_back(project_name, runtime_file);
        }
      }
    }

    if (initial_build_number)
    {
      recipe += "\n[release]\nbuild_number_format = \""
        + initial_build_number_format.value_or("dotted") + "\"\n";
    }

    if (!sibling_dependencies.empty())
    {
      recipe += "\n[dependencies.style.local-source]\n";

      for (const auto& dependency : sibling_dependencies)
      {
        recipe += dependency.name + " = { path = \""
          + escape_toml_string(dependency.path) + "\" }\n";
      }
    }

    if (!github_dependencies.empty())
    {
      recipe += "\n[dependencies.style.git-source]\n";

      for (const auto& dependency : github_dependencies)
      {
        recipe += dependency.name + " = { git = \"" + escape_toml_string(dependency.git)
          + "\", commit = \"" + dependency.commit + "\" }\n";
      }
    }

    if (sibling_dependencies.empty() && !github_dependencies.empty())
    {
      recipe += "\n[profile.workflow-release.dependencies]\n";

      for (const auto& dependency : github_dependencies)
      {
        recipe += dependency.name + " = { git = \"" + escape_toml_string(dependency.git)
          + "\", commit = \"" + dependency.commit + "\" }\n";
      }
    }
    else if (!sibling_dependencies.empty())
    {
      recipe
        += "\n# TODO(workflow-release): replace each local dependency with a published pin.\n"
        "# [profile.workflow-release.dependencies]\n";

      for (const auto& dependency : sibling_dependencies)
      {
        recipe += "# " + dependency.name
          + " = { github = \"owner/" + dependency.name
          + "\", version = \"<published-version>\" }\n"
          + "# forge update " + dependency.name
          + " --profile=workflow-release --release-targets\n";
      }
    }

    recipe += "\n[profile.workflow-release.build]\nconfiguration = \"Release\"\n";

    if (version_headers.size() == 1)
    {
      recipe += "\n[version_header]\n"
        "path = \"" + escape_toml_string(version_headers.front().path) + "\"\n"
        "prefix = \"" + version_headers.front().prefix + "\"\n";
    }

    if (visual_studio_project)
    {
      for (const auto& [name, profile] : visual_studio_project->profiles)
      {
        auto selector = name;
        std::ranges::transform(
          selector,
          selector.begin(),
          [](unsigned char character)
          {
            return static_cast<char>(std::tolower(character));
          }
        );
        recipe += "\n[build.config." + escape_toml_string(selector) + "]\n"
          "configuration = \"" + escape_toml_string(profile.configuration) + "\"\n";

        if (profile.cpp_standard != 0 && profile.cpp_standard != visual_studio_project->cpp_standard)
          recipe += "cpp_std = " + std::to_string(profile.cpp_standard) + "\n";

        if (!profile.include_directories.empty())
        {
          std::vector<std::string> includes;

          for (const auto& include : profile.include_directories)
            includes.push_back(include.generic_string());

          recipe += "include_dirs = " + format_sources(includes) + "\n";
        }

        if (!profile.compile_definitions.empty())
          recipe += "defines = " + format_sources(profile.compile_definitions) + "\n";
      }
    }

    if (show_progress)
      report_progress(output, 5, 6, "Writing recipe");

    if (!write_file(recipe_path, recipe, error))
      return 2;

    if (initialize_version_header)
    {
      const auto path = project_directory / version_headers.front().path;
      std::error_code directory_error;
      std::filesystem::create_directories(path.parent_path(), directory_error);

      if (directory_error
          || !write_file(
            path,
            generated_version_header(
              version_headers.front().prefix,
              InitialVersion { project_version, initial_build_number }
            ),
            error
          ))
      {
        return 2;
      }
    }

    if (show_progress)
      report_progress(output, 6, 6, "Creating release support");

    if (!generate_github_release_support(
      project_directory,
      qualified_initial_version(InitialVersion { project_version, initial_build_number }),
      error
    ))
    {
      return 2;
    }

    const auto c_source_count = std::ranges::count_if(
      sources,
      [](const std::string& source)
      {
        return is_c_source(std::filesystem::path { source });
      }
    );
    const auto source_language = c_source_count == 0
      ? "C++"
      : c_source_count == static_cast<decltype(c_source_count)>(sources.size())
      ? "C"
      : "C/C++";

    output
      << "Adopted project '" << project_name << "'\n"
      << "Created " << recipe_path.string() << '\n'
      << "Found " << sources.size() << ' ' << source_language << " source file";

    if (sources.size() != 1)
      output << 's';

    output << '\n'
           << "Found " << entry_points.size() << " main() entry point";

    if (entry_points.size() != 1)
      output << 's';

    output << '\n';

    if (visual_studio_project)
    {
      output << "Imported " << visual_studio_project->format << " project "
             << visual_studio_project->path.filename().string()
             << '\n';

      for (const auto& format : merged_project_formats)
        output << "Merged mirrored " << format << " project metadata\n";

      if (!visual_studio_project->profiles.empty())
      {
        output << "Imported " << visual_studio_project->profiles.size()
               << ' ' << visual_studio_project->format << " build profile";
        output << (visual_studio_project->profiles.size() == 1 ? "\n" : "s\n");
      }

      const auto system_links =
        visual_studio_project->macos_frameworks.size()
        + visual_studio_project->macos_libraries.size()
        + visual_studio_project->linux_libraries.size()
        + visual_studio_project->windows_libraries.size();

      if (system_links != 0)
      {
        output << "Imported " << system_links << " platform link requirement";
        output << (system_links == 1 ? "\n" : "s\n");
      }

      if (!visual_studio_project->unresolved_properties.empty())
      {
        output << "Skipped " << visual_studio_project->unresolved_properties.size()
               << " unresolved " << visual_studio_project->format << " value";
        output << (visual_studio_project->unresolved_properties.size() == 1 ? ":\n" : "s:\n");

        for (const auto& value : visual_studio_project->unresolved_properties)
          output << "  " << value << '\n';
      }
    }

    if (inferred_target_count != 0)
      output << "Inferred " << inferred_target_count << " Forge targets\n";

    if (!include_directories.empty())
    {
      output << "Inferred " << include_directories.size() << " local include director";
      output << (include_directories.size() == 1 ? "y\n" : "ies\n");
    }

    if (version_headers.size() == 1)
    {
      output << (initialize_version_header ? "Initialized version header " : "Inferred version header ")
             << version_headers.front().path
             << " with prefix " << version_headers.front().prefix << '\n';
    }
    else if (version_headers.size() > 1)
    {
      output << "Found " << version_headers.size()
             << " possible version headers; configure [version_header] manually:\n";

      for (const auto& candidate : version_headers)
        output << "  " << candidate.path << " with prefix " << candidate.prefix << '\n';
    }

    if (!inferred_runtime_files.empty())
    {
      output << "Inferred " << inferred_runtime_files.size() << " runtime asset";
      output << (inferred_runtime_files.size() == 1 ? ":\n" : "s:\n");

      for (const auto& [target, runtime_file] : inferred_runtime_files)
      {
        output << "  " << target << ": " << runtime_file.source.generic_string();

        if (runtime_file.source != runtime_file.destination)
          output << " -> " << runtime_file.destination.generic_string();

        output << '\n';
      }
    }

    if (!unresolved.empty())
    {
      output << "Found " << unresolved.size() << " unresolved dependency include";
      output << (unresolved.size() == 1 ? ":\n" : "s:\n");

      for (const auto& [include, source] : unresolved)
        output << "  <" << include << "> from " << source << '\n';
    }

    if (!include_dependency_evidence.empty())
    {
      output << "Resolved " << include_dependency_evidence.size()
             << " dependency include";
      output << (include_dependency_evidence.size() == 1
        ? " through include directories:\n"
        : "s through include directories:\n");

      for (const auto& evidence : include_dependency_evidence)
      {
        output << "  <" << evidence.include << "> from " << evidence.source
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
          output << "    source: https://github.com/" << evidence.github << '\n';
      }
    }

    if (!sibling_dependencies.empty())
    {
      output << "Inferred " << sibling_dependencies.size() << " sibling project dependenc";
      output << (sibling_dependencies.size() == 1 ? "y:\n" : "ies:\n");

      for (const auto& dependency : sibling_dependencies)
        output << "  " << dependency.name << " = " << dependency.path << '\n';

      output
        << "Workflow release profile requires reproducible replacements for "
        << sibling_dependencies.size() << " local dependenc"
        << (sibling_dependencies.size() == 1 ? "y\n" : "ies\n");
    }

    if (!verify_git_dependencies && !suggestions.empty())
    {
      output << "Suggested GitHub dependencies:\n";

      for (const auto& [repository, includes] : suggestions)
      {
        output << "  " << repository << " for";

        for (const auto& include : includes)
          output << " <" << include << ">";

        output << '\n';
      }

      output << "Run 'forge adopt --dependency-style=git' to verify and pin suggestions\n";
    }

    if (!searched_suggestions.empty())
    {
      output << "GitHub search candidates:\n";

      for (const auto& [repository, includes] : searched_suggestions)
      {
        output << "  " << repository << " for";

        for (const auto& include : includes)
          output << " <" << include << ">";

        output << '\n';
      }

      output << "Add the matching dependency manually or rerun with an explicit dependency style after choosing one\n";
    }

    if (entry_points.empty()
        && !sources.empty()
        && public_headers.empty()
        && (!visual_studio_project || visual_studio_project->type.empty()))
      output << "Could not infer a library interface; generated an executable recipe\n";

    return 0;
  }

  int init_project(const std::filesystem::path& project_directory,
                   std::ostream& output,
                   std::ostream& error)
  {
    return adopt_project(project_directory, AdoptOptions {}, run_process, output, error);
  }

} // namespace forge
