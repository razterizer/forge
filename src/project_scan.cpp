#include "project_scan.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <fstream>
#include <iterator>
#include <map>
#include <ostream>
#include <regex>
#include <set>
#include <sstream>
#include <string_view>
#include <system_error>

namespace forge
{

  bool is_ignored_project_scan_directory(const std::filesystem::path& path)
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

  bool scan_project(const std::filesystem::path& project_directory,
                    ProjectScan& scan,
                    std::ostream& error)
  {
    scan = {};

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

      if (entry.is_directory(filesystem_error)
          && is_ignored_project_scan_directory(entry.path()))
      {
        iterator.disable_recursion_pending();
      }
      else if (!filesystem_error
               && entry.is_regular_file(filesystem_error)
               && is_cpp_source(entry.path()))
      {
        const auto relative = entry.path().lexically_relative(project_directory).generic_string();
        scan.sources.push_back(relative);

        if (contains_main_function(entry.path()))
          scan.entry_points.push_back(relative);
      }
      else if (!filesystem_error
               && entry.is_regular_file(filesystem_error)
               && is_cpp_header(entry.path()))
      {
        const auto relative = entry.path().lexically_relative(project_directory);
        scan.headers.push_back(relative.generic_string());

        if (relative.begin() != relative.end() && relative.begin()->string() == "include")
          scan.public_headers.push_back(relative.generic_string());
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

    std::ranges::sort(scan.sources);
    std::ranges::sort(scan.public_headers);
    std::ranges::sort(scan.headers);
    std::ranges::sort(scan.entry_points);
    return true;
  }

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

  std::optional<std::string> resolve_local_header(const std::string& including_file,
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

  std::set<std::string> reachable_local_headers(const std::filesystem::path& project_directory,
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

  namespace
  {

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

  } // namespace

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

      if (entry.is_directory(filesystem_error)
          && is_ignored_project_scan_directory(entry.path()))
      {
        iterator.disable_recursion_pending();
      }
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
            inferred.push_back({ matches.front(), expected });
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

  namespace
  {

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
          origin = content.find("remote \"origin\"") != std::string_view::npos;
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
          path = std::string_view { url }.substr(github + 11);
        else if (const auto github = url.find("github.com:"); github != std::string::npos)
          path = std::string_view { url }.substr(github + 11);

        const auto slash = path.find('/');

        if (slash != std::string_view::npos && slash != 0)
          return std::string { path.substr(0, slash) };
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

  } // namespace

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
          matches[include].push_back(SiblingDependency { sibling.name, relative.generic_string() });
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

} // namespace forge
