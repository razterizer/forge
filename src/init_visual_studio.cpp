#include "init_visual_studio.h"

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

    std::string_view trim(std::string_view value)
    {
      const auto first = value.find_first_not_of(" \t\r\n");

      if (first == std::string_view::npos)
      {
        return {};
      }

      return value.substr(first, value.find_last_not_of(" \t\r\n") - first + 1);
    }

    std::string decode_xml(std::string value)
    {
      for (const auto& [entity, character] : {
        std::pair { std::string_view { "&quot;" }, std::string_view { "\"" } },
        std::pair { std::string_view { "&apos;" }, std::string_view { "'" } },
        std::pair { std::string_view { "&lt;" }, std::string_view { "<" } },
        std::pair { std::string_view { "&gt;" }, std::string_view { ">" } },
        std::pair { std::string_view { "&amp;" }, std::string_view { "&" } }
      })
      {
        std::size_t position = 0;

        while ((position = value.find(entity, position)) != std::string::npos)
        {
          value.replace(position, entity.size(), character);
          position += character.size();
        }
      }

      return value;
    }

    std::vector<std::string> xml_values(std::string_view xml, std::string_view tag)
    {
      std::vector<std::string> values;
      const auto opening = '<' + std::string { tag };
      const auto closing = "</" + std::string { tag } + '>';
      std::size_t position = 0;

      while ((position = xml.find(opening, position)) != std::string_view::npos)
      {
        const auto open_end = xml.find('>', position + opening.size());

        if (open_end == std::string_view::npos)
          break;

        const auto close = xml.find(closing, open_end + 1);

        if (close == std::string_view::npos)
          break;

        values.push_back(
          decode_xml(std::string { trim(xml.substr(open_end + 1, close - open_end - 1)) })
        );
        position = close + closing.size();
      }

      return values;
    }

    std::vector<std::string> xml_attributes(std::string_view xml,
                                            std::string_view tag,
                                            std::string_view attribute)
    {
      std::vector<std::string> values;
      const auto opening = '<' + std::string { tag };
      const auto needle = std::string { attribute } + "=\"";
      std::size_t position = 0;

      while ((position = xml.find(opening, position)) != std::string_view::npos)
      {
        const auto open_end = xml.find('>', position + opening.size());

        if (open_end == std::string_view::npos)
          break;

        const auto attribute_position = xml.find(needle, position + opening.size());

        if (attribute_position != std::string_view::npos && attribute_position < open_end)
        {
          const auto value_begin = attribute_position + needle.size();
          const auto value_end = xml.find('"', value_begin);

          if (value_end != std::string_view::npos && value_end < open_end)
          {
            values.push_back(
              decode_xml(std::string { xml.substr(value_begin, value_end - value_begin) })
            );
          }
        }

        position = open_end + 1;
      }

      return values;
    }

    struct XmlElement
    {
      std::string opening;
      std::string body;
    };

    std::vector<XmlElement> xml_elements(std::string_view xml, std::string_view tag)
    {
      std::vector<XmlElement> elements;
      const auto opening = '<' + std::string { tag };
      const auto closing = "</" + std::string { tag } + '>';
      std::size_t position = 0;

      while ((position = xml.find(opening, position)) != std::string_view::npos)
      {
        const auto open_end = xml.find('>', position + opening.size());

        if (open_end == std::string_view::npos)
          break;

        const auto close = xml.find(closing, open_end + 1);

        if (close == std::string_view::npos)
          break;

        elements.push_back({
          std::string { xml.substr(position, open_end - position + 1) },
          std::string { xml.substr(open_end + 1, close - open_end - 1) }
        });
        position = close + closing.size();
      }

      return elements;
    }

    std::vector<std::string> xml_openings(std::string_view xml, std::string_view tag)
    {
      std::vector<std::string> openings;
      const auto opening = '<' + std::string { tag };
      std::size_t position = 0;

      while ((position = xml.find(opening, position)) != std::string_view::npos)
      {
        const auto open_end = xml.find('>', position + opening.size());

        if (open_end == std::string_view::npos)
          break;

        openings.push_back(std::string { xml.substr(position, open_end - position + 1) });
        position = open_end + 1;
      }

      return openings;
    }

    std::optional<std::string> xml_attribute(std::string_view opening,
                                             std::string_view attribute)
    {
      const auto needle = std::string { attribute } + "=\"";
      const auto position = opening.find(needle);

      if (position == std::string_view::npos)
        return std::nullopt;

      const auto begin = position + needle.size();
      const auto end = opening.find('"', begin);
      return end == std::string_view::npos
        ? std::nullopt
        : std::optional<std::string> {
            decode_xml(std::string { opening.substr(begin, end - begin) })
          };
    }

    std::optional<std::string> msbuild_configuration(std::string_view condition)
    {
      if (condition.find("$(Configuration)") == std::string_view::npos)
        return std::nullopt;

      const auto equals = condition.find("==");

      if (equals == std::string_view::npos)
        return std::nullopt;

      auto value = trim(condition.substr(equals + 2));

      if (value.size() >= 2
          && ((value.front() == '\'' && value.back() == '\'')
              || (value.front() == '"' && value.back() == '"')))
      {
        value = value.substr(1, value.size() - 2);
      }

      const auto separator = value.find('|');
      value = value.substr(0, separator);
      return value.empty() || value.find("$(") != std::string_view::npos
        ? std::nullopt
        : std::optional<std::string> { value };
    }

    std::string replace_msbuild_paths(std::string value,
                                      const std::filesystem::path& project_directory,
                                      const std::filesystem::path& document_directory)
    {
      for (const auto& [variable, path] : {
        std::pair { std::string_view { "$(ProjectDir)" }, project_directory },
        std::pair { std::string_view { "$(MSBuildThisFileDirectory)" }, document_directory }
      })
      {
        std::size_t position = 0;
        const auto replacement = path.generic_string() + '/';

        while ((position = value.find(variable, position)) != std::string::npos)
        {
          value.replace(position, variable.size(), replacement);
          position += replacement.size();
        }
      }

      return value;
    }

    struct MsBuildDocument
    {
      std::filesystem::path path;
      std::string xml;
      std::optional<std::string> configuration;
    };

    bool collect_msbuild_documents(const std::filesystem::path& path,
                                   const std::filesystem::path& project_directory,
                                   const std::optional<std::string>& configuration,
                                   std::set<std::filesystem::path>& active,
                                   std::vector<MsBuildDocument>& documents,
                                   std::vector<std::string>& unresolved,
                                   std::ostream& error)
    {
      const auto normalized = path.lexically_normal();

      if (!active.insert(normalized).second)
        return true;

      std::ifstream file { normalized };

      if (!file)
      {
        error << "forge: could not open MSBuild file '" << normalized.string() << "'\n";
        return false;
      }

      MsBuildDocument document;
      document.path = normalized;
      document.configuration = configuration;
      document.xml = {
        std::istreambuf_iterator<char> { file },
        std::istreambuf_iterator<char> {}
      };
      documents.push_back(document);

      std::multiset<std::string> grouped_imports;

      for (const auto& group : xml_elements(document.xml, "ImportGroup"))
      {
        const auto group_condition = xml_attribute(group.opening, "Condition");
        const auto group_configuration =
          group_condition && msbuild_configuration(*group_condition)
            ? msbuild_configuration(*group_condition)
            : configuration;

        for (const auto& opening : xml_openings(group.body, "Import"))
        {
          if (const auto imported = xml_attribute(opening, "Project"))
          {
            grouped_imports.insert(*imported);
            auto expanded = replace_msbuild_paths(
              *imported,
              project_directory,
              normalized.parent_path()
            );
            std::replace(expanded.begin(), expanded.end(), '\\', '/');

            if (expanded.find("$(") != std::string::npos)
            {
              unresolved.push_back(*imported);
              continue;
            }

            const auto imported_path =
              (std::filesystem::path { expanded }.is_absolute()
                ? std::filesystem::path { expanded }
                : normalized.parent_path() / expanded).lexically_normal();

            if (imported_path.extension() == ".props"
                && std::filesystem::exists(imported_path)
                && !collect_msbuild_documents(
                  imported_path,
                  project_directory,
                  group_configuration,
                  active,
                  documents,
                  unresolved,
                  error
                ))
            {
              return false;
            }
          }
        }
      }

      for (const auto& opening : xml_openings(document.xml, "Import"))
      {
        const auto imported = xml_attribute(opening, "Project");

        if (!imported)
          continue;

        if (const auto grouped = grouped_imports.find(*imported); grouped != grouped_imports.end())
        {
          grouped_imports.erase(grouped);
          continue;
        }

        const auto import_condition = xml_attribute(opening, "Condition");
        const auto import_configuration =
          import_condition && msbuild_configuration(*import_condition)
            ? msbuild_configuration(*import_condition)
            : configuration;
        auto expanded = replace_msbuild_paths(
          *imported,
          project_directory,
          normalized.parent_path()
        );
        std::replace(expanded.begin(), expanded.end(), '\\', '/');

        if (expanded.find("$(") != std::string::npos)
        {
          unresolved.push_back(*imported);
          continue;
        }

        const auto imported_path =
          (std::filesystem::path { expanded }.is_absolute()
            ? std::filesystem::path { expanded }
            : normalized.parent_path() / expanded).lexically_normal();

        if (imported_path.extension() == ".props"
            && std::filesystem::exists(imported_path)
            && !collect_msbuild_documents(
              imported_path,
              project_directory,
              import_configuration,
              active,
              documents,
              unresolved,
              error
            ))
        {
          return false;
        }
      }

      active.erase(normalized);
      return true;
    }

    std::vector<std::string> split_msbuild_list(const std::vector<std::string>& values)
    {
      std::set<std::string> result;

      for (const auto& value : values)
      {
        std::string_view remaining = value;

        while (!remaining.empty())
        {
          const auto separator = remaining.find(';');
          auto item = trim(remaining.substr(0, separator));

          if (!item.empty() && !item.starts_with("%("))
          {
            result.insert(std::string { item });
          }

          if (separator == std::string_view::npos)
            break;

          remaining.remove_prefix(separator + 1);
        }
      }

      return { result.begin(), result.end() };
    }

    int cpp_standard_from_msbuild(std::string_view standard)
    {
      if (standard == "stdcpp14")
        return 14;

      if (standard == "stdcpp17")
        return 17;

      if (standard == "stdcpp20")
        return 20;

      if (standard == "stdcpp23" || standard == "stdcpplatest")
        return 23;

      return 0;
    }

    void append_visual_studio_settings(
      BuildProfile& settings,
      const MsBuildDocument& document,
      std::string_view xml,
      const std::filesystem::path& project_directory,
      std::vector<std::string>& unresolved)
    {
      for (const auto& standard : xml_values(xml, "LanguageStandard"))
      {
        if (const auto parsed = cpp_standard_from_msbuild(standard); parsed != 0)
          settings.cpp_standard = parsed;
      }

      for (const auto& include : split_msbuild_list(xml_values(xml, "AdditionalIncludeDirectories")))
      {
        const auto expanded = replace_msbuild_paths(
          include,
          project_directory,
          document.path.parent_path()
        );

        if (expanded.find("$(") != std::string::npos)
          unresolved.push_back(include);
        else if (const auto relative = project_relative_path(project_directory, expanded))
          settings.include_directories.push_back(*relative);
      }

      for (const auto& definition : split_msbuild_list(xml_values(xml, "PreprocessorDefinitions")))
      {
        if (definition.find("$(") != std::string::npos)
          unresolved.push_back(definition);
        else if (is_valid_compile_definition(definition))
          settings.compile_definitions.push_back(definition);
      }
    }


  } // namespace

  std::optional<VisualStudioProject> read_visual_studio_project(
      const std::filesystem::path& path,
      std::ostream& error)
    {
      std::ifstream file { path };

      if (!file)
      {
        error << "forge: could not open Visual Studio project '" << path.string() << "'\n";
        return std::nullopt;
      }

      const std::string xml {
        std::istreambuf_iterator<char> { file },
        std::istreambuf_iterator<char> {}
      };
      VisualStudioProject project;
      project.path = path;
      project.name = path.stem().string();
      const auto directory = path.parent_path();
      std::set<std::filesystem::path> active;
      std::vector<MsBuildDocument> documents;

      if (!collect_msbuild_documents(
        path,
        directory,
        std::nullopt,
        active,
        documents,
        project.unresolved_properties,
        error
      ))
      {
        return std::nullopt;
      }

      for (const auto& name : xml_values(xml, "ProjectName"))
      {
        if (!name.empty())
        {
          project.name = name;
          break;
        }
      }

      for (const auto& configuration : xml_values(xml, "ConfigurationType"))
      {
        if (configuration == "Application")
        {
          project.type = "executable";
          break;
        }

        if (configuration == "StaticLibrary")
        {
          project.type = "static_library";
          break;
        }

        if (configuration == "DynamicLibrary")
        {
          project.type = "dynamic_library";
          break;
        }
      }

      std::set<std::string> configurations;

      for (const auto& configuration : xml_attributes(xml, "ProjectConfiguration", "Include"))
      {
        const auto separator = configuration.find('|');
        configurations.insert(configuration.substr(0, separator));
      }

      for (const auto& document : documents)
      {
        if (document.configuration)
          configurations.insert(*document.configuration);

        for (const auto& group : xml_elements(document.xml, "ItemDefinitionGroup"))
        {
          const auto condition = xml_attribute(group.opening, "Condition");
          const auto configuration =
            condition && msbuild_configuration(*condition)
              ? msbuild_configuration(*condition)
              : document.configuration;

          if (configuration)
            configurations.insert(*configuration);
        }
      }

      BuildProfile common;

      for (const auto& document : documents)
      {
        for (const auto& group : xml_elements(document.xml, "ItemDefinitionGroup"))
        {
          const auto condition = xml_attribute(group.opening, "Condition");
          const auto configuration =
            condition && msbuild_configuration(*condition)
              ? msbuild_configuration(*condition)
              : document.configuration;

          if (!configuration)
          {
            append_visual_studio_settings(
              common,
              document,
              group.body,
              directory,
              project.unresolved_properties
            );
          }
        }
      }

      sort_unique(common);
      project.cpp_standard = common.cpp_standard == 0 ? 20 : common.cpp_standard;

      for (const auto& configuration : configurations)
      {
        auto profile = common;
        profile.configuration = configuration;

        for (const auto& document : documents)
        {
          for (const auto& group : xml_elements(document.xml, "ItemDefinitionGroup"))
          {
            const auto condition = xml_attribute(group.opening, "Condition");
            const auto group_configuration =
              condition && msbuild_configuration(*condition)
                ? msbuild_configuration(*condition)
                : document.configuration;

            if (group_configuration == configuration)
            {
              append_visual_studio_settings(
                profile,
                document,
                group.body,
                directory,
                project.unresolved_properties
              );
            }
          }
        }

        sort_unique(profile);
        project.profiles.emplace(configuration, std::move(profile));
      }

      if (!project.profiles.empty())
      {
        auto common_includes = project.profiles.begin()->second.include_directories;
        auto common_definitions = project.profiles.begin()->second.compile_definitions;

        for (const auto& [configuration, profile] : project.profiles)
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
        }

        project.include_directories.clear();

        for (const auto& include : common_includes)
          project.include_directories.push_back(include.generic_string());

        project.definitions = common_definitions;

        for (auto& [configuration, profile] : project.profiles)
        {
          std::erase_if(
            profile.include_directories,
            [&common_includes](const auto& include)
            {
              return std::binary_search(common_includes.begin(), common_includes.end(), include);
            }
          );
          std::erase_if(
            profile.compile_definitions,
            [&common_definitions](const auto& definition)
            {
              return std::binary_search(
                common_definitions.begin(),
                common_definitions.end(),
                definition
              );
            }
          );
        }
      }

      for (const auto& source : xml_attributes(xml, "ClCompile", "Include"))
      {
        if (const auto relative = project_relative_path(directory, source))
          project.sources.push_back(*relative);
      }

      for (const auto& header : xml_attributes(xml, "ClInclude", "Include"))
      {
        if (const auto relative = project_relative_path(directory, header))
          project.headers.push_back(*relative);
      }

      for (const auto& reference : xml_attributes(xml, "ProjectReference", "Include"))
      {
        auto normalized = reference;
        std::replace(normalized.begin(), normalized.end(), '\\', '/');
        project.references.push_back((directory / normalized).lexically_normal());
      }

      std::ranges::sort(project.sources);
      std::ranges::sort(project.headers);
      std::ranges::sort(project.include_directories);
      std::ranges::sort(project.definitions);
      std::ranges::sort(project.references);
      project.sources.erase(
        std::unique(project.sources.begin(), project.sources.end()),
        project.sources.end()
      );
      project.headers.erase(
        std::unique(project.headers.begin(), project.headers.end()),
        project.headers.end()
      );
      project.include_directories.erase(
        std::unique(project.include_directories.begin(), project.include_directories.end()),
        project.include_directories.end()
      );
      project.definitions.erase(
        std::unique(project.definitions.begin(), project.definitions.end()),
        project.definitions.end()
      );
      project.references.erase(
        std::unique(project.references.begin(), project.references.end()),
        project.references.end()
      );
      return project;
    }

    bool is_cmake_generated_visual_studio_project(const std::filesystem::path& path)
    {
      std::ifstream file { path };
      const std::string contents {
        std::istreambuf_iterator<char> { file },
        std::istreambuf_iterator<char> {}
      };
      return contents.find("CMakeFiles") != std::string::npos
        || contents.find("ZERO_CHECK") != std::string::npos
        || contents.find("CMAKE_") != std::string::npos;
    }

  std::vector<std::filesystem::path> read_solution_projects(
      const std::filesystem::path& solution_path,
      std::ostream& error)
    {
      std::ifstream file { solution_path };

      if (!file)
      {
        error << "forge: could not open Visual Studio solution '" << solution_path.string() << "'\n";
        return {};
      }

      std::vector<std::filesystem::path> projects;
      std::string line;

      while (std::getline(file, line))
      {
        const auto content = trim(line);

        if (!content.starts_with("Project("))
          continue;

        std::vector<std::string> quoted;
        std::size_t position = 0;

        while ((position = content.find('"', position)) != std::string_view::npos)
        {
          const auto end = content.find('"', position + 1);

          if (end == std::string_view::npos)
            break;

          quoted.emplace_back(content.substr(position + 1, end - position - 1));
          position = end + 1;
        }

        if (quoted.size() < 3)
          continue;

        std::replace(quoted[2].begin(), quoted[2].end(), '\\', '/');
        const auto project = std::filesystem::path { quoted[2] };

        if (project.extension() == ".vcxproj")
          projects.push_back((solution_path.parent_path() / project).lexically_normal());
      }

      std::ranges::sort(projects);
      projects.erase(std::unique(projects.begin(), projects.end()), projects.end());
      return projects;
    }


} // namespace forge
