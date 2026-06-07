#include "recipe.h"

#include <algorithm>
#include <charconv>
#include <fstream>
#include <iterator>
#include <string_view>

namespace forge
{
  namespace
  {

    std::string_view trim(std::string_view value)
    {
      const auto first = value.find_first_not_of(" \t\r\n");

      if (first == std::string_view::npos)
      {
        return {};
      }

      const auto last = value.find_last_not_of(" \t\r\n");
      return value.substr(first, last - first + 1);
    }

    bool parse_string(std::string_view value, std::string& result)
    {
      value = trim(value);

      if (value.size() < 2 || value.front() != '"' || value.back() != '"')
      {
        return false;
      }

      result.clear();

      for (std::size_t index = 1; index + 1 < value.size(); ++index)
      {
        if (value[index] == '\\')
        {
          ++index;

          if (index + 1 >= value.size())
          {
            return false;
          }
        }

        result += value[index];
      }

      return true;
    }

    bool parse_integer(std::string_view value, int& result)
    {
      value = trim(value);
      const auto parse_result = std::from_chars(value.data(), value.data() + value.size(), result);
      return parse_result.ec == std::errc {} && parse_result.ptr == value.data() + value.size();
    }

    bool parse_sources(std::string_view value, std::vector<std::filesystem::path>& sources)
    {
      value = trim(value);

      if (value.size() < 2 || value.front() != '[' || value.back() != ']')
      {
        return false;
      }

      value = trim(value.substr(1, value.size() - 2));
      sources.clear();

      while (!value.empty())
      {
        if (value.front() != '"')
        {
          return false;
        }

        std::size_t end = 1;

        while (end < value.size() && value[end] != '"')
        {
          if (value[end] == '\\')
          {
            ++end;
          }

          ++end;
        }

        if (end >= value.size())
        {
          return false;
        }

        std::string source;

        if (!parse_string(value.substr(0, end + 1), source))
        {
          return false;
        }

        sources.emplace_back(source);
        value = trim(value.substr(end + 1));

        if (value.empty())
        {
          break;
        }

        if (value.front() != ',')
        {
          return false;
        }

        value = trim(value.substr(1));
      }

      return true;
    }

    bool parse_dependency(std::string_view value, Dependency& dependency)
    {
      value = trim(value);

      if (value.size() < 2 || value.front() != '{' || value.back() != '}')
      {
        return false;
      }

      value = trim(value.substr(1, value.size() - 2));
      std::size_t fields = 0;

      while (!value.empty())
      {
        const auto equals = value.find('=');

        if (equals == std::string_view::npos)
        {
          return false;
        }

        const auto kind = trim(value.substr(0, equals));
        value = trim(value.substr(equals + 1));

        if (value.empty() || value.front() != '"')
        {
          return false;
        }

        std::size_t end = 1;

        while (end < value.size() && value[end] != '"')
        {
          if (value[end] == '\\')
          {
            ++end;
          }

          ++end;
        }

        if (end >= value.size())
        {
          return false;
        }

        std::string parsed_value;

        if (!parse_string(value.substr(0, end + 1), parsed_value))
        {
          return false;
        }

        if (kind == "path" && dependency.path.empty())
        {
          dependency.path = parsed_value;
        }
        else if (kind == "box" && dependency.box.empty())
        {
          dependency.box = parsed_value;
        }
        else if (kind == "url" && dependency.url.empty())
        {
          dependency.url = parsed_value;
        }
        else if (kind == "sha256" && dependency.sha256.empty())
        {
          dependency.sha256 = parsed_value;
        }
        else if (kind == "github" && dependency.github.empty())
        {
          dependency.github = parsed_value;
        }
        else if (kind == "version" && dependency.version.empty())
        {
          dependency.version = parsed_value;
        }
        else
        {
          return false;
        }

        ++fields;
        value = trim(value.substr(end + 1));

        if (value.empty())
        {
          break;
        }

        if (value.front() != ',')
        {
          return false;
        }

        value = trim(value.substr(1));
      }

      const auto local_sources = !dependency.path.empty() + !dependency.box.empty();
      return fields > 0
        && ((local_sources == 1
             && dependency.url.empty()
             && dependency.sha256.empty()
             && dependency.github.empty()
             && dependency.version.empty())
            || (local_sources == 0
                && !dependency.url.empty()
                && !dependency.sha256.empty()
                && dependency.github.empty()
                && dependency.version.empty())
            || (local_sources == 0
                && dependency.url.empty()
                && dependency.sha256.empty()
                && !dependency.github.empty()
                && !dependency.version.empty()));
    }

  } // namespace

