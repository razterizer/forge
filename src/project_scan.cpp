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

    std::optional<std::string> github_repository(const std::filesystem::path& project_directory)
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

        auto repository = std::string { path };

        if (repository.ends_with(".git"))
          repository.resize(repository.size() - 4);

        while (!repository.empty()
               && (repository.back() == '/' || std::isspace(static_cast<unsigned char>(repository.back()))))
        {
          repository.pop_back();
        }

        const auto slash = repository.find('/');

        if (slash != std::string::npos && slash != 0 && slash + 1 < repository.size())
          return repository;
      }

      return std::nullopt;
    }

    std::optional<std::string> github_owner(const std::filesystem::path& project_directory)
    {
      const auto repository = github_repository(project_directory);

      if (!repository)
        return std::nullopt;

      return repository->substr(0, repository->find('/'));
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

    std::string github_search_term(std::string_view include)
    {
      return github_repository_name(include);
    }

    std::vector<std::string> parse_github_repositories(std::string_view json)
    {
      std::vector<std::string> repositories;
      std::set<std::string> seen;
      const std::regex pattern { R"json("full_name"[[:space:]]*:[[:space:]]*"([^"]+/[^"]+)")json" };
      const auto json_begin = json.data();
      const auto json_end = json.data() + json.size();
      const auto begin = std::cregex_iterator { json_begin, json_end, pattern };
      const auto end = std::cregex_iterator {};

      for (auto match = begin; match != end && repositories.size() != 5; ++match)
      {
        auto repository = (*match)[1].str();

        if (seen.insert(repository).second)
          repositories.push_back(std::move(repository));
      }

      return repositories;
    }

    bool is_generic_github_search_term(std::string_view term)
    {
      return term == "Core"
        || term == "core"
        || term == "Common"
        || term == "common"
        || term == "Base"
        || term == "base";
    }

    bool download_github_search(const std::filesystem::path& project_directory,
                                std::string_view term,
                                const ProcessRunner& process_runner,
                                std::filesystem::path& destination,
                                std::ostream& error)
    {
      const auto cache_directory = project_directory / ".forge" / "cache" / "github-search";
      std::error_code filesystem_error;
      std::filesystem::create_directories(cache_directory, filesystem_error);

      if (filesystem_error)
        return false;

      destination = cache_directory / (std::string { term } + ".json");
      auto status_path = destination;
      status_path += ".status";
      const auto script = cache_directory / "github-search.cmake";
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

      const auto query = std::string { term } + "+in:name&per_page=5";
      const auto result = process_runner(
        {
          "cmake",
          "-DURL=https://api.github.com/search/repositories?q=" + query,
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

      if (!status_file || !(status_file >> status))
        return false;

      std::filesystem::remove(status_path, filesystem_error);
      return status == 0;
    }

    std::filesystem::path absolute_include_directory(
      const std::filesystem::path& project_directory,
      const std::filesystem::path& include_directory)
    {
      return include_directory.is_absolute()
        ? include_directory
        : project_directory / include_directory;
    }

    std::optional<std::filesystem::path> find_dependency_metadata_root(
      const std::filesystem::path& project_directory,
      const std::filesystem::path& path)
    {
      std::error_code filesystem_error;
      const auto project_root = std::filesystem::weakly_canonical(
        project_directory,
        filesystem_error
      );
      filesystem_error.clear();
      auto current = std::filesystem::is_directory(path, filesystem_error)
        ? path
        : path.parent_path();

      while (!current.empty())
      {
        if (!project_root.empty()
            && std::filesystem::equivalent(current, project_root, filesystem_error))
        {
          filesystem_error.clear();
          break;
        }

        if (std::filesystem::is_regular_file(current / "forge.recipe.toml", filesystem_error)
            || std::filesystem::is_directory(current / ".git", filesystem_error))
        {
          return current;
        }

        const auto parent = current.parent_path();

        if (parent == current)
          break;

        current = parent;
      }

      return std::nullopt;
    }

    IncludeDependencyEvidence include_dependency_evidence(
      const std::filesystem::path& project_directory,
      const std::string& include,
      const std::string& source,
      const std::filesystem::path& include_directory,
      const std::filesystem::path& header)
    {
      IncludeDependencyEvidence evidence {
        include,
        source,
        include_directory,
        header,
        {},
        {},
        {},
        {},
        false
      };

      const auto root = find_dependency_metadata_root(project_directory, header);

      if (!root)
        return evidence;

      evidence.root = *root;
      evidence.github = github_repository(*root).value_or(std::string {});

      if (std::filesystem::is_regular_file(*root / "forge.recipe.toml"))
      {
        Recipe recipe;
        std::ostringstream ignored_error;

        if (read_recipe(*root / "forge.recipe.toml", recipe, ignored_error))
        {
          evidence.name = recipe.name;
          evidence.version = recipe.version;
          evidence.forge_recipe = true;
        }
      }

      return evidence;
    }

    void add_dependency_search_root(std::vector<std::filesystem::path>& roots,
                                    const std::filesystem::path& root)
    {
      if (root.empty())
        return;

      std::error_code filesystem_error;
      const auto canonical = std::filesystem::weakly_canonical(root, filesystem_error);

      if (filesystem_error)
        return;

      if (!std::filesystem::is_directory(canonical, filesystem_error))
        return;

      if (std::ranges::find(roots, canonical) == roots.end())
        roots.push_back(canonical);
    }

    std::vector<std::filesystem::path> dependency_search_roots(
      const std::filesystem::path& project_directory)
    {
      std::vector<std::filesystem::path> roots;
      auto ancestor = project_directory.parent_path();

      for (int depth = 0; depth != 4 && !ancestor.empty(); ++depth)
      {
        if (depth == 0)
          add_dependency_search_root(roots, ancestor);

        add_dependency_search_root(roots, ancestor / "lib");
        ancestor = ancestor.parent_path();
      }

      return roots;
    }

    void collect_dependency_candidates(const std::filesystem::path& root,
                                       const std::filesystem::path& project_directory,
                                       std::vector<std::filesystem::path>& candidates)
    {
      std::error_code filesystem_error;

      const auto add_candidate = [&](const std::filesystem::path& candidate)
      {
        std::error_code candidate_error;

        if (std::filesystem::equivalent(candidate, project_directory, candidate_error)
            || !std::filesystem::is_regular_file(
              candidate / "forge.recipe.toml",
              candidate_error
            ))
        {
          return;
        }

        const auto canonical = std::filesystem::weakly_canonical(candidate, candidate_error);

        if (!candidate_error && std::ranges::find(candidates, canonical) == candidates.end())
          candidates.push_back(canonical);
      };

      add_candidate(root);

      for (const auto& entry : std::filesystem::directory_iterator { root, filesystem_error })
      {
        if (filesystem_error)
          break;

        if (entry.is_directory(filesystem_error))
          add_candidate(entry.path());

        filesystem_error.clear();
      }
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
    std::map<std::string, std::string>& unresolved,
    LocalDependencySearch local_search)
  {
    std::map<std::string, std::vector<SiblingDependency>> matches;
    std::vector<std::filesystem::path> candidates;

    if (local_search == LocalDependencySearch::off)
      return {};

    for (const auto& root : dependency_search_roots(project_directory))
      collect_dependency_candidates(root, project_directory, candidates);

    for (const auto& candidate : candidates)
    {
      Recipe sibling;
      std::ostringstream ignored_error;

      if (!read_recipe(candidate / "forge.recipe.toml", sibling, ignored_error))
        continue;

      std::error_code filesystem_error;
      const auto relative =
        std::filesystem::relative(candidate, project_directory, filesystem_error);

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
            relative.generic_string(),
            sibling.version,
            github_repository(candidate).value_or(std::string {})
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

  std::vector<IncludeDependencyEvidence> infer_include_dependency_evidence(
    const std::filesystem::path& project_directory,
    std::map<std::string, std::string>& unresolved,
    const std::vector<std::string>& include_directories,
    const std::vector<std::string>& sources,
    const std::vector<std::string>& headers)
  {
    std::vector<IncludeDependencyEvidence> result;
    std::map<std::string, std::string> candidates = unresolved;
    std::vector<std::string> scanned_files = sources;
    scanned_files.insert(scanned_files.end(), headers.begin(), headers.end());

    for (const auto& scanned_file : scanned_files)
    {
      for (const auto& included : included_headers(project_directory / scanned_file))
      {
        if (looks_like_dependency_include(included.path))
          candidates.try_emplace(included.path, scanned_file);
      }
    }

    for (const auto& [include, source] : candidates)
    {
      std::vector<IncludeDependencyEvidence> matches;
      const auto unresolved_entry = unresolved.find(include);
      const auto is_unresolved = unresolved_entry != unresolved.end();

      for (const auto& include_directory : include_directories)
      {
        const auto directory = std::filesystem::path { include_directory };
        const auto absolute_directory = absolute_include_directory(project_directory, directory);
        std::error_code filesystem_error;
        const auto header = std::filesystem::weakly_canonical(
          absolute_directory / include,
          filesystem_error
        );

        if (filesystem_error || !std::filesystem::is_regular_file(header, filesystem_error))
          continue;

        matches.push_back(
          include_dependency_evidence(
            project_directory,
            include,
            source,
            directory,
            header
          )
        );
      }

      if (!is_unresolved)
      {
        std::erase_if(
          matches,
          [](const IncludeDependencyEvidence& evidence)
          {
            return evidence.root.empty();
          }
        );
      }

      if (matches.empty())
      {
        continue;
      }

      result.insert(result.end(), matches.begin(), matches.end());
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

  std::map<std::string, std::vector<std::string>> github_search_candidates(
    const std::filesystem::path& project_directory,
    const std::map<std::string, std::string>& unresolved,
    const ProcessRunner& process_runner,
    std::ostream& error)
  {
    std::map<std::string, std::vector<std::string>> candidates;
    std::map<std::string, std::vector<std::string>> includes_by_term;

    for (const auto& [include, source] : unresolved)
    {
      const auto term = github_search_term(include);

      if (!term.empty())
        includes_by_term[term].push_back(include);
    }

    for (const auto& [term, includes] : includes_by_term)
    {
      if (is_generic_github_search_term(term))
        continue;

      std::filesystem::path response_path;

      if (!download_github_search(project_directory, term, process_runner, response_path, error))
        continue;

      std::ifstream response { response_path };
      const auto repositories = parse_github_repositories(
        std::string {
          std::istreambuf_iterator<char> { response },
          std::istreambuf_iterator<char> {}
        }
      );

      const auto owner = github_owner(project_directory);
      const auto same_owner_repository = owner
        ? std::optional { *owner + "/" + term }
        : std::nullopt;

      if (same_owner_repository
          && std::ranges::find(repositories, *same_owner_repository) != repositories.end())
      {
        continue;
      }

      for (const auto& repository : repositories)
      {
        auto& repository_includes = candidates[repository];
        repository_includes.insert(repository_includes.end(), includes.begin(), includes.end());
      }
    }

    return candidates;
  }

} // namespace forge
