#include "init.h"

#include "init_cmake.h"
#include "init_support.h"
#include "init_xcode.h"
#include "init_visual_studio.h"

#include "file_support.h"
#include "github.h"
#include "recipe.h"
#include "versioning.h"
#include "workspace.h"

#include <algorithm>
#include <array>
#include <cctype>
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

    bool is_ignored_directory(const std::filesystem::path& path)
    {
      const auto name = path.filename().string();

      return
        name == ".git"
        || name == ".forge"
        || name == "build"
        || name == "out"
        || name.starts_with("cmake-build-");
    }

    bool is_cpp_source(const std::filesystem::path& path)
    {
      const auto extension = path.extension().string();
      return extension == ".cpp" || extension == ".cc" || extension == ".cxx";
    }

    bool is_cpp_header(const std::filesystem::path& path)
    {
      const auto extension = path.extension().string();
      return extension == ".h"
        || extension == ".hpp"
        || extension == ".hh"
        || extension == ".hxx";
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

      project.cpp_standard = std::max(project.cpp_standard, additional.cpp_standard);

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
      project.linux_libraries.insert(
        project.linux_libraries.end(),
        additional.linux_libraries.begin(),
        additional.linux_libraries.end()
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
        profile.cpp_standard = std::max(profile.cpp_standard, project.cpp_standard);

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

    bool contains_main_function(const std::filesystem::path& path)
    {
      std::ifstream file { path };
      const std::string contents {
        std::istreambuf_iterator<char> { file },
        std::istreambuf_iterator<char> {}
      };
      bool line_comment = false;
      bool block_comment = false;
      char quote = '\0';

      for (std::size_t index = 0; index < contents.size(); ++index)
      {
        const auto character = contents[index];
        const auto next = index + 1 < contents.size() ? contents[index + 1] : '\0';

        if (line_comment)
        {
          line_comment = character != '\n';
          continue;
        }

        if (block_comment)
        {
          if (character == '*' && next == '/')
          {
            block_comment = false;
            ++index;
          }

          continue;
        }

        if (quote != '\0')
        {
          if (character == '\\')
            ++index;
          else if (character == quote)
            quote = '\0';

          continue;
        }

        if (character == '/' && next == '/')
        {
          line_comment = true;
          ++index;
          continue;
        }

        if (character == '/' && next == '*')
        {
          block_comment = true;
          ++index;
          continue;
        }

        if (character == '"' || character == '\'')
        {
          quote = character;
          continue;
        }

        if (contents.compare(index, std::string_view { "main" }.size(), "main") != 0
            || (index != 0
                && (std::isalnum(static_cast<unsigned char>(contents[index - 1]))
                    || contents[index - 1] == '_')))
        {
          continue;
        }

        auto position = index + std::string_view { "main" }.size();

        while (position < contents.size()
               && std::isspace(static_cast<unsigned char>(contents[position])))
        {
          ++position;
        }

        if (position < contents.size() && contents[position] == '(')
          return true;
      }

      return false;
    }

    bool discover_sources(const std::filesystem::path& project_directory,
                          std::vector<std::string>& sources,
                          std::vector<std::string>& public_headers,
                          std::vector<std::string>& headers,
                          std::vector<std::string>& entry_points,
                          std::ostream& error)
    {
      std::error_code filesystem_error;
      std::filesystem::recursive_directory_iterator iterator {
        project_directory,
        std::filesystem::directory_options::skip_permission_denied,
        filesystem_error
      };
      const std::filesystem::recursive_directory_iterator end;

      if (filesystem_error)
      {
        error << "forge: could not inspect '" << project_directory.string() << "'\n";
        return false;
      }

      while (iterator != end)
      {
        const auto& entry = *iterator;

        if (entry.is_directory(filesystem_error) && is_ignored_directory(entry.path()))
          iterator.disable_recursion_pending();
        else if (!filesystem_error && entry.is_regular_file(filesystem_error) && is_cpp_source(entry.path()))
        {
          const auto relative = entry.path().lexically_relative(project_directory).generic_string();
          sources.push_back(relative);

          if (contains_main_function(entry.path()))
            entry_points.push_back(relative);
        }
        else if (!filesystem_error
                 && entry.is_regular_file(filesystem_error)
                 && is_cpp_header(entry.path()))
        {
          const auto relative = entry.path().lexically_relative(project_directory);
          headers.push_back(relative.generic_string());

          if (relative.begin()->string() == "include")
            public_headers.push_back(relative.generic_string());
        }

        if (filesystem_error)
        {
          error << "forge: could not inspect '" << entry.path().string() << "'\n";
          return false;
        }

        iterator.increment(filesystem_error);

        if (filesystem_error)
        {
          error << "forge: could not inspect '" << project_directory.string() << "'\n";
          return false;
        }
      }

      std::ranges::sort(sources);
      std::ranges::sort(public_headers);
      std::ranges::sort(headers);
      std::ranges::sort(entry_points);
      return true;
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

    struct IncludedHeader
    {
      std::string path;
      bool quoted = false;
    };

    std::vector<IncludedHeader> included_headers(const std::filesystem::path& path)
    {
      std::ifstream file { path };
      std::vector<IncludedHeader> includes;
      std::string line;

      while (std::getline(file, line))
      {
        auto content = std::string_view { line };
        const auto first = content.find_first_not_of(" \t");

        if (first == std::string_view::npos || content[first] != '#')
          continue;

        content.remove_prefix(first + 1);
        const auto directive = content.find_first_not_of(" \t");

        if (directive == std::string_view::npos)
          continue;

        content.remove_prefix(directive);

        if (!content.starts_with("include"))
          continue;

        content.remove_prefix(std::string_view { "include" }.size());
        const auto delimiter = content.find_first_not_of(" \t");

        if (delimiter == std::string_view::npos
            || (content[delimiter] != '<' && content[delimiter] != '"'))
        {
          continue;
        }

        const auto closing = content[delimiter] == '<' ? '>' : '"';
        const auto end = content.find(closing, delimiter + 1);

        if (end != std::string_view::npos)
        {
          auto include = std::string { content.substr(delimiter + 1, end - delimiter - 1) };
          std::replace(include.begin(), include.end(), '\\', '/');
          includes.push_back({ std::move(include), content[delimiter] == '"' });
        }
      }

      return includes;
    }

    bool looks_like_dependency_include(std::string_view include)
    {
      static const std::set<std::string_view> system_headers {
        "assert.h", "complex.h", "conio.h", "ctype.h", "errno.h", "fenv.h", "float.h",
        "inttypes.h", "limits.h", "locale.h", "math.h", "process.h", "setjmp.h",
        "signal.h", "stdarg.h", "stdbool.h", "stddef.h", "stdint.h", "stdio.h",
        "stdlib.h", "string.h", "termios.h", "time.h", "uchar.h", "unistd.h", "wchar.h",
        "wctype.h", "windows.h", "audioclient.h", "ksmedia.h", "mmdeviceapi.h",
        "algorithm", "any", "array", "atomic", "barrier", "bit", "bitset",
        "cassert", "ccomplex", "cctype", "cerrno", "cfenv", "cfloat", "charconv",
        "chrono", "cinttypes", "ciso646", "climits", "clocale", "cmath",
        "codecvt", "compare", "complex", "concepts", "condition_variable",
        "coroutine", "csetjmp", "csignal", "cstdarg", "cstdbool", "cstddef",
        "cstdint", "cstdio", "cstdlib", "cstring", "ctgmath", "ctime", "cuchar",
        "cwchar", "cwctype", "deque",
        "exception", "execution", "filesystem", "format", "forward_list",
        "fstream", "functional", "future", "initializer_list", "iomanip",
        "ios", "iosfwd", "iostream", "istream", "iterator", "latch", "limits",
        "list", "map", "memory", "memory_resource", "mutex", "new", "numbers",
        "numeric", "optional", "ostream", "queue", "random", "ranges", "ratio",
        "regex", "scoped_allocator", "semaphore", "set", "shared_mutex",
        "source_location", "span", "sstream", "stack", "stdexcept", "stop_token",
        "streambuf", "string", "string_view", "syncstream", "system_error",
        "thread", "tuple", "type_traits", "typeindex", "typeinfo",
        "unordered_map", "unordered_set", "utility", "valarray", "variant",
        "vector", "version"
      };

      return
        !system_headers.contains(include)
        && !include.starts_with("sys/")
        && !include.starts_with("linux/")
        && !include.starts_with("machine/")
        && !include.starts_with("arpa/")
        && !include.starts_with("netinet/")
        && !include.starts_with("alsa/")
        && !include.starts_with("AudioToolbox/")
        && !include.starts_with("CoreAudio/")
        && !include.starts_with("CoreFoundation/");
    }

    std::optional<std::string> resolve_local_header(
      const std::string& including_file,
      const std::string& include,
      const std::vector<std::string>& headers);

    std::map<std::string, std::string> unresolved_includes(
      const std::filesystem::path& project_directory,
      const std::vector<std::string>& sources,
      const std::vector<std::string>& headers)
    {
      std::map<std::string, std::string> unresolved;
      std::vector<std::string> scanned_files = sources;
      scanned_files.insert(scanned_files.end(), headers.begin(), headers.end());

      for (const auto& scanned_file : scanned_files)
      {
        for (const auto& included : included_headers(project_directory / scanned_file))
        {
          const auto& include = included.path;

          if (!resolve_local_header(scanned_file, include, headers)
              && looks_like_dependency_include(include))
          {
            unresolved.try_emplace(include, scanned_file);
          }
        }
      }

      return unresolved;
    }

    struct SiblingDependency
    {
      std::string name;
      std::string path;
    };

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
          dependencies.push_back({ referenced->name, relative.generic_string() });
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

    bool provides_include(const Recipe& recipe, std::string_view include)
    {
      if (!recipe.targets.empty())
      {
        return std::ranges::any_of(
          recipe.targets,
          [include](const RecipeTarget& target)
          {
            if (target.type != "static_library"
                && target.type != "dynamic_library"
                && target.type != "header_only"
                && target.type != "imported_library")
            {
              return false;
            }

            return std::ranges::any_of(
              target.public_headers,
              [include](const std::filesystem::path& header)
              {
                const auto generic = header.generic_string();
                return generic.starts_with("include/") && generic.substr(8) == include;
              }
            );
          }
        );
      }

      if (recipe.type != "static_library"
              && recipe.type != "dynamic_library"
              && recipe.type != "header_only"
              && recipe.type != "imported_library")
      {
        return false;
      }

      for (const auto& header : recipe.public_headers)
      {
        const auto generic = header.generic_string();

        if (generic.starts_with("include/") && generic.substr(8) == include)
          return true;
      }

      for (const auto& profile : recipe.imports)
      {
        for (const auto& header : profile.public_headers)
        {
          const auto generic = header.generic_string();

          if (generic == include || generic.ends_with('/' + std::string { include }))
            return true;
        }
      }

      return false;
    }

    std::vector<SiblingDependency> infer_sibling_dependencies(
      const std::filesystem::path& project_directory,
      std::map<std::string, std::string>& unresolved)
    {
      const auto parent = project_directory.parent_path();
      std::error_code filesystem_error;
      std::map<std::string, std::vector<SiblingDependency>> matches;

      for (const auto& entry : std::filesystem::directory_iterator { parent, filesystem_error })
      {
        if (filesystem_error)
          break;

        if (!entry.is_directory(filesystem_error)
            || std::filesystem::equivalent(entry.path(), project_directory, filesystem_error)
            || !std::filesystem::is_regular_file(
              entry.path() / "forge.recipe.toml",
              filesystem_error
            ))
        {
          filesystem_error.clear();
          continue;
        }

        Recipe sibling;
        std::ostringstream ignored_error;

        if (!read_recipe(entry.path() / "forge.recipe.toml", sibling, ignored_error))
          continue;

        const auto relative =
          std::filesystem::relative(entry.path(), project_directory, filesystem_error);

        if (filesystem_error)
        {
          filesystem_error.clear();
          continue;
        }

        for (const auto& [include, source] : unresolved)
        {
          if (provides_include(sibling, include))
          {
            matches[include].push_back(SiblingDependency {
              sibling.name,
              relative.generic_string()
            });
          }
        }
      }

      std::map<std::string, std::vector<std::pair<std::string, SiblingDependency>>> by_name;

      for (const auto& [include, candidates] : matches)
      {
        if (candidates.size() == 1)
          by_name[candidates.front().name].emplace_back(include, candidates.front());
      }

      std::vector<SiblingDependency> result;

      for (const auto& [name, candidates] : by_name)
      {
        const auto path = candidates.front().second.path;
        const auto same_project = std::ranges::all_of(
          candidates,
          [&path](const auto& candidate)
          {
            return candidate.second.path == path;
          }
        );

        if (!same_project)
          continue;

        result.push_back(candidates.front().second);

        for (const auto& [include, dependency] : candidates)
          unresolved.erase(include);
      }

      return result;
    }

    std::optional<std::string> github_owner(const std::filesystem::path& project_directory)
    {
      std::ifstream config { project_directory / ".git" / "config" };
      std::string line;
      bool origin = false;

      while (std::getline(config, line))
      {
        const auto content = std::string_view { line };

        if (content.starts_with('['))
        {
          origin =
            content.find("remote \"origin\"") != std::string_view::npos;
          continue;
        }

        const auto equals = content.find('=');

        if (!origin || equals == std::string_view::npos)
          continue;

        const auto key = content.substr(0, equals);

        if (key.find("url") == std::string_view::npos)
          continue;

        auto url = std::string { content.substr(equals + 1) };
        const auto first = url.find_first_not_of(" \t");
        url = first == std::string::npos ? std::string {} : url.substr(first);
        std::string_view path;

        if (const auto github = url.find("github.com/"); github != std::string::npos)
        {
          path = std::string_view { url }.substr(github + 11);
        }
        else if (const auto github = url.find("github.com:"); github != std::string::npos)
        {
          path = std::string_view { url }.substr(github + 11);
        }

        const auto slash = path.find('/');

        if (slash != std::string_view::npos && slash != 0)
        {
          return std::string { path.substr(0, slash) };
        }
      }

      return std::nullopt;
    }

    std::string github_repository_name(std::string_view include)
    {
      const auto slash = include.find('/');
      auto name = std::string {
        slash == std::string_view::npos ? include : include.substr(0, slash)
      };

      if (slash == std::string_view::npos)
      {
        const auto extension = name.rfind('.');

        if (extension != std::string::npos)
          name.resize(extension);
      }

      const auto safe =
        !name.empty()
        && name != "."
        && name != ".."
        && std::ranges::all_of(
          name,
          [](unsigned char character)
          {
            return std::isalnum(character)
              || character == '-'
              || character == '_'
              || character == '.';
          }
        );
      return safe ? name : std::string {};
    }

    std::map<std::string, std::vector<std::string>> github_suggestions(
      const std::filesystem::path& project_directory,
      const std::map<std::string, std::string>& unresolved)
    {
      std::map<std::string, std::vector<std::string>> suggestions;
      const auto owner = github_owner(project_directory);

      if (!owner)
        return suggestions;

      for (const auto& [include, source] : unresolved)
      {
        const auto name = github_repository_name(include);

        if (!name.empty())
          suggestions[*owner + "/" + name].push_back(include);
      }

      return suggestions;
    }

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

    std::vector<std::string> infer_include_directories(
      const std::filesystem::path& project_directory,
      const std::vector<std::string>& sources,
      const std::vector<std::string>& headers)
    {
      std::set<std::string> include_directories;
      std::vector<std::string> scanned_files = sources;
      scanned_files.insert(scanned_files.end(), headers.begin(), headers.end());

      for (const auto& scanned_file : scanned_files)
      {
        for (const auto& included : included_headers(project_directory / scanned_file))
        {
          const auto& include = included.path;

          if (included.quoted)
          {
            const auto relative =
              (std::filesystem::path { scanned_file }.parent_path() / include)
                .lexically_normal()
                .generic_string();

            if (std::binary_search(headers.begin(), headers.end(), relative))
              continue;
          }

          std::set<std::string> matching_roots;

          for (const auto& header : headers)
          {
            if (header == include)
            {
              matching_roots.insert(".");
              continue;
            }

            const auto suffix = '/' + include;

            if (header.size() > suffix.size() && header.ends_with(suffix))
              matching_roots.insert(header.substr(0, header.size() - suffix.size()));
          }

          if (matching_roots.size() == 1 && *matching_roots.begin() != "include")
            include_directories.insert(*matching_roots.begin());
        }
      }

      return { include_directories.begin(), include_directories.end() };
    }

    std::optional<std::string> resolve_local_header(
      const std::string& including_file,
      const std::string& include,
      const std::vector<std::string>& headers)
    {
      std::set<std::string> matches;
      const auto relative =
        (std::filesystem::path { including_file }.parent_path() / include)
          .lexically_normal()
          .generic_string();

      if (std::binary_search(headers.begin(), headers.end(), relative))
        matches.insert(relative);

      if (std::binary_search(headers.begin(), headers.end(), include))
        matches.insert(include);

      const auto suffix = '/' + include;

      for (const auto& header : headers)
      {
        if (header.size() > suffix.size() && header.ends_with(suffix))
          matches.insert(header);
      }

      if (matches.size() == 1)
        return *matches.begin();

      return std::nullopt;
    }

    std::set<std::string> reachable_local_headers(
      const std::filesystem::path& project_directory,
      const std::string& source,
      const std::vector<std::string>& headers)
    {
      std::set<std::string> reachable;
      std::vector<std::string> pending { source };

      while (!pending.empty())
      {
        auto file = std::move(pending.back());
        pending.pop_back();

        for (const auto& included : included_headers(project_directory / file))
        {
          const auto& include = included.path;
          const auto resolved = resolve_local_header(file, include, headers);

          if (resolved && reachable.insert(*resolved).second)
            pending.push_back(*resolved);
        }
      }

      return reachable;
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

    bool looks_like_runtime_file_access(std::string_view line)
    {
      constexpr std::array markers {
        std::string_view { "read_file(" },
        std::string_view { "ifstream" },
        std::string_view { "fstream" },
        std::string_view { "fopen(" },
        std::string_view { "freopen(" },
        std::string_view { "::load(" },
        std::string_view { "->load(" },
        std::string_view { ".load(" },
        std::string_view { "::load_" },
        std::string_view { "->load_" },
        std::string_view { ".load_" }
      };

      return std::ranges::any_of(
        markers,
        [line](std::string_view marker)
        {
          return line.find(marker) != std::string_view::npos;
        }
      );
    }

    std::vector<RuntimeFile> infer_runtime_files(
      const std::filesystem::path& project_directory,
      const std::vector<std::string>& target_sources,
      const std::vector<std::string>& headers)
    {
      std::vector<std::string> scanned = target_sources;

      for (const auto& source : target_sources)
      {
        const auto reachable = reachable_local_headers(project_directory, source, headers);
        scanned.insert(scanned.end(), reachable.begin(), reachable.end());

        for (const auto& header : headers)
        {
          if (std::filesystem::path { header }.parent_path()
              == std::filesystem::path { source }.parent_path())
          {
            scanned.push_back(header);
          }
        }

        std::error_code directory_error;
        const auto source_directory =
          project_directory / std::filesystem::path { source }.parent_path();

        for (const auto& entry : std::filesystem::directory_iterator {
          source_directory,
          directory_error
        })
        {
          if (!directory_error && entry.is_regular_file() && is_cpp_header(entry.path()))
            scanned.push_back(entry.path().lexically_relative(project_directory).generic_string());
        }
      }

      std::ranges::sort(scanned);
      scanned.erase(std::unique(scanned.begin(), scanned.end()), scanned.end());
      std::vector<std::filesystem::path> candidates;
      std::error_code filesystem_error;
      std::filesystem::recursive_directory_iterator iterator {
        project_directory,
        std::filesystem::directory_options::skip_permission_denied,
        filesystem_error
      };
      const std::filesystem::recursive_directory_iterator end;

      while (!filesystem_error && iterator != end)
      {
        const auto& entry = *iterator;

        if (entry.is_directory(filesystem_error) && is_ignored_directory(entry.path()))
          iterator.disable_recursion_pending();
        else if (!filesystem_error
                 && entry.is_regular_file(filesystem_error)
                 && !is_cpp_source(entry.path())
                 && !is_cpp_header(entry.path()))
        {
          candidates.push_back(entry.path().lexically_relative(project_directory));
        }

        iterator.increment(filesystem_error);
      }

      std::vector<RuntimeFile> inferred;
      std::set<std::filesystem::path> destinations;
      const std::regex literal { R"forge("([^"\r\n]+)")forge" };

      for (const auto& scanned_file : scanned)
      {
        std::ifstream file { project_directory / scanned_file };
        std::string line;

        while (std::getline(file, line))
        {
          if (!looks_like_runtime_file_access(line))
            continue;

          for (auto match = std::sregex_iterator { line.begin(), line.end(), literal };
               match != std::sregex_iterator {};
               ++match)
          {
            const auto expected = std::filesystem::path { (*match)[1].str() };

            if (expected.empty() || expected.is_absolute() || expected.string().starts_with(".."))
              continue;

            std::vector<std::filesystem::path> matches;
            const auto adjacent =
              (std::filesystem::path { scanned_file }.parent_path() / expected).lexically_normal();

            if (std::ranges::find(candidates, adjacent) != candidates.end())
              matches.push_back(adjacent);

            if (matches.empty())
            {
              for (const auto& candidate : candidates)
              {
                if (candidate == expected
                    || (expected.parent_path().empty() && candidate.filename() == expected))
                {
                  matches.push_back(candidate);
                }
              }
            }

            if (matches.size() == 1 && destinations.insert(expected).second)
            {
              inferred.push_back({ matches.front(), expected });
            }
          }
        }
      }

      std::ranges::sort(
        inferred,
        {},
        [](const RuntimeFile& runtime_file)
        {
          return runtime_file.destination.generic_string();
        }
      );
      return inferred;
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
        std::pair { std::string_view { "linux_libraries" }, &project.linux_libraries },
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
      error << "forge: '" << recipe_path.string() << "' already exists\n";
      return 2;
    }

    if (filesystem_error)
    {
      error << "forge: could not inspect '" << recipe_path.string() << "'\n";
      return 2;
    }

    std::vector<std::string> sources;
    std::vector<std::string> public_headers;
    std::vector<std::string> headers;
    std::vector<std::string> entry_points;
    std::optional<VisualStudioProject> visual_studio_project;
    std::vector<std::string> merged_project_formats;

    if (show_progress)
      report_progress(output, 2, 6, "Scanning sources and headers");

    if (!discover_sources(project_directory, sources, public_headers, headers, entry_points, error))
      return 2;

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

    if (visual_studio_project)
    {
      if (!visual_studio_project->sources.empty())
        sources = visual_studio_project->sources;

      if (!visual_studio_project->headers.empty())
        headers = visual_studio_project->headers;

      public_headers.clear();
      entry_points.clear();

      for (const auto& header : headers)
      {
        const auto path = std::filesystem::path { header };

        if (!path.empty() && path.begin()->string() == "include")
          public_headers.push_back(header);
      }

      for (const auto& source : sources)
      {
        if (contains_main_function(project_directory / source))
          entry_points.push_back(source);
      }
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
    const std::size_t dependency_progress_total = verify_git_dependencies ? 7 : 6;

    if (show_progress)
      report_subprogress(output, 1, dependency_progress_total, "Inferring include directories");

    auto include_directories = infer_include_directories(project_directory, sources, headers);

    if (visual_studio_project)
    {
      include_directories.insert(
        include_directories.end(),
        visual_studio_project->include_directories.begin(),
        visual_studio_project->include_directories.end()
      );
      std::ranges::sort(include_directories);
      include_directories.erase(
        std::unique(include_directories.begin(), include_directories.end()),
        include_directories.end()
      );
    }

    if (show_progress)
      report_subprogress(output, 2, dependency_progress_total, "Scanning unresolved includes");

    auto unresolved = unresolved_includes(project_directory, sources, headers);

    if (show_progress)
      report_subprogress(output, 3, dependency_progress_total, "Matching sibling Forge projects");

    auto sibling_dependencies = infer_sibling_dependencies(project_directory, unresolved);

    if (show_progress)
      report_subprogress(output, 4, dependency_progress_total, "Reading project references");

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
      report_subprogress(output, 5, dependency_progress_total, "Preparing GitHub suggestions");

    const auto suggestions = github_suggestions(project_directory, unresolved);

    if (show_progress && verify_git_dependencies)
      report_subprogress(output, 6, dependency_progress_total, "Verifying GitHub candidates");

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
    const auto metadata_version =
      visual_studio_project && !visual_studio_project->version.empty()
        ? std::optional<std::string> { visual_studio_project->version }
        : std::optional<std::string> {};
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
          "sources = " + format_sources(library_sources) + "\n";

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
          "sources = " + format_sources(inferred_target_sources[index]) + "\n";

        const auto runtime_files =
          infer_runtime_files(project_directory, inferred_target_sources[index], runtime_headers);

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
      recipe
        += "type = \"" + std::string { type } + "\"\n"
        "cpp_std = " + std::to_string(
          visual_studio_project ? visual_studio_project->cpp_standard : 20
        ) + "\n"
        "\n"
        "[sources]\n"
        "paths = " + formatted_sources + "\n";

      if (!public_headers.empty())
        recipe += "public_headers = " + formatted_headers + "\n";

      if (!include_directories.empty())
        recipe += "include_dirs = " + formatted_include_directories + "\n";

      if (initial_build_number
          || (visual_studio_project
              && (!visual_studio_project->definitions.empty()
                  || !format_system_links(*visual_studio_project).empty())))
      {
        recipe += "\n[build]\n";

        if (initial_build_number)
          recipe += "number = " + std::to_string(*initial_build_number) + "\n";

        if (visual_studio_project && !visual_studio_project->definitions.empty())
          recipe += "defines = " + format_sources(visual_studio_project->definitions) + "\n";

        if (visual_studio_project)
          recipe += format_system_links(*visual_studio_project);
      }

      if (type == "executable")
      {
        const auto runtime_files = infer_runtime_files(project_directory, sources, runtime_headers);

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

    if (!sibling_dependencies.empty() || !github_dependencies.empty())
    {
      recipe += "\n[dependencies]\n";

      for (const auto& dependency : sibling_dependencies)
      {
        recipe += dependency.name + " = { path = \""
          + escape_toml_string(dependency.path) + "\" }\n";
      }

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
        += "\n# TODO: Replace local dependencies with reproducible workflow dependencies.\n"
        "# [profile.workflow-release.dependencies]\n";

      for (const auto& dependency : sibling_dependencies)
      {
        recipe += "# " + dependency.name
          + " = { github = \"owner/" + dependency.name
          + "\", version = \"<published-version>\" }\n";
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
        recipe += "\n[profile." + escape_toml_string(name) + ".build]\n"
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

    output
      << "Adopted project '" << project_name << "'\n"
      << "Created " << recipe_path.string() << '\n'
      << "Found " << sources.size() << " C++ source file";

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

    if (entry_points.empty() && !sources.empty() && public_headers.empty())
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
