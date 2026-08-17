#include "init_cmake.h"

#include "recipe.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <functional>
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
      std::filesystem::path directory;
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

    std::vector<CMakeCommand> cmake_commands(
      std::string_view contents,
      const std::filesystem::path& directory = {})
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
            cmake_arguments(contents.substr(arguments_begin, position - arguments_begin - 1)),
            directory
          });
        }
      }

      return commands;
    }

    std::string replace_cmake_paths(std::string value,
                                    const std::filesystem::path& project_directory,
                                    const std::filesystem::path& current_directory,
                                    std::string_view project_name)
    {
      for (const auto variable : {
        std::string_view { "${CMAKE_CURRENT_SOURCE_DIR}" },
        std::string_view { "${CMAKE_CURRENT_LIST_DIR}" }
      })
      {
        std::size_t position = 0;
        const auto replacement = current_directory.generic_string();

        while ((position = value.find(variable, position)) != std::string::npos)
        {
          value.replace(position, variable.size(), replacement);
          position += replacement.size();
        }
      }

      for (const auto variable : {
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

    bool is_safe_runtime_destination(const std::filesystem::path& path)
    {
      if (path.empty() || path.is_absolute() || path.has_root_path())
        return false;

      for (const auto& component : path)
      {
        if (component.empty() || component == "." || component == "..")
          return false;
      }

      return true;
    }

    std::optional<std::filesystem::path> cmake_copy_directory_source(
      std::string_view value,
      const std::filesystem::path& directory,
      std::string_view project_name)
    {
      const auto expanded = replace_cmake_paths(std::string { value }, directory, directory, project_name);

      if (expanded.find("${") != std::string::npos || expanded.find("$<") != std::string::npos)
        return std::nullopt;

      if (const auto relative = project_relative_path(directory, expanded))
      {
        std::error_code error;

        if (std::filesystem::exists(directory / *relative, error) && !error)
          return std::filesystem::path { *relative };
      }

      // A component CMakeLists.txt may use CMAKE_SOURCE_DIR as though it were
      // configured by a superproject. During standalone adoption that variable
      // is mapped to the component directory, so recover a matching component
      // prefix when it names a file that actually belongs to this project.
      constexpr std::string_view source_directory { "${CMAKE_SOURCE_DIR}/" };

      if (!value.starts_with(source_directory))
        return std::nullopt;

      auto candidate = std::filesystem::path { value.substr(source_directory.size()) };

      if (!candidate.empty() && *candidate.begin() == directory.filename())
      {
        auto component = candidate.begin();
        ++component;
        std::filesystem::path stripped;

        for (; component != candidate.end(); ++component)
          stripped /= *component;

        candidate = std::move(stripped);
      }

      if (!is_safe_runtime_destination(candidate))
        return std::nullopt;

      std::error_code error;

      if (std::filesystem::exists(directory / candidate, error) && !error)
        return candidate;

      return std::nullopt;
    }

    std::optional<std::filesystem::path> cmake_copy_directory_destination(std::string_view value)
    {
      constexpr std::array prefixes {
        std::string_view { "$<CONFIG>/" },
        std::string_view { "${CMAKE_CURRENT_BINARY_DIR}/" },
        std::string_view { "${CMAKE_BINARY_DIR}/" }
      };

      for (const auto prefix : prefixes)
      {
        const auto position = value.find(prefix);

        if (position == std::string_view::npos)
          continue;

        const auto destination = std::filesystem::path { value.substr(position + prefix.size()) };

        if (is_safe_runtime_destination(destination))
          return destination;
      }

      return std::nullopt;
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

      if (!value.starts_with(prefix))
        return std::nullopt;

      const auto closing = value.find('>', prefix.size());

      if (closing == std::string_view::npos)
        return std::nullopt;

      return std::string { value.substr(prefix.size(), closing - prefix.size()) }
        + std::string { value.substr(closing + 1) };
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

    bool cmake_variable_value(std::string_view value)
    {
      const auto normalized = lowercase(std::string { value });
      return !normalized.empty()
        && normalized != "off"
        && normalized != "false"
        && normalized != "no"
        && normalized != "0"
        && normalized != "ignore"
        && !normalized.ends_with("-notfound");
    }

    std::vector<std::string> cmake_condition_tokens(const std::vector<std::string>& arguments)
    {
      std::vector<std::string> tokens;

      for (const auto& argument : arguments)
      {
        std::string token;

        for (const auto character : argument)
        {
          if (character == '(' || character == ')')
          {
            if (!token.empty())
            {
              tokens.push_back(std::move(token));
              token.clear();
            }

            tokens.emplace_back(1, character);
          }
          else
            token += character;
        }

        if (!token.empty())
          tokens.push_back(std::move(token));
      }

      return tokens;
    }

    bool cmake_glob_match(std::string_view value, std::string_view pattern)
    {
      std::string expression { "^" };

      for (const auto character : pattern)
      {
        if (character == '*')
          expression += ".*";
        else if (character == '?')
          expression += '.';
        else
        {
          if (std::string_view { R"(\\.^$|()[]{}+)" }.find(character) != std::string_view::npos)
            expression += '\\';

          expression += character;
        }
      }

      expression += '$';
      return std::regex_match(value.begin(), value.end(), std::regex { expression });
    }

    std::vector<std::string> cmake_glob_paths(const std::filesystem::path& project_directory,
                                              const std::filesystem::path& current_directory,
                                              std::string pattern,
                                              bool recursive,
                                              std::string_view project_name)
    {
      pattern = replace_cmake_paths(
        std::move(pattern), project_directory, current_directory, project_name
      );
      const auto path = std::filesystem::path { pattern };
      const auto root = path.is_absolute()
        ? path.parent_path()
        : current_directory / path.parent_path();
      const auto name = path.filename().string();
      std::vector<std::string> result;
      std::error_code error;

      const auto add = [&name, &result](const std::filesystem::directory_entry& entry)
      {
        std::error_code entry_error;

        if (!entry.is_regular_file(entry_error) || !cmake_glob_match(entry.path().filename().string(), name))
          return;

        result.push_back(entry.path().generic_string());
      };

      if (recursive)
      {
        for (std::filesystem::recursive_directory_iterator iterator {
               root,
               std::filesystem::directory_options::skip_permission_denied,
               error
             }, end;
             !error && iterator != end;
             iterator.increment(error))
        {
          add(*iterator);
        }
      }
      else
      {
        for (std::filesystem::directory_iterator iterator {
               root,
               std::filesystem::directory_options::skip_permission_denied,
               error
             }, end;
             !error && iterator != end;
             iterator.increment(error))
        {
          add(*iterator);
        }
      }

      std::ranges::sort(result);
      result.erase(std::unique(result.begin(), result.end()), result.end());
      return result;
    }

    bool is_cmake_dependency_directory(const std::filesystem::path& path)
    {
      for (const auto& component : path)
      {
        const auto name = lowercase(component.string());

        if (name == "deps" || name == "dep" || name == "vendor" || name == "external"
            || name == "third_party" || name == "third-party" || name == "submodules")
        {
          return true;
        }
      }

      return false;
    }

    std::vector<CMakeCommand> cmake_project_commands(
      const std::filesystem::path& cmake_path,
      std::ostream& error)
    {
      std::vector<CMakeCommand> result;
      std::set<std::filesystem::path> visited;

      const std::function<void(const std::filesystem::path&)> visit =
        [&](const std::filesystem::path& path)
      {
        const auto normalized = path.lexically_normal();

        if (!visited.insert(normalized).second)
          return;

        std::ifstream file { normalized };

        if (!file)
        {
          error << "forge: could not open CMake project '" << normalized.string() << "'\n";
          return;
        }

        const std::string contents {
          std::istreambuf_iterator<char> { file },
          std::istreambuf_iterator<char> {}
        };
        const auto commands = cmake_commands(contents, normalized.parent_path());

        for (const auto& command : commands)
        {
          result.push_back(command);

          if (command.name != "add_subdirectory" || command.arguments.empty())
            continue;

          const auto& argument = command.arguments.front();

          if (argument.find('$') != std::string::npos)
            continue;

          const auto subdirectory = (normalized.parent_path() / argument).lexically_normal();

          if (is_cmake_dependency_directory(subdirectory)
              || !std::filesystem::is_regular_file(subdirectory / "CMakeLists.txt"))
          {
            continue;
          }

          visit(subdirectory / "CMakeLists.txt");
        }
      };

      visit(cmake_path);
      return result;
    }

  } // namespace

  std::optional<VisualStudioProject> read_cmake_project(
      const std::filesystem::path& path,
      std::ostream& error,
      const std::optional<std::string>& requested_target)
    {
      std::ifstream file { path };

      if (!file)
      {
        error << "forge: could not open CMake project '" << path.string() << "'\n";
        return std::nullopt;
      }

      VisualStudioProject project;
      project.path = path;
      project.format = "CMake";
      project.name = path.parent_path().filename().string();
      const auto directory = path.parent_path();
      const auto commands = cmake_project_commands(path, error);
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
      const auto expanded_target_name = [&project](std::string_view value)
      {
        return value == "${PROJECT_NAME}" ? project.name : std::string { value };
      };
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
          const auto name = command.arguments.empty()
            ? std::string {}
            : expanded_target_name(command.arguments.front());

          if (command.arguments.empty()
              || name.find('$') != std::string::npos
              || (command.name == "add_library"
                  && command.arguments.size() > 1
                  && command.arguments[1] == "ALIAS"))
          {
            continue;
          }

          auto declared = command;
          declared.arguments.front() = std::move(name);
          declared_targets.push_back(std::move(declared));
        }
      }

      const auto project_target = std::ranges::find_if(
        declared_targets,
        [&project](const CMakeCommand& command)
        {
          return lowercase(command.arguments.front()) == lowercase(project.name);
        }
      );
      const auto requested = requested_target
        ? std::ranges::find_if(
            declared_targets,
            [&requested_target](const CMakeCommand& command)
            {
              return command.arguments.front() == *requested_target;
            }
          )
        : declared_targets.end();
      const auto selected_target = requested_target
        ? requested != declared_targets.end()
          ? std::optional<std::string> { requested->arguments.front() }
          : std::optional<std::string> {}
        : project_target != declared_targets.end()
        ? std::optional<std::string> { project_target->arguments.front() }
        : declared_targets.size() == 1
        ? std::optional<std::string> { declared_targets.front().arguments.front() }
        : std::optional<std::string> {};

      if (selected_target)
        project.cmake_target = *selected_target;
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
        const auto add_requirement = [&project](std::string_view macos_library,
                                                std::string_view macos_brew_package,
                                                std::string_view linux_library,
                                                std::string_view linux_apt_package,
                                                std::string_view windows_library)
        {
          if (!macos_library.empty())
            project.macos_libraries.emplace_back(macos_library);

          if (!macos_brew_package.empty())
            project.macos_brew_packages.emplace_back(macos_brew_package);

          if (!linux_library.empty())
            project.linux_libraries.emplace_back(linux_library);

          if (!linux_apt_package.empty())
            project.linux_apt_packages.emplace_back(linux_apt_package);

          if (!windows_library.empty())
            project.windows_libraries.emplace_back(windows_library);
        };

        if (package == "SDL2")
        {
          add_requirement("SDL2", "sdl2-compat", "SDL2", "libsdl2-dev", "SDL2");
        }
        else if (package == "glfw3")
        {
          add_requirement("glfw", "glfw", "glfw", "libglfw3-dev", "glfw3");
        }
        else if (package == "EnTT")
        {
          add_requirement({}, "entt", {}, "libentt-dev", {});
        }
        else if (package == "glm")
        {
          add_requirement({}, "glm", {}, "libglm-dev", {});
        }
        else if (package == "yaml-cpp")
        {
          add_requirement("yaml-cpp", "yaml-cpp", "yaml-cpp", "libyaml-cpp-dev", "yaml-cpp");
        }
        else if (package == "spdlog")
        {
          add_requirement("spdlog", "spdlog", "spdlog", "libspdlog-dev", "spdlog");
          add_requirement("fmt", "fmt", "fmt", "libfmt-dev", "fmt");
        }
        else if (package == "nativefiledialog-extended")
        {
          add_requirement("nfd", "nativefiledialog-extended", "nfd", {}, "nfd");
        }
      };
      const auto add_known_system_link = [&add_known_system_package](std::string_view library)
      {
        if (library == "nfd")
          add_known_system_package("nativefiledialog-extended");
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
      std::map<std::string, std::vector<std::string>> foreach_values;
      std::map<std::string, std::string> foreach_find_patterns;
      const auto target_is_selected = [
        &selected_target,
        &expanded_target_name,
        &foreach_values
      ](const CMakeCommand& command)
      {
        if (!selected_target)
          return true;

        if (command.arguments.empty())
          return false;

        const auto& target = command.arguments.front();

        if (target.starts_with("${") && target.ends_with('}'))
        {
          const auto variable = std::string { target.substr(2, target.size() - 3) };

          if (foreach_values.contains(variable))
          {
            return std::ranges::find(foreach_values.at(variable), *selected_target)
              != foreach_values.at(variable).end();
          }
        }

        return expanded_target_name(target) == *selected_target;
      };

      const auto condition_value = [&options, &variables, &foreach_find_patterns](
        const std::vector<std::string>& arguments
      )
      {
        const auto tokens = cmake_condition_tokens(arguments);
        std::size_t position = 0;

        const auto variable = [&options, &variables](std::string_view name)
        {
          if (name == "APPLE")
          {
#if defined(__APPLE__)
            return true;
#else
            return false;
#endif
          }

          if (name == "WIN32")
          {
#if defined(_WIN32)
            return true;
#else
            return false;
#endif
          }

          if (name == "UNIX")
          {
#if defined(__unix__) || defined(__APPLE__)
            return true;
#else
            return false;
#endif
          }

          if (name == "ON" || name == "TRUE" || name == "YES" || name == "1")
            return true;

          if (name == "OFF" || name == "FALSE" || name == "NO" || name == "0")
            return false;

          if (options.contains(std::string { name }))
            return options.at(std::string { name });

          if (variables.contains(std::string { name }) && !variables.at(std::string { name }).empty())
            return cmake_variable_value(variables.at(std::string { name }).front());

          return false;
        };
        std::function<bool()> expression;
        std::function<bool()> conjunction;
        std::function<bool()> unary;
        unary = [&]()
        {
          if (position >= tokens.size())
            return false;

          if (tokens[position] == "NOT")
          {
            ++position;
            return !unary();
          }

          if (tokens[position] == "(")
          {
            ++position;
            const auto value = expression();

            if (position < tokens.size() && tokens[position] == ")")
              ++position;

            return value;
          }

          if (tokens[position] == "DEFINED")
          {
            ++position;

            if (position >= tokens.size())
              return false;

            const auto& name = tokens[position++];
            return options.contains(name) || variables.contains(name);
          }

          const auto find_result = [](std::string_view name)
          {
            if (name.starts_with("${") && name.ends_with('}'))
              return std::string { name.substr(2, name.size() - 3) };

            return std::string { name };
          };
          const auto name = find_result(tokens[position]);

          if (foreach_find_patterns.contains(name)
              && position + 2 < tokens.size()
              && tokens[position + 1] == "EQUAL"
              && tokens[position + 2] == "-1")
          {
            position += 3;
            return false;
          }

          return variable(tokens[position++]);
        };
        conjunction = [&]()
        {
          auto value = unary();

          while (position < tokens.size() && tokens[position] == "AND")
          {
            ++position;
            value = unary() && value;
          }

          return value;
        };
        expression = [&]()
        {
          auto value = conjunction();

          while (position < tokens.size() && tokens[position] == "OR")
          {
            ++position;
            value = conjunction() || value;
          }

          return value;
        };

        return expression();
      };
      const auto expand_argument = [&project, &variables](std::string_view argument)
      {
        if (argument == "${PROJECT_NAME}")
          return std::vector<std::string> { project.name };

        if (argument.starts_with("${") && argument.ends_with('}'))
        {
          const auto name = std::string { argument.substr(2, argument.size() - 3) };

          if (variables.contains(name))
            return variables.at(name);
        }

        return std::vector<std::string> { std::string { argument } };
      };
      const auto add_target_path = [&project, &directory](
        std::string_view argument,
        const std::filesystem::path& current_directory)
      {
        const auto expanded = replace_cmake_paths(
          std::string { argument }, directory, current_directory, project.name
        );

        if (expanded.find("${") != std::string::npos || expanded.find("$<") != std::string::npos)
        {
          project.unresolved_properties.push_back(std::string { argument });
          return;
        }

        const auto target_path = std::filesystem::path { expanded }.is_absolute()
          ? std::filesystem::path { expanded }
          : current_directory / expanded;
        const auto relative = project_relative_path(directory, target_path);

        if (!relative)
          return;

        const auto extension = std::filesystem::path { *relative }.extension().string();

        if (extension == ".c" || extension == ".cpp" || extension == ".cc" || extension == ".cxx" || extension == ".mm")
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

          const auto read_standard = [&variables, &command](int& destination)
          {
            const auto& standard = variables[command.arguments.front()].front();
            const auto [end, error] = std::from_chars(
              standard.data(),
              standard.data() + standard.size(),
              destination
            );

            if (error != std::errc {} || end != standard.data() + standard.size())
              destination = 0;
          };

          if (command.arguments.front() == "CMAKE_CXX_STANDARD"
              && !variables[command.arguments.front()].empty())
          {
            read_standard(project.cpp_standard);
            if (project.cpp_standard == 0)
              project.cpp_standard = 20;
          }
          else if (command.arguments.front() == "CMAKE_C_STANDARD"
                   && !variables[command.arguments.front()].empty())
          {
            read_standard(project.c_standard);
          }
        }
        else if (command.name == "foreach" && command.arguments.size() > 1 && active)
        {
          auto& values = foreach_values[command.arguments.front()];
          values.clear();

          if (command.arguments.size() > 3
              && command.arguments[1] == "IN"
              && command.arguments[2] == "LISTS")
          {
            for (std::size_t index = 3; index < command.arguments.size(); ++index)
            {
              if (variables.contains(command.arguments[index]))
              {
                const auto& listed = variables.at(command.arguments[index]);
                values.insert(values.end(), listed.begin(), listed.end());
              }
            }

            continue;
          }

          for (std::size_t index = 1; index < command.arguments.size(); ++index)
          {
            const auto expanded = expand_argument(command.arguments[index]);
            values.insert(values.end(), expanded.begin(), expanded.end());
          }
        }
        else if (command.name == "endforeach")
        {
          if (!command.arguments.empty())
          {
            foreach_values.erase(command.arguments.front());
            foreach_find_patterns.erase(command.arguments.front());
          }
          else
          {
            foreach_values.clear();
            foreach_find_patterns.clear();
          }
        }
        else if (command.name == "string"
                 && command.arguments.size() > 3
                 && command.arguments.front() == "FIND"
                 && active)
        {
          const auto& haystack = command.arguments[1];

          if (haystack.starts_with("${") && haystack.ends_with('}'))
          {
            const auto variable = haystack.substr(2, haystack.size() - 3);

            if (foreach_values.contains(variable))
            {
              const auto patterns = expand_argument(command.arguments[2]);

              if (!patterns.empty())
                foreach_find_patterns[command.arguments[3]] = patterns.front();
            }
          }
        }
        else if (command.name == "list"
                 && command.arguments.size() > 2
                 && command.arguments.front() == "REMOVE_ITEM"
                 && active)
        {
          auto& values = variables[command.arguments[1]];

          for (std::size_t index = 2; index < command.arguments.size(); ++index)
          {
            const auto& item = command.arguments[index];

            if (!item.starts_with("${") || !item.ends_with('}'))
              continue;

            const auto variable = item.substr(2, item.size() - 3);

            if (!foreach_values.contains(variable) || foreach_find_patterns.empty())
              continue;

            for (const auto& [result, pattern] : foreach_find_patterns)
            {
              (void)result;
              std::erase_if(values, [&pattern](const std::string& value)
              {
                return value.find(pattern) != std::string::npos;
              });
            }
          }
        }
        else if (command.name == "file"
                 && command.arguments.size() > 2
                 && active
                 && (command.arguments.front() == "GLOB"
                     || command.arguments.front() == "GLOB_RECURSE"))
        {
          const auto recursive = command.arguments.front() == "GLOB_RECURSE";
          auto index = std::size_t { 2 };

          if (index < command.arguments.size() && command.arguments[index] == "CONFIGURE_DEPENDS")
            ++index;

          auto& values = variables[command.arguments[1]];

          for (; index < command.arguments.size(); ++index)
          {
            for (const auto& pattern : expand_argument(command.arguments[index]))
            {
              const auto paths = cmake_glob_paths(
                directory, command.directory, pattern, recursive, project.name
              );
              values.insert(values.end(), paths.begin(), paths.end());
            }
          }

          std::ranges::sort(values);
          values.erase(std::unique(values.begin(), values.end()), values.end());
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
        else if (command.name == "add_subdirectory" && active)
        {
          project.has_cmake_subprojects = true;

          if (command.directory == directory
              && !command.arguments.empty()
              && command.arguments.front().find('$') == std::string::npos)
          {
            const auto subdirectory = (directory / command.arguments.front()).lexically_normal();

            if (std::filesystem::is_regular_file(subdirectory / "CMakeLists.txt"))
            {
              if (const auto relative = project_relative_path(directory, subdirectory))
                project.cmake_subproject_directories.emplace_back(*relative);
            }
          }
        }
        else if ((command.name == "add_custom_target" || command.name == "add_custom_command")
                 && active)
        {
          for (std::size_t index = 0; index + 2 < command.arguments.size(); ++index)
          {
            if (command.arguments[index] != "copy_directory")
              continue;

            const auto source = cmake_copy_directory_source(
              command.arguments[index + 1], directory, project.name
            );
            const auto destination = cmake_copy_directory_destination(command.arguments[index + 2]);

            if (source && destination)
              project.runtime_files.push_back({ *source, *destination });
          }
        }
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

            if (platform == "macos" && argument.starts_with("-framework "))
            {
              const auto framework = argument.substr(std::string_view { "-framework " }.size());

              if (!framework.empty())
                project.macos_frameworks.push_back(framework);

              continue;
            }

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
              project.cmake_link_dependencies.push_back(argument);
              add_known_system_link(argument);

              auto* libraries =
                platform == "macos" ? &project.macos_libraries
                : platform == "linux" ? &project.linux_libraries
                : platform == "windows" ? &project.windows_libraries
                : nullptr;

              if (libraries)
                libraries->push_back(argument);
            }
            else if (argument.find('$') == std::string::npos)
              project.cmake_link_dependencies.push_back(argument);
          }
        }
        else if (command.name == "project"
                 && command.directory == directory
                 && !command.arguments.empty())
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
              add_target_path(expanded, command.directory);
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
                add_target_path(expanded, command.directory);
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
            else if (argument.starts_with("c_std_"))
              project.c_standard = std::stoi(argument.substr(6));
          }
        }
        else if (command.name == "target_include_directories"
                 && command.arguments.size() > 1
                 && target_is_selected(command))
        {
          bool public_include_directory = false;

          for (std::size_t index = 1; index < command.arguments.size(); ++index)
          {
            const auto& original_argument = command.arguments[index];

            if (is_cmake_scope(original_argument))
            {
              public_include_directory = original_argument == "PUBLIC"
                || original_argument == "INTERFACE";
              continue;
            }

            if (original_argument.starts_with("$<INSTALL_INTERFACE:"))
            {
              continue;
            }

            for (const auto& resolved : expand_argument(original_argument))
            {
              const auto build_interface = cmake_build_interface_value(resolved);
              const auto& argument = build_interface ? *build_interface : resolved;
              const auto expanded = replace_cmake_paths(
                argument, directory, command.directory, project.name
              );
              const auto include_path = std::filesystem::path { expanded }.is_absolute()
                ? std::filesystem::path { expanded }
                : command.directory / expanded;

              if (expanded.find("${") != std::string::npos || expanded.find("$<") != std::string::npos)
                project.unresolved_properties.push_back(original_argument);
              else if (const auto relative = project_relative_path(directory, include_path))
              {
                if (!std::filesystem::is_directory(directory / *relative))
                {
                  project.unresolved_properties.push_back(*relative);
                  continue;
                }

                project.include_directories.push_back(*relative);

                if (public_include_directory)
                  project.public_include_directories.push_back(*relative);
              }
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
        &project.public_include_directories,
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

      std::ranges::sort(
        project.runtime_files,
        {},
        [](const RuntimeFile& runtime_file)
        {
          return std::pair {
            runtime_file.source.generic_string(),
            runtime_file.destination.generic_string()
          };
        }
      );
      project.runtime_files.erase(
        std::unique(
          project.runtime_files.begin(),
          project.runtime_files.end(),
          [](const RuntimeFile& left, const RuntimeFile& right)
          {
            return left.source == right.source && left.destination == right.destination;
          }
        ),
        project.runtime_files.end()
      );

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
