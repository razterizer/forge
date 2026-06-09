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

    bool parse_boolean(std::string_view value, bool& result)
    {
      value = trim(value);

      if (value == "true")
      {
        result = true;
        return true;
      }

      if (value == "false")
      {
        result = false;
        return true;
      }

      return false;
    }

    bool is_safe_name(std::string_view value)
    {
      return
        !value.empty()
        && value != "."
        && value != ".."
        && value.find('/') == std::string_view::npos
        && value.find('\\') == std::string_view::npos;
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

    bool parse_names(std::string_view value, std::vector<std::string>& names)
    {
      std::vector<std::filesystem::path> paths;

      if (!parse_sources(value, paths))
      {
        return false;
      }

      names.clear();

      for (const auto& path : paths)
      {
        const auto name = path.string();

        if (!is_safe_name(name))
        {
          return false;
        }

        names.push_back(name);
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
        else if (kind == "git" && dependency.git.empty())
        {
          dependency.git = parsed_value;
        }
        else if (kind == "commit" && dependency.commit.empty())
        {
          dependency.commit = parsed_value;
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
      const auto exact_commit =
        (dependency.commit.size() == 40 || dependency.commit.size() == 64)
        && std::all_of(
          dependency.commit.begin(),
          dependency.commit.end(),
          [](unsigned char character)
          {
            return std::isxdigit(character);
          }
        );
      return fields > 0
        && ((local_sources == 1
             && dependency.url.empty()
             && dependency.sha256.empty()
             && dependency.github.empty()
             && dependency.version.empty()
             && dependency.git.empty()
             && dependency.commit.empty())
            || (local_sources == 0
                && !dependency.url.empty()
                && !dependency.sha256.empty()
                && dependency.github.empty()
                && dependency.version.empty()
                && dependency.git.empty()
                && dependency.commit.empty())
            || (local_sources == 0
                && dependency.url.empty()
                && dependency.sha256.empty()
                && !dependency.github.empty()
                && !dependency.version.empty()
                && dependency.git.empty()
                && dependency.commit.empty())
            || (local_sources == 0
                && dependency.url.empty()
                && dependency.sha256.empty()
                && dependency.github.empty()
                && dependency.version.empty()
                && !dependency.git.empty()
                && exact_commit));
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
      else if (section == "sources" && key == "include_dirs")
      {
        valid = parse_sources(value, recipe.include_directories);
      }
      else if (section.starts_with("target."))
      {
        const auto name = section.substr(std::string_view { "target." }.size());
        auto target = std::find_if(
          recipe.targets.begin(),
          recipe.targets.end(),
          [&name](const RecipeTarget& candidate)
          {
            return candidate.name == name;
          }
        );

        if (target == recipe.targets.end())
        {
          RecipeTarget declared;
          declared.name = name;
          recipe.targets.push_back(std::move(declared));
          target = std::prev(recipe.targets.end());
        }

        if (key == "type")
        {
          valid = parse_string(value, target->type);

          if (target->type == "shared_library")
          {
            target->type = "dynamic_library";
          }
        }
        else if (key == "cpp_std")
        {
          valid = parse_integer(value, target->cpp_standard);
        }
        else if (key == "sources")
        {
          valid = parse_sources(value, target->sources);
        }
        else if (key == "public_headers")
        {
          valid = parse_sources(value, target->public_headers);
        }
        else if (key == "include_dirs")
        {
          valid = parse_sources(value, target->include_directories);
        }
        else if (key == "runtime_files")
        {
          valid = parse_sources(value, target->runtime_files);
        }
        else if (key == "dependencies")
        {
          valid = parse_names(value, target->dependencies);
        }
        else if (key == "test")
        {
          valid = parse_boolean(value, target->test);
        }
        else
        {
          valid = false;
        }
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
        else if (key == "compiler")
        {
          valid = parse_string(value, profile->compiler);
        }
        else if (key == "compiler_version")
        {
          valid = parse_string(value, profile->compiler_version);
        }
        else if (key == "configuration")
        {
          valid = parse_string(value, profile->configuration);
        }
        else if (key == "runtime")
        {
          valid = parse_string(value, profile->runtime);
        }
        else if (key == "cpp_std")
        {
          valid = parse_integer(value, profile->cpp_standard);
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
      else if (section == "dependencies" || section.starts_with("profile."))
      {
        const auto profile_prefix = std::string_view { "profile." };
        const auto profile_suffix = std::string_view { ".dependencies" };
        const auto is_profile =
          section.starts_with(profile_prefix)
          && section.ends_with(profile_suffix)
          && section.size() > profile_prefix.size() + profile_suffix.size();
        Dependency dependency;
        dependency.name = std::string { key };
        valid =
          (section == "dependencies" || is_profile)
          && !dependency.name.empty()
          && parse_dependency(value, dependency);

        if (valid)
        {
          if (is_profile)
          {
            const auto profile = section.substr(
              profile_prefix.size(),
              section.size() - profile_prefix.size() - profile_suffix.size()
            );
            valid = is_safe_name(profile);

            if (valid)
            {
              recipe.dependency_profiles[profile].push_back(std::move(dependency));
            }
          }
          else
          {
            recipe.dependencies.push_back(std::move(dependency));
          }
        }
      }
      else if (section == "runtime" && key == "files")
      {
        valid = parse_sources(value, recipe.runtime_files);
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

    if (recipe.name.empty() || recipe.version.empty())
    {
      error << "forge: recipe is missing required project fields\n";
      return false;
    }

    const auto legacy_target = !recipe.type.empty() || recipe.cpp_standard != 0
      || !recipe.sources.empty() || !recipe.public_headers.empty()
      || !recipe.include_directories.empty() || !recipe.runtime_files.empty();

    if ((legacy_target && !recipe.targets.empty())
        || (recipe.targets.empty()
            && (recipe.type.empty()
                || (recipe.type != "imported_library" && recipe.cpp_standard == 0))))
    {
      error << "forge: recipe must declare either one legacy project target or named targets\n";
      return false;
    }

    for (const auto& target : recipe.targets)
    {
      if (!is_safe_name(target.name)
          || target.type.empty()
          || (target.type != "imported_library" && target.cpp_standard == 0))
      {
        error << "forge: named target is missing required fields\n";
        return false;
      }
    }

    return true;
  }

  bool select_recipe_target(Recipe& recipe,
                            const std::optional<std::string>& requested,
                            std::ostream& error)
  {
    if (recipe.targets.empty())
    {
      if (requested && *requested != recipe.name)
      {
        error << "forge: recipe has no target named '" << *requested << "'\n";
        return false;
      }

      return true;
    }

    if (!requested && recipe.targets.size() != 1)
    {
      error << "forge: recipe contains multiple targets; specify one of:";

      for (const auto& target : recipe.targets)
      {
        error << ' ' << target.name;
      }

      error << '\n';
      return false;
    }

    const auto selected = requested
      ? std::find_if(
          recipe.targets.begin(),
          recipe.targets.end(),
          [&requested](const RecipeTarget& candidate)
          {
            return candidate.name == *requested;
          }
        )
      : recipe.targets.begin();

    if (selected == recipe.targets.end())
    {
      error << "forge: recipe has no target named '" << *requested << "'\n";
      return false;
    }

    std::vector<std::string> active;
    std::vector<std::string> resolved;

    const auto resolve = [&](const auto& self, const RecipeTarget& target) -> bool
    {
      if (std::find(active.begin(), active.end(), target.name) != active.end())
      {
        error << "forge: internal target dependency cycle detected at '" << target.name << "'\n";
        return false;
      }

      if (std::find(resolved.begin(), resolved.end(), target.name) != resolved.end())
      {
        return true;
      }

      active.push_back(target.name);

      for (const auto& dependency_name : target.dependencies)
      {
        const auto dependency = std::find_if(
          recipe.targets.begin(),
          recipe.targets.end(),
          [&dependency_name](const RecipeTarget& candidate)
          {
            return candidate.name == dependency_name;
          }
        );

        if (dependency == recipe.targets.end())
        {
          error << "forge: target '" << target.name << "' depends on missing internal target '"
                << dependency_name << "'\n";
          return false;
        }

        if (dependency->type == "executable")
        {
          error << "forge: target '" << target.name << "' cannot depend on executable target '"
                << dependency_name << "'\n";
          return false;
        }

        if (!self(self, *dependency))
        {
          return false;
        }
      }

      active.pop_back();
      resolved.push_back(target.name);

      if (target.name != selected->name)
      {
        recipe.internal_targets.push_back(target);
      }

      return true;
    };

    if (!resolve(resolve, *selected))
    {
      return false;
    }

    recipe.selected_target = selected->name;
    recipe.selected_internal_dependencies = selected->dependencies;
    recipe.name = selected->name;
    recipe.type = selected->type;
    recipe.cpp_standard = selected->cpp_standard;
    recipe.sources = selected->sources;
    recipe.public_headers = selected->public_headers;
    recipe.include_directories = selected->include_directories;
    recipe.runtime_files = selected->runtime_files;
    return true;
  }

  bool select_dependency_profile(Recipe& recipe,
                                 const std::optional<std::string>& requested,
                                 bool required,
                                 std::ostream& error)
  {
    if (!requested)
    {
      return true;
    }

    const auto profile = recipe.dependency_profiles.find(*requested);

    if (profile == recipe.dependency_profiles.end())
    {
      if (required)
      {
        error << "forge: recipe has no dependency profile named '" << *requested << "'\n";
        return false;
      }

      return true;
    }

    recipe.dependencies = profile->second;
    return true;
  }

} // namespace forge
