#include "init_cmake.h"

#include "recipe.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <iterator>
#include <map>
#include <regex>
#include <set>
#include <string>
#include <string_view>

namespace forge
{
  namespace
  {

    struct CMakeCommand
    {
      std::string name;
      std::vector<std::string> arguments;
    };

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
            argument += value[++index];
          else if (character == quote)
            quote = '\0';
          else
            argument += character;

          continue;
        }

        if (character == '"' || character == '\'')
          quote = character;
        else if (character == '#')
        {
          while (index < value.size() && value[index] != '\n')
            ++index;
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
          argument += character;
      }

      if (!argument.empty())
        arguments.push_back(std::move(argument));

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
            ++position;

          continue;
        }

        while (position < contents.size()
               && !std::isalpha(static_cast<unsigned char>(contents[position]))
               && contents[position] != '_')
        {
          if (contents[position] == '#')
          {
            while (position < contents.size() && contents[position] != '\n')
              ++position;
          }
          else
            ++position;
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
          continue;

        const auto arguments_begin = ++position;
        std::size_t depth = 1;
        char quote = '\0';

        while (position < contents.size() && depth != 0)
        {
          const auto character = contents[position];

          if (quote != '\0')
          {
            if (character == '\\')
              ++position;
            else if (character == quote)
              quote = '\0';
          }
          else if (character == '"' || character == '\'')
            quote = character;
          else if (character == '(')
            ++depth;
          else if (character == ')')
            --depth;

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

    bool looks_like_semantic_version(std::string_view value)
    {
      static const std::regex pattern {
        R"regex((0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)(-[0-9A-Za-z-]+(\.[0-9A-Za-z-]+)*)?)regex"
      };
      return std::regex_match(value.begin(), value.end(), pattern);
    }

    std::optional<std::string> cmake_build_interface_value(std::string_view value)
    {
      constexpr std::string_view prefix { "$<BUILD_INTERFACE:" };

      if (!value.starts_with(prefix) || !value.ends_with('>'))
        return std::nullopt;

      return std::string { value.substr(prefix.size(), value.size() - prefix.size() - 1) };
    }

  } // namespace

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
      std::map<std::string, std::string> frameworks;
      std::map<std::string, std::string> pkg_config_targets;
      std::string platform;

      for (const auto& command : cmake_commands(contents))
      {
        if (command.name == "if" && !command.arguments.empty())
        {
          platform =
            command.arguments.front() == "APPLE" ? "macos"
            : command.arguments.front() == "WIN32" ? "windows"
            : command.arguments.front() == "UNIX" ? "linux"
            : "";
        }
        else if (command.name == "elseif" && !command.arguments.empty())
        {
          platform =
            command.arguments.front() == "APPLE" ? "macos"
            : command.arguments.front() == "WIN32" ? "windows"
            : command.arguments.front() == "UNIX" ? "linux"
            : "";
        }
        else if (command.name == "endif")
          platform.clear();
        else if (command.name == "find_library" && command.arguments.size() > 1)
          frameworks[command.arguments[0]] = command.arguments[1];
        else if (command.name == "pkg_check_modules" && command.arguments.size() > 1)
          pkg_config_targets["PkgConfig::" + command.arguments[0]] = command.arguments.back();
        else if (command.name == "target_link_libraries" && command.arguments.size() > 1)
        {
          for (std::size_t index = 1; index < command.arguments.size(); ++index)
          {
            const auto& argument = command.arguments[index];

            if (is_cmake_scope(argument))
              continue;

            if (argument.starts_with("${") && argument.ends_with('}'))
            {
              const auto variable = argument.substr(2, argument.size() - 3);

              if (platform == "macos" && frameworks.contains(variable))
                project.macos_frameworks.push_back(frameworks.at(variable));
            }
            else if (pkg_config_targets.contains(argument) && platform == "linux")
              project.linux_libraries.push_back(pkg_config_targets.at(argument));
            else if (argument.find('$') == std::string::npos
                     && argument.find("::") == std::string::npos)
            {
              auto* libraries =
                platform == "macos" ? &project.macos_libraries
                : platform == "linux" ? &project.linux_libraries
                : platform == "windows" ? &project.windows_libraries
                : nullptr;

              if (libraries)
                libraries->push_back(argument);
            }
          }
        }
        else if (command.name == "project" && !command.arguments.empty())
        {
          project.name = command.arguments.front();

          for (std::size_t index = 1; index + 1 < command.arguments.size(); ++index)
          {
            if (command.arguments[index] == "VERSION"
                && looks_like_semantic_version(command.arguments[index + 1]))
            {
              project.version = command.arguments[index + 1];
              break;
            }
          }
        }
        else if ((command.name == "add_executable" || command.name == "add_library")
                 && !command.arguments.empty())
        {
          if (command.name == "add_library"
              && command.arguments.size() > 1
              && command.arguments[1] == "ALIAS")
          {
            continue;
          }

          targets.insert(command.arguments.front());

          if (targets.size() == 1)
          {
            project.type = command.name == "add_executable" ? "executable" : "static_library";

            if (command.name == "add_library" && command.arguments.size() > 1)
            {
              if (command.arguments[1] == "SHARED" || command.arguments[1] == "MODULE")
                project.type = "dynamic_library";
              else if (command.arguments[1] == "INTERFACE")
                project.type = "header_only";
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
              project.unresolved_properties.push_back(argument);
            else if (const auto relative = project_relative_path(directory, expanded))
            {
              const auto extension = std::filesystem::path { *relative }.extension().string();

              if (extension == ".cpp" || extension == ".cc" || extension == ".cxx")
                project.sources.push_back(*relative);
              else if (extension == ".h" || extension == ".hpp" || extension == ".hh")
                project.headers.push_back(*relative);
            }
          }
        }
        else if (command.name == "target_compile_features" && command.arguments.size() > 1)
        {
          for (const auto& argument : command.arguments)
          {
            if (argument.starts_with("cxx_std_"))
              project.cpp_standard = std::stoi(argument.substr(8));
          }
        }
        else if (command.name == "target_include_directories" && command.arguments.size() > 1)
        {
          for (std::size_t index = 1; index < command.arguments.size(); ++index)
          {
            const auto& original_argument = command.arguments[index];

            if (is_cmake_scope(original_argument)
                || original_argument.starts_with("$<INSTALL_INTERFACE:"))
            {
              continue;
            }

            const auto build_interface = cmake_build_interface_value(original_argument);
            const auto& argument = build_interface ? *build_interface : original_argument;
            const auto expanded = replace_cmake_paths(argument, directory);

            if (expanded.find("${") != std::string::npos || expanded.find("$<") != std::string::npos)
              project.unresolved_properties.push_back(original_argument);
            else if (const auto relative = project_relative_path(directory, expanded))
              project.include_directories.push_back(*relative);
          }
        }
        else if (command.name == "target_compile_definitions" && command.arguments.size() > 1)
        {
          for (std::size_t index = 1; index < command.arguments.size(); ++index)
          {
            const auto& argument = command.arguments[index];

            if (!is_cmake_scope(argument) && is_valid_compile_definition(argument))
              project.definitions.push_back(argument);
            else if (argument.find('$') != std::string::npos)
              project.unresolved_properties.push_back(argument);
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
        &project.macos_frameworks,
        &project.macos_libraries,
        &project.linux_libraries,
        &project.windows_libraries,
        &project.unresolved_properties
      })
      {
        std::ranges::sort(*values);
        values->erase(std::unique(values->begin(), values->end()), values->end());
      }

      return project;
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
          continue;

        const auto& argument = command.arguments.front();

        if (argument.find('$') != std::string::npos)
          continue;

        const auto project = (cmake_path.parent_path() / argument).lexically_normal();

        if (std::filesystem::is_regular_file(project / "CMakeLists.txt"))
          projects.push_back(project);
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


} // namespace forge