  bool read_recipe(const std::filesystem::path& path,
                   Recipe& recipe,
                   std::ostream& error)
  {
    std::ifstream file { path };

    if (!file)
    {
      error << "forge: could not open '" << path.string() << "'\n";
      return false;
    }

    std::string section;
    std::string line;
    std::size_t line_number = 0;

    while (std::getline(file, line))
    {
      ++line_number;
      auto content = trim(line);

      if (content.empty() || content.front() == '#')
      {
        continue;
      }

      if (content.front() == '[' && content.back() == ']')
      {
        section = std::string { trim(content.substr(1, content.size() - 2)) };
        continue;
      }

      const auto equals = content.find('=');

      if (equals == std::string_view::npos)
      {
        error << "forge: invalid recipe line " << line_number << '\n';
        return false;
      }

      const auto key = trim(content.substr(0, equals));
      const auto value = trim(content.substr(equals + 1));
      bool valid = true;

      if (section == "project" && key == "name")
      {
        valid = parse_string(value, recipe.name);
      }
      else if (section == "project" && key == "version")
      {
        valid = parse_string(value, recipe.version);
      }
      else if (section == "project" && key == "type")
      {
        valid = parse_string(value, recipe.type);

        if (recipe.type == "shared_library")
        {
          recipe.type = "dynamic_library";
        }
      }
      else if (section == "project" && key == "cpp_std")
      {
        valid = parse_integer(value, recipe.cpp_standard);
      }
      else if (section == "build" && key == "number")
      {
        int build_number = 0;
        valid = parse_integer(value, build_number) && build_number >= 0;

        if (valid)
        {
          recipe.build_number = build_number;
        }
      }
      else if (section == "sources" && key == "paths")
      {
        valid = parse_sources(value, recipe.sources);
      }
      else if (section == "sources" && key == "public_headers")
      {
        valid = parse_sources(value, recipe.public_headers);
      }
      else if (section.starts_with("import."))
      {
        const auto target = section.substr(std::string_view { "import." }.size());
        auto profile = std::find_if(
          recipe.imports.begin(),
          recipe.imports.end(),
          [&target](const ImportProfile& candidate)
          {
            return candidate.target == target;
          }
        );

        if (profile == recipe.imports.end())
        {
          ImportProfile imported;
          imported.target = target;
          recipe.imports.push_back(std::move(imported));
          profile = std::prev(recipe.imports.end());
        }

        if (key == "public_headers")
        {
          valid = parse_sources(value, profile->public_headers);
        }
        else if (key == "static_libraries")
        {
          valid = parse_sources(value, profile->static_libraries);
        }
        else if (key == "dynamic_libraries")
        {
          valid = parse_sources(value, profile->dynamic_libraries);
        }
        else if (key == "import_libraries")
        {
          valid = parse_sources(value, profile->import_libraries);
        }
        else
        {
          valid = false;
        }
      }
      else if (section == "dependencies")
      {
        Dependency dependency;
        dependency.name = std::string { key };
        valid = !dependency.name.empty() && parse_dependency(value, dependency);

        if (valid)
        {
          recipe.dependencies.push_back(std::move(dependency));
        }
      }
      else if (section == "release" && key == "files")
      {
        valid = parse_sources(value, recipe.release_files);
      }

      if (!valid)
      {
        error << "forge: invalid recipe value on line " << line_number << '\n';
        return false;
      }
    }

    if (recipe.name.empty()
        || recipe.version.empty()
        || recipe.type.empty()
        || (recipe.type != "imported_library" && recipe.cpp_standard == 0))
    {
      error << "forge: recipe is missing required project fields\n";
      return false;
    }

    return true;
  }

} // namespace forge
