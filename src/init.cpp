#include "init.h"

#include "github.h"
#include "recipe.h"
#include "workspace.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <iterator>
#include <map>
#include <optional>
#include <regex>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace forge
{
  static int adopt_project_impl(const std::filesystem::path& project_directory,
                                const AdoptOptions& options,
                                const ProcessRunner& process_runner,
                                bool show_progress,
                                std::ostream& output,
                                std::ostream& error);

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

    void report_progress(std::ostream& output,
                         std::size_t current,
                         std::size_t total,
                         std::string_view description)
    {
      output << '[' << current << '/' << total << "] " << description << '\n' << std::flush;
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
        {
          break;
        }

        const auto close = xml.find(closing, open_end + 1);

        if (close == std::string_view::npos)
        {
          break;
        }

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
        {
          break;
        }

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
        {
          break;
        }

        const auto close = xml.find(closing, open_end + 1);

        if (close == std::string_view::npos)
        {
          break;
        }

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
        {
          break;
        }

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
      {
        return std::nullopt;
      }

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
      {
        return std::nullopt;
      }

      const auto equals = condition.find("==");

      if (equals == std::string_view::npos)
      {
        return std::nullopt;
      }

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
      {
        return true;
      }

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
        {
          continue;
        }

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
          {
            break;
          }

          remaining.remove_prefix(separator + 1);
        }
      }

      return { result.begin(), result.end() };
    }

    std::optional<std::string> project_relative_path(
      const std::filesystem::path& project_directory,
      const std::filesystem::path& path)
    {
      auto normalized_path = path.generic_string();
      std::replace(normalized_path.begin(), normalized_path.end(), '\\', '/');

      if (normalized_path.empty() || normalized_path.find("$(") != std::string::npos)
      {
        return std::nullopt;
      }

      const auto normalized =
        (std::filesystem::path { normalized_path }.is_absolute()
          ? std::filesystem::path { normalized_path }
          : project_directory / normalized_path).lexically_normal();
      const auto relative = normalized.lexically_relative(project_directory);

      if (relative.empty() || relative.is_absolute() || *relative.begin() == "..")
      {
        return std::nullopt;
      }

      return relative.generic_string();
    }

    struct VisualStudioProject
    {
      std::filesystem::path path;
      std::string format = "Visual Studio";
      std::string name;
      std::string type;
      int cpp_standard = 20;
      std::vector<std::string> sources;
      std::vector<std::string> headers;
      std::vector<std::string> include_directories;
      std::vector<std::string> definitions;
      std::vector<std::filesystem::path> references;
      std::map<std::string, BuildProfile> profiles;
      std::vector<std::string> unresolved_properties;
    };

    struct CMakeCommand
    {
      std::string name;
      std::vector<std::string> arguments;
    };

    void sort_unique(BuildProfile& profile);

    std::vector<std::string> cmake_arguments(std::string_view value)
    {
      std::vector<std::string> arguments;
      std::string argument;
      char quote = '\0';

      for (std::size_t index = 0; index < value.size(); ++index)
      {
        const auto character = value[index];

        if (quote != '\0')
        {
          if (character == '\\' && index + 1 < value.size())
          {
            argument += value[++index];
          }
          else if (character == quote)
          {
            quote = '\0';
          }
          else
          {
            argument += character;
          }

          continue;
        }

        if (character == '"' || character == '\'')
        {
          quote = character;
        }
        else if (character == '#')
        {
          while (index < value.size() && value[index] != '\n')
          {
            ++index;
          }
        }
        else if (std::isspace(static_cast<unsigned char>(character)) || character == ';')
        {
          if (!argument.empty())
          {
            arguments.push_back(std::move(argument));
            argument.clear();
          }
        }
        else
        {
          argument += character;
        }
      }

      if (!argument.empty())
      {
        arguments.push_back(std::move(argument));
      }

      return arguments;
    }

    std::vector<CMakeCommand> cmake_commands(std::string_view contents)
    {
      std::vector<CMakeCommand> commands;
      std::size_t position = 0;

      while (position < contents.size())
      {
        if (contents[position] == '#')
        {
          while (position < contents.size() && contents[position] != '\n')
          {
            ++position;
          }

          continue;
        }

        while (position < contents.size()
               && !std::isalpha(static_cast<unsigned char>(contents[position]))
               && contents[position] != '_')
        {
          if (contents[position] == '#')
          {
            while (position < contents.size() && contents[position] != '\n')
            {
              ++position;
            }
          }
          else
          {
            ++position;
          }
        }

        const auto name_begin = position;

        while (position < contents.size()
               && (std::isalnum(static_cast<unsigned char>(contents[position]))
                   || contents[position] == '_'))
        {
          ++position;
        }

        const auto name = contents.substr(name_begin, position - name_begin);

        while (position < contents.size()
               && std::isspace(static_cast<unsigned char>(contents[position])))
        {
          ++position;
        }

        if (name.empty() || position >= contents.size() || contents[position] != '(')
        {
          continue;
        }

        const auto arguments_begin = ++position;
        std::size_t depth = 1;
        char quote = '\0';

        while (position < contents.size() && depth != 0)
        {
          const auto character = contents[position];

          if (quote != '\0')
          {
            if (character == '\\')
            {
              ++position;
            }
            else if (character == quote)
            {
              quote = '\0';
            }
          }
          else if (character == '"' || character == '\'')
          {
            quote = character;
          }
          else if (character == '(')
          {
            ++depth;
          }
          else if (character == ')')
          {
            --depth;
          }

          ++position;
        }

        if (depth == 0)
        {
          auto command_name = std::string { name };
          std::ranges::transform(command_name, command_name.begin(), [](unsigned char character)
          {
            return static_cast<char>(std::tolower(character));
          });
          commands.push_back({
            std::move(command_name),
            cmake_arguments(contents.substr(arguments_begin, position - arguments_begin - 1))
          });
        }
      }

      return commands;
    }

    std::string replace_cmake_paths(std::string value,
                                    const std::filesystem::path& project_directory)
    {
      for (const auto variable : {
        std::string_view { "${CMAKE_CURRENT_SOURCE_DIR}" },
        std::string_view { "${CMAKE_SOURCE_DIR}" },
        std::string_view { "${PROJECT_SOURCE_DIR}" }
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

    bool is_cmake_scope(std::string_view value)
    {
      return value == "PUBLIC" || value == "PRIVATE" || value == "INTERFACE"
        || value == "BEFORE" || value == "SYSTEM";
    }

    std::optional<VisualStudioProject> read_cmake_project(
      const std::filesystem::path& path,
      std::ostream& error)
    {
      std::ifstream file { path };

      if (!file)
      {
        error << "forge: could not open CMake project '" << path.string() << "'\n";
        return std::nullopt;
      }

      const std::string contents {
        std::istreambuf_iterator<char> { file },
        std::istreambuf_iterator<char> {}
      };
      VisualStudioProject project;
      project.path = path;
      project.format = "CMake";
      project.name = path.parent_path().filename().string();
      const auto directory = path.parent_path();
      std::set<std::string> targets;

      for (const auto& command : cmake_commands(contents))
      {
        if (command.name == "project" && !command.arguments.empty())
        {
          project.name = command.arguments.front();
        }
        else if ((command.name == "add_executable" || command.name == "add_library")
                 && !command.arguments.empty())
        {
          targets.insert(command.arguments.front());

          if (targets.size() == 1)
          {
            project.type = command.name == "add_executable" ? "executable" : "static_library";

            if (command.name == "add_library" && command.arguments.size() > 1)
            {
              if (command.arguments[1] == "SHARED" || command.arguments[1] == "MODULE")
              {
                project.type = "dynamic_library";
              }
              else if (command.arguments[1] == "INTERFACE")
              {
                project.type = "header_only";
              }
            }
          }

          for (std::size_t index = 1; index < command.arguments.size(); ++index)
          {
            const auto& argument = command.arguments[index];

            if (is_cmake_scope(argument)
                || argument == "STATIC"
                || argument == "SHARED"
                || argument == "MODULE"
                || argument == "INTERFACE"
                || argument == "EXCLUDE_FROM_ALL"
                || argument == "WIN32"
                || argument == "MACOSX_BUNDLE")
            {
              continue;
            }

            const auto expanded = replace_cmake_paths(argument, directory);

            if (expanded.find("${") != std::string::npos || expanded.find("$<") != std::string::npos)
            {
              project.unresolved_properties.push_back(argument);
            }
            else if (const auto relative = project_relative_path(directory, expanded))
            {
              const auto extension = std::filesystem::path { *relative }.extension().string();

              if (extension == ".cpp" || extension == ".cc" || extension == ".cxx")
              {
                project.sources.push_back(*relative);
              }
              else if (extension == ".h" || extension == ".hpp" || extension == ".hh")
              {
                project.headers.push_back(*relative);
              }
            }
          }
        }
        else if (command.name == "target_compile_features" && command.arguments.size() > 1)
        {
          for (const auto& argument : command.arguments)
          {
            if (argument.starts_with("cxx_std_"))
            {
              project.cpp_standard = std::stoi(argument.substr(8));
            }
          }
        }
        else if (command.name == "target_include_directories" && command.arguments.size() > 1)
        {
          for (std::size_t index = 1; index < command.arguments.size(); ++index)
          {
            const auto& argument = command.arguments[index];

            if (is_cmake_scope(argument))
            {
              continue;
            }

            const auto expanded = replace_cmake_paths(argument, directory);

            if (expanded.find("${") != std::string::npos || expanded.find("$<") != std::string::npos)
            {
              project.unresolved_properties.push_back(argument);
            }
            else if (const auto relative = project_relative_path(directory, expanded))
            {
              project.include_directories.push_back(*relative);
            }
          }
        }
        else if (command.name == "target_compile_definitions" && command.arguments.size() > 1)
        {
          for (std::size_t index = 1; index < command.arguments.size(); ++index)
          {
            const auto& argument = command.arguments[index];

            if (!is_cmake_scope(argument) && is_valid_compile_definition(argument))
            {
              project.definitions.push_back(argument);
            }
            else if (argument.find('$') != std::string::npos)
            {
              project.unresolved_properties.push_back(argument);
            }
          }
        }
      }

      if (targets.size() > 1)
      {
        project.type.clear();
        project.unresolved_properties.push_back(
          std::to_string(targets.size()) + " CMake targets require inferred Forge targets"
        );
      }

      for (auto* values : {
        &project.sources,
        &project.headers,
        &project.include_directories,
        &project.definitions,
        &project.unresolved_properties
      })
      {
        std::ranges::sort(*values);
        values->erase(std::unique(values->begin(), values->end()), values->end());
      }

      return project;
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
          {
            quote = '\0';
          }
          else
          {
            current += character;
          }
        }
        else if (character == '"' || character == '\'')
        {
          quote = character;
        }
        else if (character == ',' || std::isspace(static_cast<unsigned char>(character)))
        {
          if (!current.empty())
          {
            values.push_back(std::move(current));
            current.clear();
          }
        }
        else
        {
          current += character;
        }
      }

      if (!current.empty())
      {
        values.push_back(std::move(current));
      }

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
        {
          unresolved.push_back(standard);
        }
      }

      for (const auto& include : xcode_setting_values(settings, "HEADER_SEARCH_PATHS"))
      {
        if (include == "$(inherited)")
        {
          continue;
        }

        const auto expanded = replace_xcode_paths(include, project_directory);

        if (expanded.find("$(") != std::string::npos || expanded.find("${") != std::string::npos)
        {
          unresolved.push_back(include);
        }
        else if (const auto relative = project_relative_path(project_directory, expanded))
        {
          profile.include_directories.push_back(*relative);
        }
      }

      for (const auto& definition : xcode_setting_values(settings, "GCC_PREPROCESSOR_DEFINITIONS"))
      {
        if (definition == "$(inherited)")
        {
          continue;
        }

        if (definition.find('$') != std::string::npos)
        {
          unresolved.push_back(definition);
        }
        else if (is_valid_compile_definition(definition))
        {
          profile.compile_definitions.push_back(definition);
        }
      }
    }

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
      {
        project.unresolved_properties.push_back(
          std::to_string(targets.size()) + " Xcode targets require inferred Forge targets"
        );
      }

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
        {
          project.unresolved_properties.push_back(relative);
        }
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
          {
            common_standard = 0;
          }
        }

        project.cpp_standard = common_standard == 0 ? 20 : common_standard;

        for (const auto& include : common_includes)
        {
          project.include_directories.push_back(include.generic_string());
        }

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

    int cpp_standard_from_msbuild(std::string_view standard)
    {
      if (standard == "stdcpp14")
      {
        return 14;
      }

      if (standard == "stdcpp17")
      {
        return 17;
      }

      if (standard == "stdcpp20")
      {
        return 20;
      }

      if (standard == "stdcpp23" || standard == "stdcpplatest")
      {
        return 23;
      }

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
        {
          settings.cpp_standard = parsed;
        }
      }

      for (const auto& include : split_msbuild_list(xml_values(xml, "AdditionalIncludeDirectories")))
      {
        const auto expanded = replace_msbuild_paths(
          include,
          project_directory,
          document.path.parent_path()
        );

        if (expanded.find("$(") != std::string::npos)
        {
          unresolved.push_back(include);
        }
        else if (const auto relative = project_relative_path(project_directory, expanded))
        {
          settings.include_directories.push_back(*relative);
        }
      }

      for (const auto& definition : split_msbuild_list(xml_values(xml, "PreprocessorDefinitions")))
      {
        if (definition.find("$(") != std::string::npos)
        {
          unresolved.push_back(definition);
        }
        else if (is_valid_compile_definition(definition))
        {
          settings.compile_definitions.push_back(definition);
        }
      }
    }

    void sort_unique(BuildProfile& profile)
    {
      std::ranges::sort(profile.include_directories);
      std::ranges::sort(profile.compile_definitions);
      profile.include_directories.erase(
        std::unique(profile.include_directories.begin(), profile.include_directories.end()),
        profile.include_directories.end()
      );
      profile.compile_definitions.erase(
        std::unique(profile.compile_definitions.begin(), profile.compile_definitions.end()),
        profile.compile_definitions.end()
      );
    }

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
        {
          configurations.insert(*document.configuration);
        }

        for (const auto& group : xml_elements(document.xml, "ItemDefinitionGroup"))
        {
          const auto condition = xml_attribute(group.opening, "Condition");
          const auto configuration =
            condition && msbuild_configuration(*condition)
              ? msbuild_configuration(*condition)
              : document.configuration;

          if (configuration)
          {
            configurations.insert(*configuration);
          }
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
        {
          project.include_directories.push_back(include.generic_string());
        }

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
        {
          project.sources.push_back(*relative);
        }
      }

      for (const auto& header : xml_attributes(xml, "ClInclude", "Include"))
      {
        if (const auto relative = project_relative_path(directory, header))
        {
          project.headers.push_back(*relative);
        }
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

    bool is_cmake_generated_xcode_project(const std::filesystem::path& path)
    {
      std::ifstream file { path / "project.pbxproj" };
      const std::string contents {
        std::istreambuf_iterator<char> { file },
        std::istreambuf_iterator<char> {}
      };
      return contents.find("CMakeFiles") != std::string::npos
        || contents.find("ZERO_CHECK") != std::string::npos
        || contents.find("CMAKE_") != std::string::npos;
    }

    std::vector<std::filesystem::path> files_with_extension(
      const std::filesystem::path& directory,
      std::string_view extension)
    {
      std::vector<std::filesystem::path> paths;
      std::error_code filesystem_error;

      for (const auto& entry : std::filesystem::directory_iterator { directory, filesystem_error })
      {
        if (!filesystem_error
            && entry.is_regular_file(filesystem_error)
            && entry.path().extension() == extension)
        {
          paths.push_back(entry.path());
        }
      }

      std::ranges::sort(paths);
      return paths;
    }

    std::vector<std::filesystem::path> directories_with_extension(
      const std::filesystem::path& directory,
      std::string_view extension)
    {
      std::vector<std::filesystem::path> paths;
      std::error_code filesystem_error;

      for (const auto& entry : std::filesystem::directory_iterator { directory, filesystem_error })
      {
        if (!filesystem_error
            && entry.is_directory(filesystem_error)
            && entry.path().extension() == extension)
        {
          paths.push_back(entry.path());
        }
      }

      std::ranges::sort(paths);
      return paths;
    }

    void merge_project_metadata(VisualStudioProject& project,
                                const VisualStudioProject& additional)
    {
      if (project.name.empty())
      {
        project.name = additional.name;
      }

      if (project.type.empty())
      {
        project.type = additional.type;
      }

      if (project.cpp_standard == 20 && additional.cpp_standard != 20)
      {
        project.cpp_standard = additional.cpp_standard;
      }

      project.include_directories.insert(
        project.include_directories.end(),
        additional.include_directories.begin(),
        additional.include_directories.end()
      );
      project.definitions.insert(
        project.definitions.end(),
        additional.definitions.begin(),
        additional.definitions.end()
      );
      project.unresolved_properties.insert(
        project.unresolved_properties.end(),
        additional.unresolved_properties.begin(),
        additional.unresolved_properties.end()
      );

      for (const auto& [name, profile] : additional.profiles)
      {
        project.profiles.try_emplace(name, profile);
      }

      for (auto* values : {
        &project.include_directories,
        &project.definitions,
        &project.unresolved_properties
      })
      {
        std::ranges::sort(*values);
        values->erase(std::unique(values->begin(), values->end()), values->end());
      }
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
                          std::vector<std::string>& headers,
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
                 && is_cpp_header(entry.path()))
        {
          const auto relative = entry.path().lexically_relative(project_directory);
          headers.push_back(relative.generic_string());

          if (relative.begin()->string() == "include")
          {
            public_headers.push_back(relative.generic_string());
          }
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
      std::ranges::sort(headers);
      std::ranges::sort(entry_points);
      return true;
    }

    std::vector<std::string> included_headers(const std::filesystem::path& path)
    {
      std::ifstream file { path };
      std::vector<std::string> includes;
      std::string line;

      while (std::getline(file, line))
      {
        auto content = std::string_view { line };
        const auto first = content.find_first_not_of(" \t");

        if (first == std::string_view::npos || content[first] != '#')
        {
          continue;
        }

        content.remove_prefix(first + 1);
        const auto directive = content.find_first_not_of(" \t");

        if (directive == std::string_view::npos)
        {
          continue;
        }

        content.remove_prefix(directive);

        if (!content.starts_with("include"))
        {
          continue;
        }

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
          includes.push_back(std::move(include));
        }
      }

      return includes;
    }

    bool looks_like_dependency_include(std::string_view include)
    {
      static const std::set<std::string_view> system_headers {
        "assert.h", "complex.h", "ctype.h", "errno.h", "fenv.h", "float.h",
        "inttypes.h", "limits.h", "locale.h", "math.h", "process.h", "setjmp.h",
        "signal.h", "stdarg.h", "stdbool.h", "stddef.h", "stdint.h", "stdio.h",
        "stdlib.h", "string.h", "time.h", "uchar.h", "unistd.h", "wchar.h",
        "wctype.h", "windows.h",
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
        && !include.starts_with("netinet/");
    }

    std::optional<std::string> resolve_local_header(
      const std::string& including_file,
      const std::string& include,
      const std::vector<std::string>& headers);

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
        for (const auto& include : included_headers(project_directory / scanned_file))
        {
          if (!resolve_local_header(scanned_file, include, headers)
              && looks_like_dependency_include(include))
          {
            unresolved.try_emplace(include, scanned_file);
          }
        }
      }

      return unresolved;
    }

    struct SiblingDependency
    {
      std::string name;
      std::string path;
    };

    std::vector<SiblingDependency> visual_studio_dependencies(
      const std::filesystem::path& project_directory,
      const VisualStudioProject& project)
    {
      std::vector<SiblingDependency> dependencies;

      for (const auto& reference : project.references)
      {
        std::ostringstream ignored_error;
        const auto referenced = read_visual_studio_project(reference, ignored_error);

        if (!referenced)
        {
          continue;
        }

        std::error_code filesystem_error;
        const auto relative = std::filesystem::relative(
          reference.parent_path(),
          project_directory,
          filesystem_error
        );

        if (!filesystem_error)
        {
          dependencies.push_back({ referenced->name, relative.generic_string() });
        }
      }

      std::ranges::sort(
        dependencies,
        {},
        [](const SiblingDependency& dependency)
        {
          return dependency.name;
        }
      );
      dependencies.erase(
        std::unique(
          dependencies.begin(),
          dependencies.end(),
          [](const SiblingDependency& left, const SiblingDependency& right)
          {
            return left.name == right.name;
          }
        ),
        dependencies.end()
      );
      return dependencies;
    }

    struct GitHubDependency
    {
      std::string name;
      std::string repository;
      std::string git;
      std::string commit;
    };

    bool provides_include(const Recipe& recipe, std::string_view include)
    {
      if (!recipe.targets.empty()
          || (recipe.type != "static_library"
              && recipe.type != "dynamic_library"
              && recipe.type != "header_only"
              && recipe.type != "imported_library"))
      {
        return false;
      }

      for (const auto& header : recipe.public_headers)
      {
        const auto generic = header.generic_string();

        if (generic.starts_with("include/") && generic.substr(8) == include)
        {
          return true;
        }
      }

      for (const auto& profile : recipe.imports)
      {
        for (const auto& header : profile.public_headers)
        {
          const auto generic = header.generic_string();

          if (generic == include || generic.ends_with('/' + std::string { include }))
          {
            return true;
          }
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
        {
          break;
        }

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
        {
          continue;
        }

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
          {
            matches[include].push_back(SiblingDependency {
              sibling.name,
              relative.generic_string()
            });
          }
        }
      }

      std::map<std::string, std::vector<std::pair<std::string, SiblingDependency>>> by_name;

      for (const auto& [include, candidates] : matches)
      {
        if (candidates.size() == 1)
        {
          by_name[candidates.front().name].emplace_back(include, candidates.front());
        }
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
        {
          continue;
        }

        result.push_back(candidates.front().second);

        for (const auto& [include, dependency] : candidates)
        {
          unresolved.erase(include);
        }
      }

      return result;
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
          origin =
            content.find("remote \"origin\"") != std::string_view::npos;
          continue;
        }

        const auto equals = content.find('=');

        if (!origin || equals == std::string_view::npos)
        {
          continue;
        }

        const auto key = content.substr(0, equals);

        if (key.find("url") == std::string_view::npos)
        {
          continue;
        }

        auto url = std::string { content.substr(equals + 1) };
        const auto first = url.find_first_not_of(" \t");
        url = first == std::string::npos ? std::string {} : url.substr(first);
        std::string_view path;

        if (const auto github = url.find("github.com/"); github != std::string::npos)
        {
          path = std::string_view { url }.substr(github + 11);
        }
        else if (const auto github = url.find("github.com:"); github != std::string::npos)
        {
          path = std::string_view { url }.substr(github + 11);
        }

        const auto slash = path.find('/');

        if (slash != std::string_view::npos && slash != 0)
        {
          return std::string { path.substr(0, slash) };
        }
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
        {
          name.resize(extension);
        }
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

    std::map<std::string, std::vector<std::string>> github_suggestions(
      const std::filesystem::path& project_directory,
      const std::map<std::string, std::string>& unresolved)
    {
      std::map<std::string, std::vector<std::string>> suggestions;
      const auto owner = github_owner(project_directory);

      if (!owner)
      {
        return suggestions;
      }

      for (const auto& [include, source] : unresolved)
      {
        const auto name = github_repository_name(include);

        if (!name.empty())
        {
          suggestions[*owner + "/" + name].push_back(include);
        }
      }

      return suggestions;
    }

    std::optional<std::string> git_head(const std::filesystem::path& repository)
    {
      std::ifstream head_file { repository / ".git" / "HEAD" };
      std::string head;
      std::getline(head_file, head);

      if (!head.starts_with("ref: "))
      {
        return head.empty() ? std::nullopt : std::optional<std::string> { head };
      }

      const auto reference = head.substr(5);
      std::ifstream reference_file { repository / ".git" / reference };
      std::string commit;
      std::getline(reference_file, commit);

      if (!commit.empty())
      {
        return commit;
      }

      std::ifstream packed { repository / ".git" / "packed-refs" };
      std::string line;

      while (std::getline(packed, line))
      {
        const auto separator = line.find(' ');

        if (separator != std::string::npos && line.substr(separator + 1) == reference)
        {
          return line.substr(0, separator);
        }
      }

      return std::nullopt;
    }

    bool is_exact_commit(std::string_view commit)
    {
      return
        (commit.size() == 40 || commit.size() == 64)
        && std::ranges::all_of(
          commit,
          [](unsigned char character)
          {
            return std::isxdigit(character);
          }
        );
    }

    std::vector<GitHubDependency> resolve_github_dependencies(
      const std::filesystem::path& project_directory,
      const std::map<std::string, std::vector<std::string>>& suggestions,
      std::map<std::string, std::string>& unresolved,
      const ProcessRunner& process_runner,
      std::ostream& output)
    {
      std::map<std::string, GitHubDependency> dependencies;
      std::set<std::string> conflicting_names;
      const auto cache = project_directory / ".forge" / "adopt" / "github";
      std::error_code filesystem_error;
      std::filesystem::create_directories(cache, filesystem_error);

      for (const auto& [repository, includes] : suggestions)
      {
        const auto slash = repository.find('/');
        const auto name = repository.substr(slash + 1);
        const auto checkout = cache / name;
        std::filesystem::remove_all(checkout, filesystem_error);
        std::ostringstream clone_error;
        const auto git = "https://github.com/" + repository + ".git";

        if (process_runner({ "git", "clone", "--quiet", "--depth", "1", git, checkout.string() },
                           project_directory,
                           clone_error) != 0)
        {
          continue;
        }

        Recipe recipe;
        std::ostringstream recipe_error;

        if (!read_recipe(checkout / "forge.recipe.toml", recipe, recipe_error))
        {
          continue;
        }

        const auto verified = std::ranges::all_of(
          includes,
          [&recipe](const std::string& include)
          {
            return provides_include(recipe, include);
          }
        );
        const auto commit = git_head(checkout);

        if (!verified || !commit || !is_exact_commit(*commit))
        {
          continue;
        }

        const GitHubDependency dependency { recipe.name, repository, git, *commit };
        const auto existing = dependencies.find(recipe.name);

        if (existing != dependencies.end() && existing->second.repository != repository)
        {
          conflicting_names.insert(recipe.name);
          continue;
        }

        dependencies[recipe.name] = dependency;
      }

      std::vector<GitHubDependency> result;

      for (auto& [name, dependency] : dependencies)
      {
        if (!conflicting_names.contains(name))
        {
          for (const auto& include : suggestions.at(dependency.repository))
          {
            unresolved.erase(include);
          }

          output << "Pinned GitHub dependency " << dependency.repository
                 << " at " << dependency.commit << '\n';
          result.push_back(std::move(dependency));
        }
      }

      return result;
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
        for (const auto& include : included_headers(project_directory / scanned_file))
        {
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
            {
              matching_roots.insert(header.substr(0, header.size() - suffix.size()));
            }
          }

          if (matching_roots.size() == 1 && *matching_roots.begin() != "include")
          {
            include_directories.insert(*matching_roots.begin());
          }
        }
      }

      return { include_directories.begin(), include_directories.end() };
    }

    std::optional<std::string> resolve_local_header(
      const std::string& including_file,
      const std::string& include,
      const std::vector<std::string>& headers)
    {
      std::set<std::string> matches;
      const auto relative =
        (std::filesystem::path { including_file }.parent_path() / include)
          .lexically_normal()
          .generic_string();

      if (std::binary_search(headers.begin(), headers.end(), relative))
      {
        matches.insert(relative);
      }

      if (std::binary_search(headers.begin(), headers.end(), include))
      {
        matches.insert(include);
      }

      const auto suffix = '/' + include;

      for (const auto& header : headers)
      {
        if (header.size() > suffix.size() && header.ends_with(suffix))
        {
          matches.insert(header);
        }
      }

      if (matches.size() == 1)
      {
        return *matches.begin();
      }

      return std::nullopt;
    }

    std::set<std::string> reachable_local_headers(
      const std::filesystem::path& project_directory,
      const std::string& source,
      const std::vector<std::string>& headers)
    {
      std::set<std::string> reachable;
      std::vector<std::string> pending { source };

      while (!pending.empty())
      {
        auto file = std::move(pending.back());
        pending.pop_back();

        for (const auto& include : included_headers(project_directory / file))
        {
          const auto resolved = resolve_local_header(file, include, headers);

          if (resolved && reachable.insert(*resolved).second)
          {
            pending.push_back(*resolved);
          }
        }
      }

      return reachable;
    }

    std::vector<std::vector<std::string>> infer_target_sources(
      const std::filesystem::path& project_directory,
      const std::vector<std::string>& sources,
      const std::vector<std::string>& headers,
      const std::vector<std::string>& entry_points)
    {
      std::map<std::string, std::set<std::string>> reachable;

      for (const auto& source : sources)
      {
        reachable[source] = reachable_local_headers(project_directory, source, headers);
      }

      std::vector<std::vector<std::string>> target_sources(entry_points.size());

      for (std::size_t target_index = 0; target_index < entry_points.size(); ++target_index)
      {
        target_sources[target_index].push_back(entry_points[target_index]);
      }

      for (const auto& source : sources)
      {
        if (std::binary_search(entry_points.begin(), entry_points.end(), source))
        {
          continue;
        }

        std::vector<std::size_t> owners;

        for (std::size_t target_index = 0; target_index < entry_points.size(); ++target_index)
        {
          const auto& target_headers = reachable.at(entry_points[target_index]);
          const auto& source_headers = reachable.at(source);

          if (std::ranges::any_of(
            source_headers,
            [&target_headers](const std::string& header)
            {
              return target_headers.contains(header);
            }
          ))
          {
            owners.push_back(target_index);
          }
        }

        if (owners.empty())
        {
          for (std::size_t target_index = 0; target_index < entry_points.size(); ++target_index)
          {
            target_sources[target_index].push_back(source);
          }
        }
        else
        {
          for (const auto owner : owners)
          {
            target_sources[owner].push_back(source);
          }
        }
      }

      for (auto& target : target_sources)
      {
        std::ranges::sort(target);
      }

      return target_sources;
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
        {
          continue;
        }

        std::vector<std::string> quoted;
        std::size_t position = 0;

        while ((position = content.find('"', position)) != std::string_view::npos)
        {
          const auto end = content.find('"', position + 1);

          if (end == std::string_view::npos)
          {
            break;
          }

          quoted.emplace_back(content.substr(position + 1, end - position - 1));
          position = end + 1;
        }

        if (quoted.size() < 3)
        {
          continue;
        }

        std::replace(quoted[2].begin(), quoted[2].end(), '\\', '/');
        const auto project = std::filesystem::path { quoted[2] };

        if (project.extension() == ".vcxproj")
        {
          projects.push_back((solution_path.parent_path() / project).lexically_normal());
        }
      }

      std::ranges::sort(projects);
      projects.erase(std::unique(projects.begin(), projects.end()), projects.end());
      return projects;
    }

    std::vector<std::filesystem::path> read_cmake_subdirectories(
      const std::filesystem::path& cmake_path)
    {
      std::ifstream file { cmake_path };
      const std::string contents {
        std::istreambuf_iterator<char> { file },
        std::istreambuf_iterator<char> {}
      };
      std::vector<std::filesystem::path> projects;

      for (const auto& command : cmake_commands(contents))
      {
        if (command.name != "add_subdirectory" || command.arguments.empty())
        {
          continue;
        }

        const auto& argument = command.arguments.front();

        if (argument.find('$') != std::string::npos)
        {
          continue;
        }

        const auto project = (cmake_path.parent_path() / argument).lexically_normal();

        if (std::filesystem::is_regular_file(project / "CMakeLists.txt"))
        {
          projects.push_back(project);
        }
      }

      std::ranges::sort(projects);
      projects.erase(std::unique(projects.begin(), projects.end()), projects.end());
      return projects;
    }

    bool cmake_defines_target(const std::filesystem::path& cmake_path)
    {
      std::ifstream file { cmake_path };
      const std::string contents {
        std::istreambuf_iterator<char> { file },
        std::istreambuf_iterator<char> {}
      };

      return std::ranges::any_of(cmake_commands(contents), [](const CMakeCommand& command)
      {
        return command.name == "add_executable" || command.name == "add_library";
      });
    }

    int adopt_solution(const std::filesystem::path& workspace_directory,
                       const std::filesystem::path& solution_path,
                       const AdoptOptions& options,
                       const ProcessRunner& process_runner,
                       std::ostream& output,
                       std::ostream& error)
    {
      const auto workspace_path = workspace_directory / "forge.workspace.toml";

      if (std::filesystem::exists(workspace_path))
      {
        error << "forge: '" << workspace_path.string() << "' already exists\n";
        return 2;
      }

      const auto projects = read_solution_projects(solution_path, error);

      if (projects.empty())
      {
        error << "forge: solution contains no C++ projects\n";
        return 2;
      }

      std::set<std::filesystem::path> directories;
      std::vector<std::string> relative_directories;
      const auto progress_total = projects.size() + 2;

      report_progress(output, 1, progress_total, "Reading Visual Studio solution");

      for (const auto& project : projects)
      {
        const auto directory = project.parent_path();
        const auto relative = directory.lexically_relative(workspace_directory);

        if (relative.empty()
            || relative == "."
            || relative.is_absolute()
            || *relative.begin() == "..")
        {
          error << "forge: solution projects must live in distinct subdirectories\n";
          return 2;
        }

        if (!directories.insert(directory).second)
        {
          error << "forge: multiple solution projects share directory '" << directory.string()
                << "'\n";
          return 2;
        }

        relative_directories.push_back(relative.generic_string());

        if (std::filesystem::exists(directory / "forge.recipe.toml"))
        {
          error << "forge: '" << (directory / "forge.recipe.toml").string() << "' already exists\n";
          return 2;
        }
      }

      for (std::size_t index = 0; index < projects.size(); ++index)
      {
        const auto& project = projects[index];
        report_progress(
          output,
          index + 2,
          progress_total,
          "Adopting project " + project.stem().string()
        );

        if (adopt_project_impl(
          project.parent_path(),
          options,
          process_runner,
          false,
          output,
          error
        ) != 0)
        {
          return 2;
        }
      }

      report_progress(output, progress_total, progress_total, "Writing workspace");

      const std::string workspace =
        "#:schema " + std::string { workspace_schema_url } + "\n"
        "\n"
        "[workspace]\n"
        "name = \"" + escape_toml_string(solution_path.stem().string()) + "\"\n"
        "projects = " + format_sources(relative_directories) + "\n";

      if (!write_file(workspace_path, workspace, error))
      {
        return 2;
      }

      output << "Created " << workspace_path.string() << '\n'
             << "Adopted " << projects.size() << " Visual Studio project";
      output << (projects.size() == 1 ? "\n" : "s\n");
      return 0;
    }

    int adopt_cmake_workspace(const std::filesystem::path& workspace_directory,
                              const std::filesystem::path& cmake_path,
                              const std::vector<std::filesystem::path>& projects,
                              const AdoptOptions& options,
                              const ProcessRunner& process_runner,
                              std::ostream& output,
                              std::ostream& error)
    {
      const auto workspace_path = workspace_directory / "forge.workspace.toml";

      if (std::filesystem::exists(workspace_path))
      {
        error << "forge: '" << workspace_path.string() << "' already exists\n";
        return 2;
      }

      std::vector<std::string> relative_directories;
      const auto progress_total = projects.size() + 2;
      report_progress(output, 1, progress_total, "Reading CMake superproject");

      for (const auto& project : projects)
      {
        const auto relative = project.lexically_relative(workspace_directory);

        if (relative.empty()
            || relative == "."
            || relative.is_absolute()
            || *relative.begin() == "..")
        {
          error << "forge: CMake subprojects must live inside the workspace\n";
          return 2;
        }

        if (std::filesystem::exists(project / "forge.recipe.toml"))
        {
          error << "forge: '" << (project / "forge.recipe.toml").string() << "' already exists\n";
          return 2;
        }

        relative_directories.push_back(relative.generic_string());
      }

      for (std::size_t index = 0; index < projects.size(); ++index)
      {
        report_progress(
          output,
          index + 2,
          progress_total,
          "Adopting project " + projects[index].filename().string()
        );

        if (adopt_project_impl(
          projects[index],
          options,
          process_runner,
          false,
          output,
          error
        ) != 0)
        {
          return 2;
        }
      }

      report_progress(output, progress_total, progress_total, "Writing workspace");
      auto name = cmake_path.parent_path().filename().string();
      const auto cmake_project = read_cmake_project(cmake_path, error);

      if (cmake_project && !cmake_project->name.empty())
      {
        name = cmake_project->name;
      }

      const std::string workspace =
        "#:schema " + std::string { workspace_schema_url } + "\n"
        "\n"
        "[workspace]\n"
        "name = \"" + escape_toml_string(name) + "\"\n"
        "projects = " + format_sources(relative_directories) + "\n";

      if (!write_file(workspace_path, workspace, error))
      {
        return 2;
      }

      output << "Created " << workspace_path.string() << '\n'
             << "Adopted " << projects.size() << " CMake project";
      output << (projects.size() == 1 ? "\n" : "s\n");
      return 0;
    }

  } // namespace

  int adopt_project(const std::filesystem::path& project_directory,
                    std::ostream& output,
                    std::ostream& error)
  {
    return adopt_project(project_directory, AdoptOptions {}, run_process, output, error);
  }

  int adopt_project(const std::filesystem::path& project_directory,
                    const AdoptOptions& options,
                    const ProcessRunner& process_runner,
                    std::ostream& output,
                    std::ostream& error)
  {
    return adopt_project_impl(
      project_directory,
      options,
      process_runner,
      true,
      output,
      error
    );
  }

  static int adopt_project_impl(const std::filesystem::path& project_directory,
                                const AdoptOptions& options,
                                const ProcessRunner& process_runner,
                                bool show_progress,
                                std::ostream& output,
                                std::ostream& error)
  {
    const auto solutions = files_with_extension(project_directory, ".sln");
    const auto visual_studio_projects = files_with_extension(project_directory, ".vcxproj");
    const auto xcode_projects = directories_with_extension(project_directory, ".xcodeproj");
    const auto cmake_path = project_directory / "CMakeLists.txt";
    const auto has_cmake_project = std::filesystem::is_regular_file(cmake_path);
    const auto cmake_subdirectories = has_cmake_project
      ? read_cmake_subdirectories(cmake_path)
      : std::vector<std::filesystem::path> {};

    if (has_cmake_project
        && !cmake_subdirectories.empty()
        && !cmake_defines_target(cmake_path))
    {
      return adopt_cmake_workspace(
        project_directory,
        cmake_path,
        cmake_subdirectories,
        options,
        process_runner,
        output,
        error
      );
    }

    if (solutions.size() == 1
        && visual_studio_projects.empty()
        && xcode_projects.empty()
        && !has_cmake_project)
    {
      return adopt_solution(
        project_directory,
        solutions.front(),
        options,
        process_runner,
        output,
        error
      );
    }

    if (show_progress)
    {
      report_progress(output, 1, 6, "Inspecting project");
    }

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
    std::vector<std::string> headers;
    std::vector<std::string> entry_points;
    std::optional<VisualStudioProject> visual_studio_project;
    std::vector<std::string> merged_project_formats;

    if (show_progress)
    {
      report_progress(output, 2, 6, "Scanning sources and headers");
    }

    if (!discover_sources(project_directory, sources, public_headers, headers, entry_points, error))
    {
      return 2;
    }

    if (show_progress)
    {
      report_progress(output, 3, 6, "Reading project metadata");
    }

    if (visual_studio_projects.size() == 1
        && !(has_cmake_project
             && is_cmake_generated_visual_studio_project(visual_studio_projects.front())))
    {
      visual_studio_project = read_visual_studio_project(visual_studio_projects.front(), error);

      if (!visual_studio_project)
      {
        return 2;
      }
    }
    else if (xcode_projects.size() == 1
             && !(has_cmake_project && is_cmake_generated_xcode_project(xcode_projects.front())))
    {
      visual_studio_project = read_xcode_project(xcode_projects.front(), error);

      if (!visual_studio_project)
      {
        return 2;
      }
    }
    else if (has_cmake_project)
    {
      visual_studio_project = read_cmake_project(cmake_path, error);

      if (!visual_studio_project)
      {
        return 2;
      }
    }

    if (visual_studio_project && has_cmake_project && visual_studio_project->format != "CMake")
    {
      const auto cmake_project = read_cmake_project(cmake_path, error);

      if (!cmake_project)
      {
        return 2;
      }

      merge_project_metadata(*visual_studio_project, *cmake_project);
      merged_project_formats.push_back("CMake");
    }

    if (visual_studio_project)
    {
      if (!visual_studio_project->sources.empty())
      {
        sources = visual_studio_project->sources;
      }

      if (!visual_studio_project->headers.empty())
      {
        headers = visual_studio_project->headers;
      }

      public_headers.clear();
      entry_points.clear();

      for (const auto& header : headers)
      {
        const auto path = std::filesystem::path { header };

        if (!path.empty() && path.begin()->string() == "include")
        {
          public_headers.push_back(header);
        }
      }

      for (const auto& source : sources)
      {
        if (contains_main_function(project_directory / source))
        {
          entry_points.push_back(source);
        }
      }
    }

    if (show_progress)
    {
      report_progress(output, 4, 6, "Resolving dependencies");
    }

    auto include_directories = infer_include_directories(project_directory, sources, headers);

    if (visual_studio_project)
    {
      include_directories.insert(
        include_directories.end(),
        visual_studio_project->include_directories.begin(),
        visual_studio_project->include_directories.end()
      );
      std::ranges::sort(include_directories);
      include_directories.erase(
        std::unique(include_directories.begin(), include_directories.end()),
        include_directories.end()
      );
    }

    auto unresolved = unresolved_includes(project_directory, sources, headers);
    auto sibling_dependencies = infer_sibling_dependencies(project_directory, unresolved);

    if (visual_studio_project)
    {
      const auto referenced = visual_studio_dependencies(project_directory, *visual_studio_project);
      sibling_dependencies.insert(
        sibling_dependencies.end(),
        referenced.begin(),
        referenced.end()
      );
      std::ranges::sort(
        sibling_dependencies,
        {},
        [](const SiblingDependency& dependency)
        {
          return dependency.name;
        }
      );
      sibling_dependencies.erase(
        std::unique(
          sibling_dependencies.begin(),
          sibling_dependencies.end(),
          [](const SiblingDependency& left, const SiblingDependency& right)
          {
            return left.name == right.name;
          }
        ),
        sibling_dependencies.end()
      );
    }

    const auto suggestions = github_suggestions(project_directory, unresolved);
    const auto github_dependencies = options.github
      ? resolve_github_dependencies(
        project_directory,
        suggestions,
        unresolved,
        process_runner,
        output
      )
      : std::vector<GitHubDependency> {};
    const auto project_name = visual_studio_project
      ? visual_studio_project->name
      : project_directory.filename().string();
    const auto escaped_project_name = escape_toml_string(project_name);
    const auto formatted_sources = format_sources(sources);
    const auto formatted_headers = format_sources(public_headers);
    const auto formatted_include_directories = format_sources(include_directories);
    std::string recipe =
      "#:schema " + std::string { recipe_schema_url } + "\n"
      "\n"
      "[project]\n"
      "name = \"" + escaped_project_name + "\"\n"
      "version = \"0.1.0\"\n";

    if (entry_points.size() > 1)
    {
      std::set<std::string> target_names;
      const auto inferred_target_sources =
        infer_target_sources(project_directory, sources, headers, entry_points);

      for (std::size_t index = 0; index < entry_points.size(); ++index)
      {
        auto name = target_name(entry_points[index], index);

        if (!target_names.insert(name).second)
        {
          name += '-' + std::to_string(index + 1);
          target_names.insert(name);
        }

        recipe
          += "\n[target." + name + "]\n"
          "type = \"executable\"\n"
          "cpp_std = " + std::to_string(
            visual_studio_project ? visual_studio_project->cpp_standard : 20
          ) + "\n"
          "sources = " + format_sources(inferred_target_sources[index]) + "\n";

        if (!public_headers.empty())
        {
          recipe += "public_headers = " + formatted_headers + "\n";
        }

        if (!include_directories.empty())
        {
          recipe += "include_dirs = " + formatted_include_directories + "\n";
        }

        if (visual_studio_project && !visual_studio_project->definitions.empty())
        {
          recipe += "defines = " + format_sources(visual_studio_project->definitions) + "\n";
        }
      }
    }
    else
    {
      const auto type =
        visual_studio_project && !visual_studio_project->type.empty()
          ? visual_studio_project->type
          : sources.empty() && !public_headers.empty()
          ? "header_only"
          : entry_points.empty() && !sources.empty() && !public_headers.empty()
          ? "static_library"
          : "executable";
      recipe
        += "type = \"" + std::string { type } + "\"\n"
        "cpp_std = " + std::to_string(
          visual_studio_project ? visual_studio_project->cpp_standard : 20
        ) + "\n"
        "\n"
        "[sources]\n"
        "paths = " + formatted_sources + "\n";

      if (!public_headers.empty())
      {
        recipe += "public_headers = " + formatted_headers + "\n";
      }

      if (!include_directories.empty())
      {
        recipe += "include_dirs = " + formatted_include_directories + "\n";
      }

      if (visual_studio_project && !visual_studio_project->definitions.empty())
      {
        recipe += "\n[build]\n"
          "defines = " + format_sources(visual_studio_project->definitions) + "\n";
      }
    }

    if (!sibling_dependencies.empty() || !github_dependencies.empty())
    {
      recipe += "\n[dependencies]\n";

      for (const auto& dependency : sibling_dependencies)
      {
        recipe += dependency.name + " = { path = \""
          + escape_toml_string(dependency.path) + "\" }\n";
      }

      for (const auto& dependency : github_dependencies)
      {
        recipe += dependency.name + " = { git = \"" + escape_toml_string(dependency.git)
          + "\", commit = \"" + dependency.commit + "\" }\n";
      }
    }

    if (visual_studio_project)
    {
      for (const auto& [name, profile] : visual_studio_project->profiles)
      {
        recipe += "\n[profile." + escape_toml_string(name) + ".build]\n"
          "configuration = \"" + escape_toml_string(profile.configuration) + "\"\n";

        if (profile.cpp_standard != 0 && profile.cpp_standard != visual_studio_project->cpp_standard)
        {
          recipe += "cpp_std = " + std::to_string(profile.cpp_standard) + "\n";
        }

        if (!profile.include_directories.empty())
        {
          std::vector<std::string> includes;

          for (const auto& include : profile.include_directories)
          {
            includes.push_back(include.generic_string());
          }

          recipe += "include_dirs = " + format_sources(includes) + "\n";
        }

        if (!profile.compile_definitions.empty())
        {
          recipe += "defines = " + format_sources(profile.compile_definitions) + "\n";
        }
      }
    }

    if (show_progress)
    {
      report_progress(output, 5, 6, "Writing recipe");
    }

    if (!write_file(recipe_path, recipe, error))
    {
      return 2;
    }

    if (show_progress)
    {
      report_progress(output, 6, 6, "Creating release support");
    }

    if (!generate_github_release_support(project_directory, error))
    {
      return 2;
    }

    output
      << "Adopted project '" << project_name << "'\n"
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

    if (visual_studio_project)
    {
      output << "Imported " << visual_studio_project->format << " project "
             << visual_studio_project->path.filename().string()
             << '\n';

      for (const auto& format : merged_project_formats)
      {
        output << "Merged mirrored " << format << " project metadata\n";
      }

      if (!visual_studio_project->profiles.empty())
      {
        output << "Imported " << visual_studio_project->profiles.size()
               << ' ' << visual_studio_project->format << " build profile";
        output << (visual_studio_project->profiles.size() == 1 ? "\n" : "s\n");
      }

      if (!visual_studio_project->unresolved_properties.empty())
      {
        output << "Skipped " << visual_studio_project->unresolved_properties.size()
               << " unresolved " << visual_studio_project->format << " value";
        output << (visual_studio_project->unresolved_properties.size() == 1 ? ":\n" : "s:\n");

        for (const auto& value : visual_studio_project->unresolved_properties)
        {
          output << "  " << value << '\n';
        }
      }
    }

    if (!include_directories.empty())
    {
      output << "Inferred " << include_directories.size() << " local include director";
      output << (include_directories.size() == 1 ? "y\n" : "ies\n");
    }

    if (!unresolved.empty())
    {
      output << "Found " << unresolved.size() << " unresolved dependency include";
      output << (unresolved.size() == 1 ? ":\n" : "s:\n");

      for (const auto& [include, source] : unresolved)
      {
        output << "  <" << include << "> from " << source << '\n';
      }
    }

    if (!sibling_dependencies.empty())
    {
      output << "Inferred " << sibling_dependencies.size() << " sibling project dependenc";
      output << (sibling_dependencies.size() == 1 ? "y:\n" : "ies:\n");

      for (const auto& dependency : sibling_dependencies)
      {
        output << "  " << dependency.name << " = " << dependency.path << '\n';
      }
    }

    if (!options.github && !suggestions.empty())
    {
      output << "Suggested GitHub dependencies:\n";

      for (const auto& [repository, includes] : suggestions)
      {
        output << "  " << repository << " for";

        for (const auto& include : includes)
        {
          output << " <" << include << ">";
        }

        output << '\n';
      }

      output << "Run 'forge adopt --github' to verify and pin suggestions\n";
    }

    if (entry_points.empty() && !sources.empty() && public_headers.empty())
    {
      output << "Could not infer a library interface; generated an executable recipe\n";
    }

    return 0;
  }

  int init_project(const std::filesystem::path& project_directory,
                   std::ostream& output,
                   std::ostream& error)
  {
    return adopt_project(project_directory, AdoptOptions {}, run_process, output, error);
  }

} // namespace forge
