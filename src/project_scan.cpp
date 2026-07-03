#include "project_scan.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <iterator>
#include <ostream>
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

} // namespace forge
