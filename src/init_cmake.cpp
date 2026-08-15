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
                                    const std::filesystem::path& project_directory,
                                    std::string_view project_name)
    {
      for (const auto variable : {
        std::string_view { "${CMAKE_CURRENT_SOURCE_DIR}" },
        std::string_view { "${CMAKE_CURRENT_LIST_DIR}" },
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

      // CMake defines <PROJECT-NAME>_SOURCE_DIR alongside PROJECT_SOURCE_DIR.
      // Projects commonly use this spelling in target include directories.
      const auto project_source_directory =
        "${" + std::string { project_name } + "_SOURCE_DIR}";
      std::size_t position = 0;
      const auto replacement = project_directory.generic_string();

      while ((position = value.find(project_source_directory, position)) != std::string::npos)
      {
        value.replace(position, project_source_directory.size(), replacement);
        position += replacement.size();
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

    std::string lowercase(std::string value)
    {
      std::ranges::transform(value, value.begin(), [](unsigned char character)
      {
        return static_cast<char>(std::tolower(character));
      });
      return value;
    }

    bool cmake_option_value(std::string_view value)
    {
      return value == "ON" || value == "TRUE" || value == "YES" || value == "1";
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
      const auto commands = cmake_commands(contents);
      int function_depth = 0;

      for (const auto& command : commands)
      {
        if (command.name == "function" || command.name == "macro")
        {
          ++function_depth;
          continue;
        }

        if (command.name == "endfunction" || command.name == "endmacro")
        {
          function_depth = std::max(0, function_depth - 1);
          continue;
        }

        if (function_depth != 0)
          continue;

        if (command.name == "project" && !command.arguments.empty())
        {
          project.name = command.arguments.front();
          break;
        }
      }

      std::map<std::string, bool> options;
      std::vector<CMakeCommand> declared_targets;
      function_depth = 0;

      for (const auto& command : commands)
      {
        if (command.name == "function" || command.name == "macro")
        {
          ++function_depth;
          continue;
        }

        if (command.name == "endfunction" || command.name == "endmacro")
        {
          function_depth = std::max(0, function_depth - 1);
          continue;
        }

        if (function_depth != 0)
          continue;

        if (command.name == "option" && command.arguments.size() > 2)
          options[command.arguments.front()] = cmake_option_value(command.arguments.back());
        else if (command.name == "add_executable" || command.name == "add_library")
        {
          if (command.arguments.empty()
              || command.arguments.front().find('$') != std::string::npos
              || (command.name == "add_library"
                  && command.arguments.size() > 1
                  && command.arguments[1] == "ALIAS"))
          {
            continue;
          }

          declared_targets.push_back(command);
        }
      }

      const auto project_target = std::ranges::find_if(
        declared_targets,
        [&project](const CMakeCommand& command)
        {
          return lowercase(command.arguments.front()) == lowercase(project.name);
        }
      );
      const auto selected_target = project_target != declared_targets.end()
        ? std::optional<std::string> { project_target->arguments.front() }
        : declared_targets.size() == 1
        ? std::optional<std::string> { declared_targets.front().arguments.front() }
        : std::optional<std::string> {};
      const auto target_is_selected = [&selected_target](const CMakeCommand& command)
      {
        return !selected_target
          || (!command.arguments.empty() && command.arguments.front() == *selected_target);
      };
      const auto target_type = [](const CMakeCommand& command)
      {
        if (command.name == "add_executable")
          return std::string { "executable" };

        if (command.arguments.size() > 1
            && (command.arguments[1] == "SHARED" || command.arguments[1] == "MODULE"))
        {
          return std::string { "dynamic_library" };
        }

        return command.arguments.size() > 1 && command.arguments[1] == "INTERFACE"
          ? std::string { "header_only" }
          : std::string { "static_library" };
      };

      if (selected_target)
      {
        const auto selected = std::ranges::find_if(
          declared_targets,
          [&selected_target](const CMakeCommand& command)
          {
            return command.arguments.front() == *selected_target;
          }
        );
        project.type = target_type(*selected);
      }

      std::map<std::string, std::string> frameworks;
      std::map<std::string, std::string> pkg_config_targets;
      const auto add_known_system_package = [&project](std::string_view package)
      {
        if (package == "SDL2")
        {
          project.macos_libraries.push_back("SDL2");
          project.macos_brew_packages.push_back("sdl2-compat");
          project.linux_libraries.push_back("SDL2");
          project.linux_apt_packages.push_back("libsdl2-dev");
          project.windows_libraries.push_back("SDL2");
        }
      };
      std::string platform;
      struct ConditionalScope
      {
        bool parent_active;
        bool branch_matched;
      };
      std::vector<ConditionalScope> conditional_scopes;
      bool active = true;
      function_depth = 0;
      std::map<std::string, std::vector<std::string>> variables;

      const auto condition_value = [&options](const std::vector<std::string>& arguments)
      {
        if (arguments.empty())
          return false;

        const auto negated = arguments.front() == "NOT";
        if (negated && arguments.size() == 1)
          return false;

        const auto& name = arguments[negated ? 1 : 0];
        const auto value = options.contains(name) && options.at(name);
        return negated ? !value : value;
      };
      const auto expand_argument = [&variables](std::string_view argument)
      {
        if (argument.starts_with("${") && argument.ends_with('}'))
        {
          const auto name = std::string { argument.substr(2, argument.size() - 3) };

          if (variables.contains(name))
            return variables.at(name);
        }

        return std::vector<std::string> { std::string { argument } };
      };
      const auto add_target_path = [&project, &directory](std::string_view argument)
      {
        const auto expanded = replace_cmake_paths(std::string { argument }, directory, project.name);

        if (expanded.find("${") != std::string::npos || expanded.find("$<") != std::string::npos)
        {
          project.unresolved_properties.push_back(std::string { argument });
          return;
        }

        const auto relative = project_relative_path(directory, expanded);

        if (!relative)
          return;

        const auto extension = std::filesystem::path { *relative }.extension().string();

        if (extension == ".cpp" || extension == ".cc" || extension == ".cxx")
          project.sources.push_back(*relative);
        else if (extension == ".h" || extension == ".hpp" || extension == ".hh")
          project.headers.push_back(*relative);
      };

      for (const auto& command : commands)
      {
        if (command.name == "function" || command.name == "macro")
        {
          ++function_depth;
          continue;
        }
        else if (command.name == "endfunction" || command.name == "endmacro")
        {
          function_depth = std::max(0, function_depth - 1);
          continue;
        }

        if (function_depth != 0)
          continue;

        if (command.name == "if")
        {
          const auto value = condition_value(command.arguments);
          conditional_scopes.push_back({ active, value });
          active = active && value;
          platform =
            !command.arguments.empty() && command.arguments.front() == "APPLE" ? "macos"
            : !command.arguments.empty() && command.arguments.front() == "WIN32" ? "windows"
            : !command.arguments.empty() && command.arguments.front() == "UNIX" ? "linux"
            : "";
          continue;
        }
        else if (command.name == "elseif")
        {
          if (!conditional_scopes.empty())
          {
            auto& scope = conditional_scopes.back();
            const auto value = !scope.branch_matched && condition_value(command.arguments);
            scope.branch_matched = scope.branch_matched || value;
            active = scope.parent_active && value;
          }
          platform =
            !command.arguments.empty() && command.arguments.front() == "APPLE" ? "macos"
            : !command.arguments.empty() && command.arguments.front() == "WIN32" ? "windows"
            : !command.arguments.empty() && command.arguments.front() == "UNIX" ? "linux"
            : "";
          continue;
        }
        else if (command.name == "else")
        {
          if (!conditional_scopes.empty())
          {
            auto& scope = conditional_scopes.back();
            active = scope.parent_active && !scope.branch_matched;
            scope.branch_matched = true;
          }
          continue;
        }
        else if (command.name == "endif")
        {
          if (!conditional_scopes.empty())
          {
            active = conditional_scopes.back().parent_active;
            conditional_scopes.pop_back();
          }
          platform.clear();
          continue;
        }

        if (command.name == "set" && !command.arguments.empty() && active)
        {
          std::vector<std::string> values;

          for (std::size_t index = 1; index < command.arguments.size(); ++index)
          {
            if (command.arguments[index] == "CACHE" || command.arguments[index] == "PARENT_SCOPE")
              break;

            const auto expanded = expand_argument(command.arguments[index]);
            values.insert(values.end(), expanded.begin(), expanded.end());
          }

          variables[command.arguments.front()] = std::move(values);
        }
        else if (command.name == "list"
                 && command.arguments.size() > 2
                 && command.arguments[0] == "APPEND"
                 && active)
        {
          auto& values = variables[command.arguments[1]];

          for (std::size_t index = 2; index < command.arguments.size(); ++index)
          {
            const auto expanded = expand_argument(command.arguments[index]);
            values.insert(values.end(), expanded.begin(), expanded.end());
          }
        }
        else if (command.name == "add_subdirectory")
          project.has_cmake_subprojects = true;
        else if (command.name == "find_library" && command.arguments.size() > 1)
          frameworks[command.arguments[0]] = command.arguments[1];
        else if (command.name == "find_package" && !command.arguments.empty())
          add_known_system_package(command.arguments.front());
        else if (command.name == "pkg_check_modules" && command.arguments.size() > 1)
          pkg_config_targets["PkgConfig::" + command.arguments[0]] = command.arguments.back();
        else if (command.name == "target_link_libraries"
                 && command.arguments.size() > 1
                 && target_is_selected(command))
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

          if (!target_is_selected(command) || !active)
            continue;

          project.type = target_type(command);

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

            for (const auto& expanded : expand_argument(argument))
              add_target_path(expanded);
          }
        }
        else if (command.name == "target_sources"
                 && command.arguments.size() > 1
                 && target_is_selected(command)
                 && active)
        {
          for (std::size_t index = 1; index < command.arguments.size(); ++index)
          {
            const auto& argument = command.arguments[index];

            if (!is_cmake_scope(argument))
            {
              for (const auto& expanded : expand_argument(argument))
                add_target_path(expanded);
            }
          }
        }
        else if (command.name == "target_compile_features"
                 && command.arguments.size() > 1
                 && target_is_selected(command))
        {
          for (const auto& argument : command.arguments)
          {
            if (argument.starts_with("cxx_std_"))
              project.cpp_standard = std::stoi(argument.substr(8));
          }
        }
        else if (command.name == "target_include_directories"
                 && command.arguments.size() > 1
                 && target_is_selected(command))
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

            for (const auto& resolved : expand_argument(argument))
            {
              const auto expanded = replace_cmake_paths(resolved, directory, project.name);

              if (expanded.find("${") != std::string::npos || expanded.find("$<") != std::string::npos)
                project.unresolved_properties.push_back(original_argument);
              else if (const auto relative = project_relative_path(directory, expanded))
                project.include_directories.push_back(*relative);
            }
          }
        }
        else if (command.name == "target_compile_definitions"
                 && command.arguments.size() > 1
                 && target_is_selected(command)
                 && active)
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

      if (!selected_target && declared_targets.size() > 1)
      {
        project.type.clear();
        project.unresolved_properties.push_back(
          std::to_string(declared_targets.size()) + " CMake targets require inferred Forge targets"
        );
      }

      for (auto* values : {
        &project.sources,
        &project.headers,
        &project.include_directories,
        &project.definitions,
        &project.macos_frameworks,
        &project.macos_libraries,
        &project.macos_brew_packages,
        &project.linux_libraries,
        &project.linux_apt_packages,
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
