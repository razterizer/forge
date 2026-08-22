#include "recipe.h"
#include "file_support.h"
#include "toml_support.h"
#include <algorithm>
#include <cctype>
#include <charconv>
#include <fstream>
#include <iterator>
#include <set>
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
      return parse_toml_string(value, result);
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

    bool parse_sources(std::string_view value, std::vector<std::filesystem::path>& sources);
    bool parse_definitions(std::string_view value, std::vector<std::string>& definitions);

    bool is_selector_argument(std::string_view value)
    {
      return value == "style"
        || value == "platform"
        || value == "config"
        || value == "profile";
    }

    bool is_dependency_style(std::string_view value)
    {
      return value == "local-source"
        || value == "git-source"
        || value == "local-package"
        || value == "url-package"
        || value == "github-package";
    }

    bool parse_selector_path(std::string_view value, RecipeSelectors& selectors)
    {
      selectors.clear();

      while (!value.empty())
      {
        const auto argument_end = value.find('.');

        if (argument_end == std::string_view::npos)
          return false;

        const auto argument = value.substr(0, argument_end);
        value.remove_prefix(argument_end + 1);
        const auto value_end = value.find('.');
        const auto selected = value.substr(0, value_end);

        if (!is_selector_argument(argument)
            || !is_safe_name(selected)
            || selectors.contains(std::string { argument })
            || (argument == "style" && selected != "-" && !is_dependency_style(selected)))
        {
          return false;
        }

        selectors.emplace(argument, selected);

        if (value_end == std::string_view::npos)
          value = {};
        else
          value.remove_prefix(value_end + 1);
      }

      return !selectors.empty();
    }

    bool parse_build_setting(std::string_view key,
                             std::string_view value,
                             BuildProfile& build)
    {
      if (key == "configuration")
        return parse_string(value, build.configuration);
      if (key == "cpp_std")
        return parse_integer(value, build.cpp_standard);
      if (key == "c_std")
        return parse_integer(value, build.c_standard);
      if (key == "include_dirs")
        return parse_sources(value, build.include_directories);
      if (key == "macos_system_include_dirs")
        return parse_sources(value, build.macos_system_include_directories);
      if (key == "linux_system_include_dirs")
        return parse_sources(value, build.linux_system_include_directories);
      if (key == "windows_system_include_dirs")
        return parse_sources(value, build.windows_system_include_directories);
      if (key == "macos_system_library_dirs")
        return parse_sources(value, build.macos_system_library_directories);
      if (key == "linux_system_library_dirs")
        return parse_sources(value, build.linux_system_library_directories);
      if (key == "windows_system_library_dirs")
        return parse_sources(value, build.windows_system_library_directories);
      if (key == "defines")
        return parse_definitions(value, build.compile_definitions);

      return false;
    }

    bool dependency_matches_style(const Dependency& dependency, std::string_view style)
    {
      if (style == "local-source")
        return !dependency.path.empty() && dependency.git.empty() && dependency.box.empty()
          && dependency.url.empty();
      if (style == "git-source")
        return !dependency.git.empty() && !dependency.commit.empty();
      if (style == "local-package")
        return !dependency.box.empty();
      if (style == "url-package")
        return !dependency.url.empty() && !dependency.sha256.empty();
      if (style == "github-package")
        return !dependency.github.empty() && !dependency.version.empty();

      return true;
    }

    bool parse_sources(std::string_view value, std::vector<std::filesystem::path>& sources)
    {
      value = trim(value);

      if (value.size() < 2 || value.front() != '[' || value.back() != ']')
        return false;

      value = trim(value.substr(1, value.size() - 2));
      sources.clear();

      while (!value.empty())
      {
        std::string source;
        std::size_t consumed = 0;

        if (!parse_toml_string_prefix(value, source, consumed))
          return false;

        sources.emplace_back(source);
        value = trim(value.substr(consumed));

        if (value.empty())
          break;

        if (value.front() != ',')
          return false;

        value = trim(value.substr(1));
      }

      return true;
    }

    bool parse_runtime_mapping(std::string_view value, RuntimeFile& runtime_file)
    {
      value = trim(value);

      if (value.size() < 2 || value.front() != '{' || value.back() != '}')
        return false;

      value = trim(value.substr(1, value.size() - 2));
      bool has_source = false;
      bool has_destination = false;

      while (!value.empty())
      {
        const auto equals = value.find('=');

        if (equals == std::string_view::npos)
          return false;

        const auto key = trim(value.substr(0, equals));
        value = trim(value.substr(equals + 1));

        if (value.empty() || value.front() != '"')
          return false;

        std::size_t end = 1;

        while (end < value.size() && value[end] != '"')
        {
          if (value[end] == '\\')
            ++end;

          ++end;
        }

        std::string parsed;

        if (end >= value.size() || !parse_string(value.substr(0, end + 1), parsed))
          return false;

        if (key == "source" && !has_source)
        {
          runtime_file.source = parsed;
          has_source = true;
        }
        else if (key == "destination" && !has_destination)
        {
          runtime_file.destination = parsed;
          has_destination = true;
        }
        else
          return false;

        value = trim(value.substr(end + 1));

        if (value.empty())
          break;

        if (value.front() != ',')
          return false;

        value = trim(value.substr(1));
      }

      return has_source && has_destination;
    }

    bool parse_runtime_files(std::string_view value, std::vector<RuntimeFile>& runtime_files)
    {
      value = trim(value);

      if (value.size() < 2 || value.front() != '[' || value.back() != ']')
        return false;

      value = trim(value.substr(1, value.size() - 2));
      runtime_files.clear();

      while (!value.empty())
      {
        RuntimeFile runtime_file;
        std::size_t end = std::string_view::npos;

        if (value.front() == '"')
        {
          end = 1;

          while (end < value.size() && value[end] != '"')
          {
            if (value[end] == '\\')
              ++end;

            ++end;
          }

          std::string path;

          if (end >= value.size() || !parse_string(value.substr(0, end + 1), path))
            return false;

          runtime_file.source = path;
          runtime_file.destination = path;
        }
        else if (value.front() == '{')
        {
          end = value.find('}');

          if (end == std::string_view::npos
              || !parse_runtime_mapping(value.substr(0, end + 1), runtime_file))
          {
            return false;
          }
        }
        else
          return false;

        runtime_files.push_back(std::move(runtime_file));
        value = trim(value.substr(end + 1));

        if (value.empty())
          break;

        if (value.front() != ',')
          return false;

        value = trim(value.substr(1));
      }

      return true;
    }

    bool parse_release_variant(std::string_view value, ReleaseVariant& variant)
    {
      value = trim(value);

      if (value.size() < 2 || value.front() != '{' || value.back() != '}')
        return false;

      value = trim(value.substr(1, value.size() - 2));
      bool has_profile = false;
      bool has_suffix = false;

      while (!value.empty())
      {
        const auto equals = value.find('=');

        if (equals == std::string_view::npos)
          return false;

        const auto key = trim(value.substr(0, equals));
        value = trim(value.substr(equals + 1));

        if (value.empty() || value.front() != '"')
          return false;

        std::size_t end = 1;

        while (end < value.size() && value[end] != '"')
        {
          if (value[end] == '\\')
            ++end;

          ++end;
        }

        std::string parsed;

        if (end >= value.size() || !parse_string(value.substr(0, end + 1), parsed))
          return false;

        if (key == "profile" && !has_profile)
        {
          variant.profile = std::move(parsed);
          has_profile = true;
        }
        else if (key == "suffix" && !has_suffix)
        {
          variant.suffix = std::move(parsed);
          has_suffix = true;
        }
        else
          return false;

        value = trim(value.substr(end + 1));

        if (value.empty())
          break;

        if (value.front() != ',')
          return false;

        value = trim(value.substr(1));
      }

      if (!has_profile || variant.profile.empty())
        return false;

      if (!has_suffix)
        variant.suffix = variant.profile;

      return is_safe_name(variant.profile) && is_safe_name(variant.suffix);
    }

    bool parse_release_variants(std::string_view value, std::vector<ReleaseVariant>& variants)
    {
      value = trim(value);

      if (value.size() < 2 || value.front() != '[' || value.back() != ']')
        return false;

      value = trim(value.substr(1, value.size() - 2));
      variants.clear();

      while (!value.empty())
      {
        if (value.front() != '{')
          return false;

        const auto end = value.find('}');

        if (end == std::string_view::npos)
          return false;

        ReleaseVariant variant;

        if (!parse_release_variant(value.substr(0, end + 1), variant))
          return false;

        variants.push_back(std::move(variant));
        value = trim(value.substr(end + 1));

        if (value.empty())
          break;

        if (value.front() != ',')
          return false;

        value = trim(value.substr(1));
      }

      return !variants.empty();
    }

    bool parse_platform_files(std::string_view value, PlatformReleaseFiles& files)
    {
      value = trim(value);

      if (value.size() < 2 || value.front() != '{' || value.back() != '}')
        return false;

      value = trim(value.substr(1, value.size() - 2));
      bool saw_entry = false;

      while (!value.empty())
      {
        const auto equals = value.find('=');

        if (equals == std::string_view::npos)
          return false;

        const auto key = trim(value.substr(0, equals));
        value = trim(value.substr(equals + 1));

        if (value.empty() || value.front() != '"')
          return false;

        std::size_t end = 1;

        while (end < value.size() && value[end] != '"')
        {
          if (value[end] == '\\')
            ++end;

          ++end;
        }

        std::string parsed;

        if (end >= value.size() || !parse_string(value.substr(0, end + 1), parsed))
          return false;

        if (key == "linux" && files.linux_path.empty())
          files.linux_path = parsed;
        else if (key == "macos" && files.macos_path.empty())
          files.macos_path = parsed;
        else if (key == "windows" && files.windows_path.empty())
          files.windows_path = parsed;
        else
          return false;

        saw_entry = true;
        value = trim(value.substr(end + 1));

        if (value.empty())
          break;

        if (value.front() != ',')
          return false;

        value = trim(value.substr(1));
      }

      return saw_entry;
    }

    bool parse_names(std::string_view value, std::vector<std::string>& names)
    {
      std::vector<std::filesystem::path> paths;

      if (!parse_sources(value, paths))
        return false;

      names.clear();

      for (const auto& path : paths)
      {
        const auto name = path.string();

        if (!is_safe_name(name))
          return false;

        names.push_back(name);
      }

      return true;
    }

    bool parse_link_names(std::string_view value, std::vector<std::string>& names)
    {
      if (!parse_names(value, names))
        return false;

      return std::ranges::all_of(
        names,
        [](const std::string& name)
        {
          return std::ranges::all_of(
            name,
            [](unsigned char character)
            {
              return std::isalnum(character)
                || character == '_'
                || character == '-'
                || character == '.'
                || character == '+';
            }
          );
        }
      );
    }

    bool is_reserved_system_profile(std::string_view value)
    {
      return value == workflow_release_profile
        || value == "local-debug"
        || value == "local-release"
        || value == "git-debug"
        || value == "git-release"
        || value == "github-cbox-debug"
        || value == "github-cbox-release";
    }

    bool parse_package_names(std::string_view value, std::vector<std::string>& names)
    {
      std::vector<std::filesystem::path> paths;

      if (!parse_sources(value, paths))
        return false;

      names.clear();

      for (const auto& path : paths)
      {
        const auto name = path.generic_string();

        if (name.empty()
            || name == "."
            || name == ".."
            || name.front() == '/'
            || name.find('\\') != std::string::npos
            || !std::ranges::all_of(
                 name,
                 [](unsigned char character)
                 {
                   return std::isalnum(character)
                     || character == '_'
                     || character == '-'
                     || character == '.'
                     || character == '+'
                     || character == '@'
                     || character == '/';
                 }
               ))
        {
          return false;
        }

        names.push_back(name);
      }

      return true;
    }

    bool parse_definitions(std::string_view value, std::vector<std::string>& definitions)
    {
      std::vector<std::filesystem::path> paths;

      if (!parse_sources(value, paths))
        return false;

      definitions.clear();

      for (const auto& path : paths)
        definitions.push_back(path.string());

      return std::all_of(
        definitions.begin(),
        definitions.end(),
        is_valid_compile_definition
      );
    }

    bool parse_dependency(std::string_view value, Dependency& dependency)
    {
      value = trim(value);

      if (value.size() < 2 || value.front() != '{' || value.back() != '}')
        return false;

      value = trim(value.substr(1, value.size() - 2));
      std::size_t fields = 0;

      while (!value.empty())
      {
        const auto equals = value.find('=');

        if (equals == std::string_view::npos)
          return false;

        const auto kind = trim(value.substr(0, equals));
        value = trim(value.substr(equals + 1));

        if ((kind == "targets" || kind == "provides")
            && (kind == "targets" ? dependency.targets.empty() : dependency.provides.empty()))
        {
          const auto end = value.find(']');

          auto& names = kind == "targets" ? dependency.targets : dependency.provides;

          if (end == std::string_view::npos
              || !parse_link_names(value.substr(0, end + 1), names))
          {
            return false;
          }

          ++fields;
          value = trim(value.substr(end + 1));

          if (value.empty())
            break;

          if (value.front() != ',')
            return false;

          value = trim(value.substr(1));
          continue;
        }

        if (value.empty() || value.front() != '"')
          return false;

        std::size_t end = 1;

        while (end < value.size() && value[end] != '"')
        {
          if (value[end] == '\\')
            ++end;

          ++end;
        }

        if (end >= value.size())
          return false;

        std::string parsed_value;

        if (!parse_string(value.substr(0, end + 1), parsed_value))
          return false;

        if (kind == "path" && dependency.path.empty())
          dependency.path = parsed_value;
        else if (kind == "box" && dependency.box.empty())
          dependency.box = parsed_value;
        else if (kind == "url" && dependency.url.empty())
          dependency.url = parsed_value;
        else if (kind == "sha256" && dependency.sha256.empty())
          dependency.sha256 = parsed_value;
        else if (kind == "github" && dependency.github.empty())
          dependency.github = parsed_value;
        else if (kind == "package" && dependency.package.empty())
          dependency.package = parsed_value;
        else if (kind == "version" && dependency.version.empty())
          dependency.version = parsed_value;
        else if (kind == "git" && dependency.git.empty())
          dependency.git = parsed_value;
        else if (kind == "commit" && dependency.commit.empty())
          dependency.commit = parsed_value;
        else if (kind == "component" && dependency.component.empty())
          dependency.component = parsed_value;
        else if (kind == "variant" && dependency.variant.empty())
          dependency.variant = parsed_value;
        else
          return false;

        ++fields;
        value = trim(value.substr(end + 1));

        if (value.empty())
          break;

        if (value.front() != ',')
          return false;

        value = trim(value.substr(1));
      }

      const auto local_sources = !dependency.path.empty() + !dependency.box.empty();
      const auto has_github_package =
        !dependency.github.empty() && !dependency.version.empty();
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
             && dependency.package.empty()
             && dependency.variant.empty()
             && dependency.version.empty()
             && dependency.git.empty()
             && dependency.commit.empty())
            || (!dependency.path.empty()
                && dependency.box.empty()
                && dependency.url.empty()
                && dependency.sha256.empty()
                && has_github_package
                && dependency.git.empty()
                && dependency.commit.empty())
            || (local_sources == 0
                && !dependency.url.empty()
                && !dependency.sha256.empty()
                && dependency.github.empty()
                && dependency.package.empty()
                && dependency.variant.empty()
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
                && dependency.package.empty()
                && dependency.variant.empty()
                && dependency.version.empty()
                && !dependency.git.empty()
                && exact_commit));
    }

  } // namespace

  bool is_valid_compile_definition(std::string_view definition)
  {
    const auto equals = definition.find('=');
    const auto name = definition.substr(0, equals);

    if (name.empty()
        || (!std::isalpha(static_cast<unsigned char>(name.front())) && name.front() != '_')
        || definition.find(';') != std::string_view::npos
        || definition.find('\n') != std::string_view::npos
        || definition.find('\r') != std::string_view::npos)
    {
      return false;
    }

    return std::all_of(
      name.begin() + 1,
      name.end(),
      [](unsigned char character)
      {
        return std::isalnum(character) || character == '_';
      }
    );
  }

  bool read_recipe(const std::filesystem::path& path,
                   Recipe& result,
                   std::ostream& error)
  {
    std::ifstream file { path };

    if (!file)
    {
      error << "forge: could not open '" << path.string() << "'\n";
      return false;
    }

    Recipe recipe;
    std::string section;
    std::vector<TomlStatement> statements;
    std::size_t invalid_line = 0;

    if (!read_toml_statements(file, statements, invalid_line))
    {
      error << "forge: invalid recipe line " << invalid_line << '\n';
      return false;
    }

    for (const auto& statement : statements)
    {
      const auto content = trim(statement.content);
      const auto line_number = statement.line;

      if (content.front() == '[' && content.back() == ']')
      {
        section = std::string { trim(content.substr(1, content.size() - 2)) };

        if (section.empty())
        {
          error << "forge: invalid recipe section on line " << line_number << '\n';
          return false;
        }

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

      if (key.empty())
      {
        error << "forge: invalid recipe key on line " << line_number << '\n';
        return false;
      }

      bool valid = true;

      if (section == "project" && key == "name")
        valid = parse_string(value, recipe.name);
      else if (section == "project" && key == "version")
        valid = parse_string(value, recipe.version);
      else if (section == "project" && key == "type")
      {
        valid = parse_string(value, recipe.type);

        if (recipe.type == "shared_library")
          recipe.type = "dynamic_library";
      }
      else if (section == "project" && key == "cpp_std")
        valid = parse_integer(value, recipe.cpp_standard);
      else if (section == "project" && key == "c_std")
        valid = parse_integer(value, recipe.c_standard);
      else if (section == "defaults" && key == "style")
      {
        std::string style;
        valid = parse_string(value, style) && is_dependency_style(style);

        if (valid)
          recipe.default_style = std::move(style);
      }
      else if (section == "defaults" && key == "profile")
      {
        std::string profile;
        valid = parse_string(value, profile)
          && is_safe_name(profile)
          && profile != "-"
          && profile != "*";

        if (valid)
          recipe.default_profile = std::move(profile);
      }
      else if (section == "defaults" && key == "target")
      {
        std::string target;
        valid = parse_string(value, target) && is_safe_name(target);

        if (valid)
          recipe.default_target = std::move(target);
      }
      else if (section == "build" && key == "number")
      {
        int build_number = 0;
        valid = parse_integer(value, build_number) && build_number >= 0;

        if (valid)
          recipe.build_number = build_number;
      }
      else if (section == "build" && key == "python_extension")
        valid = parse_boolean(value, recipe.python_extension);
      else if (section == "build" && key == "python_extension_name")
        valid = parse_string(value, recipe.python_extension_name) && is_safe_name(recipe.python_extension_name);
      else if (section == "build" && key == "python_extension_output_dir")
      {
        std::string output_directory;
        valid = parse_string(value, output_directory)
          && is_safe_project_path(output_directory);

        if (valid)
          recipe.python_extension_output_directory = output_directory;
      }
      else if (section == "build" && key == "python_extension_with_soabi")
        valid = parse_boolean(value, recipe.python_extension_with_soabi);
      else if (section == "build" && key == "defines")
        valid = parse_definitions(value, recipe.compile_definitions);
      else if (section == "build" && key == "public_defines")
        valid = parse_definitions(value, recipe.public_compile_definitions);
      else if (section == "build" && key == "macos_system_include_dirs")
        valid = parse_sources(value, recipe.macos_system_include_directories);
      else if (section == "build" && key == "linux_system_include_dirs")
        valid = parse_sources(value, recipe.linux_system_include_directories);
      else if (section == "build" && key == "windows_system_include_dirs")
        valid = parse_sources(value, recipe.windows_system_include_directories);
      else if (section == "build" && key == "macos_system_library_dirs")
        valid = parse_sources(value, recipe.macos_system_library_directories);
      else if (section == "build" && key == "linux_system_library_dirs")
        valid = parse_sources(value, recipe.linux_system_library_directories);
      else if (section == "build" && key == "windows_system_library_dirs")
        valid = parse_sources(value, recipe.windows_system_library_directories);
      else if (section == "build" && key == "macos_frameworks")
        valid = parse_link_names(value, recipe.macos_frameworks);
      else if (section == "build" && key == "macos_libraries")
        valid = parse_link_names(value, recipe.macos_libraries);
      else if (section == "build" && key == "macos_brew_packages")
        valid = parse_package_names(value, recipe.macos_brew_packages);
      else if (section == "build" && key == "linux_libraries")
        valid = parse_link_names(value, recipe.linux_libraries);
      else if (section == "build" && key == "linux_apt_packages")
        valid = parse_package_names(value, recipe.linux_apt_packages);
      else if (section == "build" && key == "windows_libraries")
        valid = parse_link_names(value, recipe.windows_libraries);
      else if (section == "sources" && key == "paths")
        valid = parse_sources(value, recipe.sources);
      else if (section == "sources" && key == "public_headers")
        valid = parse_sources(value, recipe.public_headers);
      else if (section == "sources" && key == "validation_headers")
        valid = parse_sources(value, recipe.header_validation_headers);
      else if (section == "sources" && key == "include_dirs")
        valid = parse_sources(value, recipe.include_directories);
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
            target->type = "dynamic_library";
        }
        else if (key == "cpp_std")
          valid = parse_integer(value, target->cpp_standard);
        else if (key == "c_std")
          valid = parse_integer(value, target->c_standard);
        else if (key == "sources")
          valid = parse_sources(value, target->sources);
        else if (key == "public_headers")
          valid = parse_sources(value, target->public_headers);
        else if (key == "include_dirs")
          valid = parse_sources(value, target->include_directories);
        else if (key == "macos_system_include_dirs")
          valid = parse_sources(value, target->macos_system_include_directories);
        else if (key == "linux_system_include_dirs")
          valid = parse_sources(value, target->linux_system_include_directories);
        else if (key == "windows_system_include_dirs")
          valid = parse_sources(value, target->windows_system_include_directories);
        else if (key == "macos_system_library_dirs")
          valid = parse_sources(value, target->macos_system_library_directories);
        else if (key == "linux_system_library_dirs")
          valid = parse_sources(value, target->linux_system_library_directories);
        else if (key == "windows_system_library_dirs")
          valid = parse_sources(value, target->windows_system_library_directories);
        else if (key == "defines")
          valid = parse_definitions(value, target->compile_definitions);
        else if (key == "public_defines")
          valid = parse_definitions(value, target->public_compile_definitions);
        else if (key == "runtime_files")
          valid = parse_runtime_files(value, target->runtime_files);
        else if (key == "dependencies")
          valid = parse_names(value, target->dependencies);
        else if (key == "macos_frameworks")
          valid = parse_link_names(value, target->macos_frameworks);
        else if (key == "macos_libraries")
          valid = parse_link_names(value, target->macos_libraries);
        else if (key == "macos_brew_packages")
          valid = parse_package_names(value, target->macos_brew_packages);
        else if (key == "linux_libraries")
          valid = parse_link_names(value, target->linux_libraries);
        else if (key == "linux_apt_packages")
          valid = parse_package_names(value, target->linux_apt_packages);
        else if (key == "windows_libraries")
          valid = parse_link_names(value, target->windows_libraries);
        else if (key == "test")
          valid = parse_boolean(value, target->test);
        else
          valid = false;
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
          valid = parse_sources(value, profile->public_headers);
        else if (key == "compiler")
          valid = parse_string(value, profile->compiler);
        else if (key == "compiler_version")
          valid = parse_string(value, profile->compiler_version);
        else if (key == "configuration")
          valid = parse_string(value, profile->configuration);
        else if (key == "runtime")
          valid = parse_string(value, profile->runtime);
        else if (key == "cpp_std")
          valid = parse_integer(value, profile->cpp_standard);
        else if (key == "static_libraries")
          valid = parse_sources(value, profile->static_libraries);
        else if (key == "dynamic_libraries")
          valid = parse_sources(value, profile->dynamic_libraries);
        else if (key == "import_libraries")
          valid = parse_sources(value, profile->import_libraries);
        else
          valid = false;
      }
      else if (section.starts_with("build."))
      {
        RecipeSelectors selectors;
        valid = parse_selector_path(
          section.substr(std::string_view { "build." }.size()),
          selectors
        );

        if (valid)
        {
          auto rule = std::ranges::find_if(
            recipe.build_rules,
            [&selectors](const BuildRule& candidate)
            {
              return candidate.selectors == selectors;
            }
          );

          if (rule == recipe.build_rules.end())
          {
            recipe.build_rules.push_back({ std::move(selectors), {} });
            rule = std::prev(recipe.build_rules.end());
          }

          valid = parse_build_setting(key, value, rule->build);
        }
      }
      else if (section.starts_with("dependencies."))
      {
        RecipeSelectors selectors;
        valid = parse_selector_path(
          section.substr(std::string_view { "dependencies." }.size()),
          selectors
        );
        Dependency dependency;
        dependency.name = std::string { key };
        valid = valid && !dependency.name.empty() && parse_dependency(value, dependency);

        if (valid)
        {
          const auto style = selectors.find("style");
          valid = style == selectors.end()
            || style->second == "-"
            || dependency_matches_style(dependency, style->second);
        }

        if (valid)
        {
          auto rule = std::ranges::find_if(
            recipe.dependency_rules,
            [&selectors](const DependencyRule& candidate)
            {
              return candidate.selectors == selectors;
            }
          );

          if (rule == recipe.dependency_rules.end())
          {
            recipe.dependency_rules.push_back({ std::move(selectors), {} });
            rule = std::prev(recipe.dependency_rules.end());
          }

          rule->dependencies.push_back(std::move(dependency));
        }
      }
      else if (section == "dependencies"
               || section.starts_with("profile.")
               || section.starts_with("sysprofile."))
      {
        const auto profile_prefix = std::string_view { "profile." };
        const auto system_profile_prefix = std::string_view { "sysprofile." };
        const auto profile_suffix = std::string_view { ".dependencies" };
        const auto build_suffix = std::string_view { ".build" };
        const auto is_profile =
          section.starts_with(profile_prefix)
          && section.ends_with(profile_suffix)
          && section.size() > profile_prefix.size() + profile_suffix.size();
        const auto is_build_profile =
          section.starts_with(profile_prefix)
          && section.ends_with(build_suffix)
          && section.size() > profile_prefix.size() + build_suffix.size();
        const auto is_system_profile =
          section.starts_with(system_profile_prefix)
          && section.ends_with(profile_suffix)
          && section.size() > system_profile_prefix.size() + profile_suffix.size();
        const auto is_system_build_profile =
          section.starts_with(system_profile_prefix)
          && section.ends_with(build_suffix)
          && section.size() > system_profile_prefix.size() + build_suffix.size();
        Dependency dependency;
        dependency.name = std::string { key };
        valid =
          (section == "dependencies"
           || is_profile
           || is_build_profile
           || is_system_profile
           || is_system_build_profile)
          && !dependency.name.empty()
          && ((is_build_profile || is_system_build_profile)
              || parse_dependency(value, dependency));

        if (valid)
        {
          if (is_build_profile || is_system_build_profile)
          {
            const auto prefix = is_system_build_profile ? system_profile_prefix : profile_prefix;
            const auto profile = section.substr(
              prefix.size(),
              section.size() - prefix.size() - build_suffix.size()
            );
            valid = is_safe_name(profile)
              && (is_system_build_profile
                  ? is_reserved_system_profile(profile)
                  : (!is_reserved_system_profile(profile) || profile == workflow_release_profile));

            if (valid)
            {
              auto& build =
                is_system_build_profile
                  ? recipe.system_build_profiles[std::string { profile }]
                  : recipe.build_profiles[std::string { profile }];

              valid = parse_build_setting(key, value, build);
            }
          }
          else if (is_profile || is_system_profile)
          {
            const auto prefix = is_system_profile ? system_profile_prefix : profile_prefix;
            const auto profile = section.substr(
              prefix.size(),
              section.size() - prefix.size() - profile_suffix.size()
            );
            valid = is_safe_name(profile)
              && (is_system_profile
                  ? is_reserved_system_profile(profile)
                  : (!is_reserved_system_profile(profile) || profile == workflow_release_profile));

            if (valid)
            {
              if (is_system_profile)
                recipe.system_dependency_profiles[profile].push_back(std::move(dependency));
              else
                recipe.dependency_profiles[profile].push_back(std::move(dependency));
            }
          }
          else
            recipe.dependencies.push_back(std::move(dependency));
        }
      }
      else if (section == "runtime" && key == "files")
        valid = parse_runtime_files(value, recipe.runtime_files);
      else if (section == "release" && key == "files")
        valid = parse_sources(value, recipe.release_files);
      else if (section == "release" && key == "bundle_name")
      {
        std::string name;
        valid = parse_string(value, name) && is_safe_name(name);

        if (valid)
          recipe.release_bundle_name = std::move(name);
      }
      else if (section == "release" && key == "variants")
        valid = parse_release_variants(value, recipe.release_variants);
      else if (section == "box" && key == "variants")
        valid = parse_release_variants(value, recipe.box_variants);
      else if (section == "release" && key == "readme")
        valid = parse_platform_files(value, recipe.release_readme);
      else if (section == "release" && key == "unblock")
        valid = parse_platform_files(value, recipe.release_unblock);
      else if (section == "release" && key == "build_number_format")
      {
        std::string format;
        valid = parse_string(value, format) && (format == "semver" || format == "dotted");

        if (valid)
          recipe.release_notes_build_number_format = std::move(format);
      }
      else if (section == "version_header" && key == "path")
      {
        std::string header_path;
        valid = parse_string(value, header_path);

        if (valid)
          recipe.version_header_path = std::move(header_path);
      }
      else if (section == "version_header" && key == "prefix")
      {
        valid = parse_string(value, recipe.version_header_prefix)
          && !recipe.version_header_prefix.empty()
          && std::ranges::all_of(
            recipe.version_header_prefix,
            [](unsigned char character)
            {
              return std::isupper(character)
                || std::isdigit(character)
                || character == '_';
            }
          )
          && !std::isdigit(static_cast<unsigned char>(recipe.version_header_prefix.front()));
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

    if (recipe.release_notes_build_number_format && !recipe.build_number)
    {
      error << "forge: release.build_number_format requires build.number\n";
      return false;
    }

    std::vector<std::string> release_variant_suffixes;

    for (const auto& variant : recipe.release_variants)
    {
      if (!is_safe_name(variant.profile)
          || !is_safe_name(variant.suffix)
          || std::find(
               release_variant_suffixes.begin(),
               release_variant_suffixes.end(),
               variant.suffix
             ) != release_variant_suffixes.end())
      {
        error << "forge: release variants require unique safe profile suffixes\n";
        return false;
      }

      release_variant_suffixes.push_back(variant.suffix);
    }

    if (recipe.version_header_path.empty() != recipe.version_header_prefix.empty())
    {
      error << "forge: version_header requires path and prefix\n";
      return false;
    }

    const auto legacy_target = !recipe.type.empty() || recipe.cpp_standard != 0 || recipe.c_standard != 0
      || !recipe.sources.empty() || !recipe.public_headers.empty()
      || !recipe.include_directories.empty() || !recipe.runtime_files.empty();

    if ((legacy_target && !recipe.targets.empty())
        || (recipe.targets.empty()
            && (recipe.type.empty()
                || (recipe.type != "imported_library"
                    && recipe.cpp_standard == 0
                    && recipe.c_standard == 0))))
    {
      error << "forge: recipe must declare either one legacy project target or named targets\n";
      return false;
    }

    for (const auto& target : recipe.targets)
    {
      if (!is_safe_name(target.name)
          || target.type.empty()
          || (target.type != "imported_library"
              && target.cpp_standard == 0
              && target.c_standard == 0))
      {
        error << "forge: named target is missing required fields\n";
        return false;
      }
    }

    result = std::move(recipe);
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

    const auto requested_target = requested ? requested : recipe.default_target;

    if (!requested_target && recipe.targets.size() != 1)
    {
      error << "forge: recipe contains multiple targets; specify one of:";

      for (const auto& target : recipe.targets)
        error << ' ' << target.name;

      error << '\n';
      return false;
    }

    const auto selected = requested_target
      ? std::find_if(
          recipe.targets.begin(),
          recipe.targets.end(),
          [&requested_target](const RecipeTarget& candidate)
          {
            return candidate.name == *requested_target;
          }
        )
      : recipe.targets.begin();

    if (selected == recipe.targets.end())
    {
      error << "forge: recipe has no target named '" << *requested_target << "'\n";
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
        return true;

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
          return false;
      }

      active.pop_back();
      resolved.push_back(target.name);

      if (target.name != selected->name)
        recipe.internal_targets.push_back(target);

      return true;
    };

    if (!resolve(resolve, *selected))
      return false;

    const auto base_include_directories = recipe.include_directories;
    const auto base_macos_system_include_directories = recipe.macos_system_include_directories;
    const auto base_linux_system_include_directories = recipe.linux_system_include_directories;
    const auto base_windows_system_include_directories = recipe.windows_system_include_directories;
    const auto base_macos_system_library_directories = recipe.macos_system_library_directories;
    const auto base_linux_system_library_directories = recipe.linux_system_library_directories;
    const auto base_windows_system_library_directories = recipe.windows_system_library_directories;
    const auto base_compile_definitions = recipe.compile_definitions;
    const auto base_public_compile_definitions = recipe.public_compile_definitions;
    const auto base_macos_frameworks = recipe.macos_frameworks;
    const auto base_macos_libraries = recipe.macos_libraries;
    const auto base_macos_brew_packages = recipe.macos_brew_packages;
    const auto base_linux_libraries = recipe.linux_libraries;
    const auto base_linux_apt_packages = recipe.linux_apt_packages;
    const auto base_windows_libraries = recipe.windows_libraries;
    const auto append =
      [](auto& values, const auto& additional)
      {
        values.insert(values.end(), additional.begin(), additional.end());
      };

    recipe.selected_target = selected->name;
    recipe.selected_internal_dependencies = selected->dependencies;
    recipe.name = selected->name;
    recipe.type = selected->type;
    recipe.cpp_standard = selected->cpp_standard;
    recipe.c_standard = selected->c_standard;
    recipe.sources = selected->sources;
    recipe.public_headers = selected->public_headers;
    recipe.header_validation_headers.clear();
    recipe.include_directories = selected->include_directories;
    recipe.macos_system_include_directories = selected->macos_system_include_directories;
    recipe.linux_system_include_directories = selected->linux_system_include_directories;
    recipe.windows_system_include_directories = selected->windows_system_include_directories;
    recipe.macos_system_library_directories = selected->macos_system_library_directories;
    recipe.linux_system_library_directories = selected->linux_system_library_directories;
    recipe.windows_system_library_directories = selected->windows_system_library_directories;
    recipe.compile_definitions = selected->compile_definitions;
    recipe.public_compile_definitions = selected->public_compile_definitions;
    recipe.macos_frameworks = selected->macos_frameworks;
    recipe.macos_libraries = selected->macos_libraries;
    recipe.macos_brew_packages = selected->macos_brew_packages;
    recipe.linux_libraries = selected->linux_libraries;
    recipe.linux_apt_packages = selected->linux_apt_packages;
    recipe.windows_libraries = selected->windows_libraries;
    recipe.runtime_files = selected->runtime_files;
    append(recipe.include_directories, base_include_directories);
    append(recipe.macos_system_include_directories, base_macos_system_include_directories);
    append(recipe.linux_system_include_directories, base_linux_system_include_directories);
    append(recipe.windows_system_include_directories, base_windows_system_include_directories);
    append(recipe.macos_system_library_directories, base_macos_system_library_directories);
    append(recipe.linux_system_library_directories, base_linux_system_library_directories);
    append(recipe.windows_system_library_directories, base_windows_system_library_directories);
    append(recipe.compile_definitions, base_compile_definitions);
    append(recipe.public_compile_definitions, base_public_compile_definitions);
    append(recipe.macos_frameworks, base_macos_frameworks);
    append(recipe.macos_libraries, base_macos_libraries);
    append(recipe.macos_brew_packages, base_macos_brew_packages);
    append(recipe.linux_libraries, base_linux_libraries);
    append(recipe.linux_apt_packages, base_linux_apt_packages);
    append(recipe.windows_libraries, base_windows_libraries);
    return true;
  }

  namespace
  {
    template <typename SelectedProfiles, typename OtherProfiles, typename Apply>
    bool select_profile(const std::optional<std::string>& requested,
                        const SelectedProfiles& selected_profiles,
                        const OtherProfiles& other_profiles,
                        bool required,
                        bool system_profile,
                        Apply&& apply,
                        std::ostream& error)
    {
      if (!requested)
        return true;

      if (system_profile && !is_reserved_system_profile(*requested))
      {
        error << "forge: unknown system profile '" << *requested << "'\n";
        return false;
      }

      const auto profile = selected_profiles.find(*requested);

      if (profile == selected_profiles.end())
      {
        if (required && !other_profiles.contains(*requested))
        {
          error << "forge: recipe has no " << (system_profile ? "system " : "")
                << "profile named '" << *requested << "'\n";
          return false;
        }

        return true;
      }

      apply(profile->second);
      return true;
    }
  }

  bool select_dependency_profile(Recipe& recipe,
                                 const std::optional<std::string>& requested,
                                 bool required,
                                 std::ostream& error)
  {
    return select_profile(
      requested,
      recipe.dependency_profiles,
      recipe.build_profiles,
      required,
      false,
      [&recipe](const auto& dependencies)
      {
        recipe.dependencies = dependencies;
      },
      error
    );
  }

  bool select_system_dependency_profile(Recipe& recipe,
                                        const std::optional<std::string>& requested,
                                        bool required,
                                        std::ostream& error)
  {
    return select_profile(
      requested,
      recipe.system_dependency_profiles,
      recipe.system_build_profiles,
      required,
      true,
      [&recipe](const auto& dependencies)
      {
        recipe.dependencies = dependencies;
      },
      error
    );
  }

  void apply_build_profile(Recipe& recipe, const BuildProfile& build, std::string& configuration)
  {
    recipe.include_directories.insert(
      recipe.include_directories.end(),
      build.include_directories.begin(),
      build.include_directories.end()
    );
    recipe.macos_system_include_directories.insert(
      recipe.macos_system_include_directories.end(),
      build.macos_system_include_directories.begin(),
      build.macos_system_include_directories.end()
    );
    recipe.linux_system_include_directories.insert(
      recipe.linux_system_include_directories.end(),
      build.linux_system_include_directories.begin(),
      build.linux_system_include_directories.end()
    );
    recipe.windows_system_include_directories.insert(
      recipe.windows_system_include_directories.end(),
      build.windows_system_include_directories.begin(),
      build.windows_system_include_directories.end()
    );
    recipe.macos_system_library_directories.insert(
      recipe.macos_system_library_directories.end(),
      build.macos_system_library_directories.begin(),
      build.macos_system_library_directories.end()
    );
    recipe.linux_system_library_directories.insert(
      recipe.linux_system_library_directories.end(),
      build.linux_system_library_directories.begin(),
      build.linux_system_library_directories.end()
    );
    recipe.windows_system_library_directories.insert(
      recipe.windows_system_library_directories.end(),
      build.windows_system_library_directories.begin(),
      build.windows_system_library_directories.end()
    );
    recipe.compile_definitions.insert(
      recipe.compile_definitions.end(),
      build.compile_definitions.begin(),
      build.compile_definitions.end()
    );

    if (build.cpp_standard != 0)
      recipe.cpp_standard = build.cpp_standard;

    if (build.c_standard != 0)
      recipe.c_standard = build.c_standard;

    if (!build.configuration.empty())
      configuration = build.configuration;
  }

  bool select_build_profile(Recipe& recipe,
                            const std::optional<std::string>& requested,
                            bool required,
                            std::string& configuration,
                            std::ostream& error)
  {
    return select_profile(
      requested,
      recipe.build_profiles,
      recipe.dependency_profiles,
      required,
      false,
      [&recipe, &configuration](const BuildProfile& build)
      {
        apply_build_profile(recipe, build, configuration);
      },
      error
    );
  }

  bool select_system_build_profile(Recipe& recipe,
                                   const std::optional<std::string>& requested,
                                   bool required,
                                   std::string& configuration,
                                   std::ostream& error)
  {
    return select_profile(
      requested,
      recipe.system_build_profiles,
      recipe.system_dependency_profiles,
      required,
      true,
      [&recipe, &configuration](const BuildProfile& build)
      {
        apply_build_profile(recipe, build, configuration);
      },
      error
    );
  }

  bool apply_selector_rules(Recipe& recipe,
                            const RecipeSelection& selection,
                            std::string& configuration,
                            std::ostream& error)
  {
    const auto selected_value = [&selection](std::string_view argument) -> std::string_view
    {
      if (argument == "style")
        return selection.style;
      if (argument == "platform")
        return selection.platform;
      if (argument == "config")
        return selection.configuration;
      if (argument == "profile")
        return selection.profile;
      return {};
    };
    const auto matches = [&selected_value](const RecipeSelectors& selectors)
    {
      return std::ranges::all_of(
        selectors,
        [&selected_value](const auto& selector)
        {
          return selector.second == "-" || selected_value(selector.first) == selector.second;
        }
      );
    };
    const auto specificity = [](const RecipeSelectors& selectors)
    {
      return std::ranges::count_if(
        selectors,
        [](const auto& selector)
        {
          return selector.second != "-";
        }
      );
    };

    std::vector<const DependencyRule*> dependency_rules;

    for (const auto& rule : recipe.dependency_rules)
    {
      if (matches(rule.selectors))
        dependency_rules.push_back(&rule);
    }

    std::ranges::stable_sort(
      dependency_rules,
      [&specificity](const DependencyRule* left, const DependencyRule* right)
      {
        return specificity(left->selectors) < specificity(right->selectors);
      }
    );

    for (const auto* rule : dependency_rules)
    {
      for (const auto& dependency : rule->dependencies)
      {
        const auto existing = std::ranges::find_if(
          recipe.dependencies,
          [&dependency](const Dependency& candidate)
          {
            return candidate.name == dependency.name;
          }
        );

        if (existing == recipe.dependencies.end())
          recipe.dependencies.push_back(dependency);
        else
          *existing = dependency;
      }
    }

    std::vector<const BuildRule*> build_rules;

    for (const auto& rule : recipe.build_rules)
    {
      if (matches(rule.selectors))
        build_rules.push_back(&rule);
    }

    std::ranges::stable_sort(
      build_rules,
      [&specificity](const BuildRule* left, const BuildRule* right)
      {
        return specificity(left->selectors) < specificity(right->selectors);
      }
    );

    for (const auto* rule : build_rules)
      apply_build_profile(recipe, rule->build, configuration);

    if (!selection.style.empty() && !is_dependency_style(selection.style))
    {
      error << "forge: unknown dependency style '" << selection.style << "'\n";
      return false;
    }

    return true;
  }

  RecipeSelection resolve_recipe_selection(
    const Recipe& recipe,
    const std::optional<std::string>& style,
    std::string platform,
    const std::optional<std::string>& configuration,
    const std::optional<std::string>& profile
  )
  {
    RecipeSelection selection;
    selection.style = style.value_or(recipe.default_style.value_or(""));

    if (selection.style.empty())
    {
      std::set<std::string> declared_styles;

      for (const auto& rule : recipe.dependency_rules)
      {
        const auto declared = rule.selectors.find("style");

        if (declared != rule.selectors.end() && declared->second != "-")
          declared_styles.insert(declared->second);
      }

      selection.style = declared_styles.size() == 1
        ? *declared_styles.begin()
        : "local-source";
    }

    selection.platform = std::move(platform);
    selection.configuration = configuration.value_or("debug");
    std::ranges::transform(
      selection.configuration,
      selection.configuration.begin(),
      [](unsigned char character)
      {
        return static_cast<char>(std::tolower(character));
      }
    );
    selection.profile = profile.value_or(recipe.default_profile.value_or(""));
    return selection;
  }

  bool resolve_effective_build_selection(
    Recipe& recipe,
    const BuildSelectionRequest& request,
    EffectiveBuildSelection& effective,
    std::ostream& error
  )
  {
    if (request.select_target && !select_recipe_target(recipe, request.target, error))
      return false;

    const auto uses_selector_rules = !recipe.build_rules.empty() || !recipe.dependency_rules.empty();
    const auto named_legacy_profile = request.profile
      && (recipe.dependency_profiles.contains(*request.profile)
          || recipe.build_profiles.contains(*request.profile));
    const auto use_legacy_profile = request.profile_resolution == ProfileResolution::automatic
      ? named_legacy_profile || (request.profile && !uses_selector_rules && !request.style)
      : request.profile_resolution == ProfileResolution::inherited_legacy && named_legacy_profile;

    effective.legacy_profile = use_legacy_profile ? request.profile : std::optional<std::string> {};

    if (!select_dependency_profile(recipe, effective.legacy_profile, request.require_profile, error)
        || !select_system_dependency_profile(
          recipe,
          request.system_profile,
          request.require_profile,
          error
        ))
    {
      return false;
    }

    effective.configuration = request.build_configuration;

    if (request.selector_configuration)
    {
      effective.configuration = *request.selector_configuration;

      if (!effective.configuration.empty())
      {
        effective.configuration.front() = static_cast<char>(
          std::toupper(static_cast<unsigned char>(effective.configuration.front()))
        );
      }
    }

    effective.selectors = resolve_recipe_selection(
      recipe,
      request.style,
      request.platform,
      request.selector_configuration,
      effective.legacy_profile ? std::optional<std::string> {} : request.profile
    );

    if (effective.legacy_profile)
      effective.selectors.profile.clear();

    if (!effective.legacy_profile
        && (uses_selector_rules || request.style)
        && !apply_selector_rules(recipe, effective.selectors, effective.configuration, error))
    {
      return false;
    }

    if (request.apply_build_profiles
        && (!select_build_profile(
              recipe,
              effective.legacy_profile,
              request.require_profile,
              effective.configuration,
              error
            )
            || !select_system_build_profile(
              recipe,
              request.system_profile,
              request.require_profile,
              effective.configuration,
              error
            )))
    {
      return false;
    }

    return true;
  }

} // namespace forge
