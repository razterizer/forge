#include "init_xcode.h"

#include "recipe.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <iterator>
#include <regex>
#include <set>
#include <string>
#include <string_view>
#include <vector>

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

      return value.substr(first, value.find_last_not_of(" \t\r\n") - first + 1);
    }

    std::vector<std::string> xcode_setting_values(std::string_view settings,
                                                   std::string_view key)
    {
      const auto position = settings.find(key);

      if (position == std::string_view::npos)
      {
        return {};
      }

      const auto equals = settings.find('=', position + key.size());

      if (equals == std::string_view::npos)
      {
        return {};
      }

      auto begin = equals + 1;

      while (begin < settings.size()
             && std::isspace(static_cast<unsigned char>(settings[begin])))
      {
        ++begin;
      }

      std::string_view value;

      if (begin < settings.size() && settings[begin] == '(')
      {
        const auto end = settings.find(");", begin + 1);

        if (end == std::string_view::npos)
        {
          return {};
        }

        value = settings.substr(begin + 1, end - begin - 1);
      }
      else
      {
        const auto end = settings.find(';', begin);

        if (end == std::string_view::npos)
        {
          return {};
        }

        value = settings.substr(begin, end - begin);
      }

      std::vector<std::string> values;
      std::string current;
      char quote = '\0';

      for (const auto character : value)
      {
        if (quote != '\0')
        {
          if (character == quote)
            quote = '\0';
          else
            current += character;
        }
        else if (character == '"' || character == '\'')
          quote = character;
        else if (character == ',' || std::isspace(static_cast<unsigned char>(character)))
        {
          if (!current.empty())
          {
            values.push_back(std::move(current));
            current.clear();
          }
        }
        else
          current += character;
      }

      if (!current.empty())
        values.push_back(std::move(current));

      return values;
    }

    std::string replace_xcode_paths(std::string value,
                                    const std::filesystem::path& project_directory)
    {
      for (const auto variable : {
        std::string_view { "$(PROJECT_DIR)" },
        std::string_view { "$(SRCROOT)" },
        std::string_view { "${PROJECT_DIR}" },
        std::string_view { "${SRCROOT}" }
      })
      {
        std::size_t position = 0;
        const auto replacement = project_directory.generic_string();

        while ((position = value.find(variable, position)) != std::string::npos)
        {
          value.replace(position, variable.size(), replacement);
          position += replacement.size();
        }
      }

      return value;
    }

    void append_xcode_settings(BuildProfile& profile,
                               std::string_view settings,
                               const std::filesystem::path& project_directory,
                               std::vector<std::string>& unresolved)
    {
      for (const auto& standard : xcode_setting_values(settings, "CLANG_CXX_LANGUAGE_STANDARD"))
      {
        const auto number =
          standard.starts_with("c++")
            ? standard.substr(3)
            : standard.starts_with("gnu++")
              ? standard.substr(5)
              : std::string {};

        if (!number.empty()
            && std::ranges::all_of(number, [](unsigned char character)
            {
              return std::isdigit(character);
            }))
        {
          profile.cpp_standard = std::stoi(number);
        }
        else if (!standard.empty())
          unresolved.push_back(standard);
      }

      for (const auto& include : xcode_setting_values(settings, "HEADER_SEARCH_PATHS"))
      {
        if (include == "$(inherited)")
          continue;

        const auto expanded = replace_xcode_paths(include, project_directory);

        if (expanded.find("$(") != std::string::npos || expanded.find("${") != std::string::npos)
          unresolved.push_back(include);
        else if (const auto relative = project_relative_path(project_directory, expanded))
          profile.include_directories.push_back(*relative);
      }

      for (const auto& definition : xcode_setting_values(settings, "GCC_PREPROCESSOR_DEFINITIONS"))
      {
        if (definition == "$(inherited)")
          continue;

        if (definition.find('$') != std::string::npos)
          unresolved.push_back(definition);
        else if (is_valid_compile_definition(definition))
          profile.compile_definitions.push_back(definition);
      }
    }


  } // namespace

  std::optional<VisualStudioProject> read_xcode_project(
      const std::filesystem::path& path,
      std::ostream& error)
    {
      const auto project_file = path / "project.pbxproj";
      std::ifstream file { project_file };

      if (!file)
      {
        error << "forge: could not open Xcode project '" << path.string() << "'\n";
        return std::nullopt;
      }

      const std::string contents {
        std::istreambuf_iterator<char> { file },
        std::istreambuf_iterator<char> {}
      };
      VisualStudioProject project;
      project.path = path;
      project.format = "Xcode";
      project.name = path.stem().string();
      const auto directory = path.parent_path();
      const std::regex target_pattern {
        R"regex(isa\s*=\s*PBXNativeTarget;[\s\S]*?name\s*=\s*"?([^";\n]+)"?;[\s\S]*?productType\s*=\s*"([^"]+)";)regex"
      };
      std::vector<std::pair<std::string, std::string>> targets;

      for (std::sregex_iterator target { contents.begin(), contents.end(), target_pattern };
           target != std::sregex_iterator {};
           ++target)
      {
        targets.emplace_back((*target)[1].str(), (*target)[2].str());
      }

      if (targets.size() == 1)
      {
        project.name = trim(targets.front().first);
        const auto& product_type = targets.front().second;
        project.type =
          product_type.find("static-library") != std::string::npos
            ? "static_library"
            : product_type.find("dynamic-library") != std::string::npos
              ? "dynamic_library"
              : product_type.find("tool") != std::string::npos
                  || product_type.find("application") != std::string::npos
                ? "executable"
                : "";
      }
      else if (targets.size() > 1)
        project.type.clear();

      const std::regex configuration_pattern {
        R"regex(isa\s*=\s*XCBuildConfiguration;[\s\S]*?buildSettings\s*=\s*\{([\s\S]*?)\};[\s\S]*?name\s*=\s*"?([^";\n]+)"?;)regex"
      };

      for (std::sregex_iterator configuration {
             contents.begin(),
             contents.end(),
             configuration_pattern
           };
           configuration != std::sregex_iterator {};
           ++configuration)
      {
        const auto configuration_name = (*configuration)[2].str();
        const auto name = std::string { trim(configuration_name) };
        auto& profile = project.profiles[name];
        profile.configuration = name;
        append_xcode_settings(
          profile,
          (*configuration)[1].str(),
          directory,
          project.unresolved_properties
        );
        sort_unique(profile);
      }

      const std::regex xcconfig_pattern { R"regex(path\s*=\s*"?([^";]+\.xcconfig)"?;)regex" };

      for (std::sregex_iterator reference { contents.begin(), contents.end(), xcconfig_pattern };
           reference != std::sregex_iterator {};
           ++reference)
      {
        const auto relative = (*reference)[1].str();
        const auto xcconfig_path = directory / relative;
        std::ifstream xcconfig_file { xcconfig_path };

        if (!xcconfig_file)
        {
          project.unresolved_properties.push_back(relative);
          continue;
        }

        std::string settings;
        std::string line;

        while (std::getline(xcconfig_file, line))
        {
          const auto content = trim(line);

          if (!content.empty() && !content.starts_with("//") && !content.starts_with('#'))
          {
            settings += std::string { content } + ";\n";
          }
        }

        auto stem = xcconfig_path.stem().string();
        std::ranges::transform(stem, stem.begin(), [](unsigned char character)
        {
          return static_cast<char>(std::tolower(character));
        });
        bool matched = false;

        for (auto& [name, profile] : project.profiles)
        {
          auto normalized_name = name;
          std::ranges::transform(normalized_name, normalized_name.begin(), [](unsigned char character)
          {
            return static_cast<char>(std::tolower(character));
          });

          if (stem.find(normalized_name) != std::string::npos)
          {
            append_xcode_settings(
              profile,
              settings,
              directory,
              project.unresolved_properties
            );
            sort_unique(profile);
            matched = true;
          }
        }

        if (!matched)
          project.unresolved_properties.push_back(relative);
      }

      if (!project.profiles.empty())
      {
        auto common_includes = project.profiles.begin()->second.include_directories;
        auto common_definitions = project.profiles.begin()->second.compile_definitions;
        auto common_standard = project.profiles.begin()->second.cpp_standard;

        for (const auto& [name, profile] : project.profiles)
        {
          std::vector<std::filesystem::path> includes;
          std::vector<std::string> definitions;
          std::set_intersection(
            common_includes.begin(),
            common_includes.end(),
            profile.include_directories.begin(),
            profile.include_directories.end(),
            std::back_inserter(includes)
          );
          std::set_intersection(
            common_definitions.begin(),
            common_definitions.end(),
            profile.compile_definitions.begin(),
            profile.compile_definitions.end(),
            std::back_inserter(definitions)
          );
          common_includes = std::move(includes);
          common_definitions = std::move(definitions);

          if (profile.cpp_standard != common_standard)
            common_standard = 0;
        }

        project.cpp_standard = common_standard == 0 ? 20 : common_standard;

        for (const auto& include : common_includes)
          project.include_directories.push_back(include.generic_string());

        project.definitions = common_definitions;

        for (auto& [name, profile] : project.profiles)
        {
          std::erase_if(profile.include_directories, [&common_includes](const auto& include)
          {
            return std::binary_search(common_includes.begin(), common_includes.end(), include);
          });
          std::erase_if(profile.compile_definitions, [&common_definitions](const auto& definition)
          {
            return std::binary_search(
              common_definitions.begin(),
              common_definitions.end(),
              definition
            );
          });
        }
      }

      std::ranges::sort(project.unresolved_properties);
      project.unresolved_properties.erase(
        std::unique(project.unresolved_properties.begin(), project.unresolved_properties.end()),
        project.unresolved_properties.end()
      );
      return project;
    }


} // namespace forge
