#include "init.h"

#include "github.h"
#include "recipe.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <iterator>
#include <set>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace forge
{
  namespace
  {

    std::string escape_toml_string(std::string_view value)
    {
      std::string escaped;
      escaped.reserve(value.size());

      for (const char character : value)
      {
        if (character == '\\' || character == '"')
        {
          escaped += '\\';
        }

        escaped += character;
      }

      return escaped;
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
          {
            ++index;
          }
          else if (character == quote)
          {
            quote = '\0';
          }

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
        {
          return true;
        }
      }

      return false;
    }

    bool discover_sources(const std::filesystem::path& project_directory,
                          std::vector<std::string>& sources,
                          std::vector<std::string>& public_headers,
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
        {
          iterator.disable_recursion_pending();
        }
        else if (!filesystem_error && entry.is_regular_file(filesystem_error) && is_cpp_source(entry.path()))
        {
          const auto relative = entry.path().lexically_relative(project_directory).generic_string();
          sources.push_back(relative);

          if (contains_main_function(entry.path()))
          {
            entry_points.push_back(relative);
          }
        }
        else if (!filesystem_error
                 && entry.is_regular_file(filesystem_error)
                 && is_cpp_header(entry.path())
                 && entry.path().lexically_relative(project_directory).begin()->string() == "include")
        {
          public_headers.push_back(entry.path().lexically_relative(project_directory).generic_string());
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
      std::ranges::sort(entry_points);
      return true;
    }

    std::string format_sources(const std::vector<std::string>& sources)
    {
      if (sources.empty())
      {
        return "[]";
      }

      std::string formatted = "[";

      for (std::size_t index = 0; index < sources.size(); ++index)
      {
        if (index != 0)
        {
          formatted += ", ";
        }

        formatted += '"' + escape_toml_string(sources[index]) + '"';
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

    bool write_file(const std::filesystem::path& path,
                    std::string_view contents,
                    std::ostream& error)
    {
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

  } // namespace

  int init_project(const std::filesystem::path& project_directory,
                   std::ostream& output,
                   std::ostream& error)
  {
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
    std::vector<std::string> entry_points;

    if (!discover_sources(project_directory, sources, public_headers, entry_points, error))
    {
      return 2;
    }

    const auto project_name = project_directory.filename().string();
    const auto escaped_project_name = escape_toml_string(project_name);
    const auto formatted_sources = format_sources(sources);
    const auto formatted_headers = format_sources(public_headers);
    std::string recipe =
      "#:schema " + std::string { recipe_schema_url } + "\n"
      "\n"
      "[project]\n"
      "name = \"" + escaped_project_name + "\"\n"
      "version = \"0.1.0\"\n";

    if (entry_points.size() > 1)
    {
      std::set<std::string> target_names;

      for (std::size_t index = 0; index < entry_points.size(); ++index)
      {
        auto name = target_name(entry_points[index], index);

        if (!target_names.insert(name).second)
        {
          name += '-' + std::to_string(index + 1);
          target_names.insert(name);
        }

        auto target_sources = sources;
        target_sources.erase(
          std::remove_if(
            target_sources.begin(),
            target_sources.end(),
            [&entry_points, index](const std::string& source)
            {
              return source != entry_points[index]
                && std::find(entry_points.begin(), entry_points.end(), source) != entry_points.end();
            }
          ),
          target_sources.end()
        );
        recipe
          += "\n[target." + name + "]\n"
          "type = \"executable\"\n"
          "cpp_std = 20\n"
          "sources = " + format_sources(target_sources) + "\n";

        if (!public_headers.empty())
        {
          recipe += "public_headers = " + formatted_headers + "\n";
        }
      }
    }
    else
    {
      const auto type =
        sources.empty() && !public_headers.empty()
          ? "header_only"
          : entry_points.empty() && !sources.empty() && !public_headers.empty()
          ? "static_library"
          : "executable";
      recipe
        += "type = \"" + std::string { type } + "\"\n"
        "cpp_std = 20\n"
        "\n"
        "[sources]\n"
        "paths = " + formatted_sources + "\n";

      if (!public_headers.empty())
      {
        recipe += "public_headers = " + formatted_headers + "\n";
      }
    }

    if (!write_file(recipe_path, recipe, error))
    {
      return 2;
    }

    if (!generate_github_release_support(project_directory, error))
    {
      return 2;
    }

    output
      << "Created " << recipe_path.string() << '\n'
      << "Found " << sources.size() << " C++ source file";

    if (sources.size() != 1)
    {
      output << 's';
    }

    output << '\n'
           << "Found " << entry_points.size() << " main() entry point";

    if (entry_points.size() != 1)
    {
      output << 's';
    }

    output << '\n';

    if (entry_points.empty() && !sources.empty() && public_headers.empty())
    {
      output << "Could not infer a library interface; generated an executable recipe\n";
    }

    return 0;
  }

} // namespace forge
