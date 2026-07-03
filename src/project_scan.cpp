#include "project_scan.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <fstream>
#include <iterator>
#include <ostream>
#include <regex>
#include <set>
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

} // namespace forge
