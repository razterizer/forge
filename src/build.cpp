#include "build.h"

#include "box.h"
#include "process.h"
#include "recipe.h"

#include <algorithm>
#include <fstream>
#include <optional>
#include <set>
#include <string>
#include <system_error>
#include <vector>

namespace forge
{
  namespace
  {

    struct ResolvedDependency
    {
      std::string name;
      std::filesystem::path root;
      std::optional<std::filesystem::path> library;
    };

    std::string escape_cmake(std::string_view value)
    {
      std::string escaped;

      for (const char character : value)
      {
        if (character == '\\' || character == '"' || character == '$')
        {
          escaped += '\\';
        }

        escaped += character;
      }

      return escaped;
    }

    bool is_safe_dependency_name(std::string_view name)
    {
      return
        !name.empty()
        && name != "."
        && name != ".."
        && name.find('/') == std::string_view::npos
        && name.find('\\') == std::string_view::npos;
    }

    bool write_header_validation_sources(const std::filesystem::path& directory,
                                         const Recipe& recipe,
                                         std::ostream& error)
    {
      std::error_code filesystem_error;
      std::filesystem::remove_all(directory, filesystem_error);
      filesystem_error.clear();
      std::filesystem::create_directories(directory, filesystem_error);

      if (filesystem_error)
      {
        error << "forge: could not create '" << directory.string() << "'\n";
        return false;
      }

      for (std::size_t index = 0; index < recipe.public_headers.size(); ++index)
      {
        auto include_path = recipe.public_headers[index];
        include_path = include_path.lexically_relative("include");
        std::ofstream source { directory / ("header-" + std::to_string(index) + ".cpp") };

        if (!source)
        {
          error << "forge: could not create header validation source\n";
          return false;
        }

        source << "#include <" << include_path.generic_string() << ">\n";
      }

      return true;
    }

    bool write_generated_cmake(const std::filesystem::path& path,
                               const Recipe& recipe,
                               const std::vector<ResolvedDependency>& dependencies,
                               std::ostream& error)
    {
      std::ofstream file { path };

      if (!file)
      {
        error << "forge: could not create '" << path.string() << "'\n";
        return false;
      }

      file
        << "cmake_minimum_required(VERSION 3.25)\n"
        << "project(forge_project LANGUAGES CXX)\n\n";

      if (recipe.type == "static_library")
      {
        file << "add_library(forge_project STATIC\n";
      }
      else if (recipe.type == "header_only")
      {
        file << "add_library(forge_project OBJECT\n";

        for (std::size_t index = 0; index < recipe.public_headers.size(); ++index)
        {
          file << "  \"${CMAKE_CURRENT_SOURCE_DIR}/header-validation/header-"
               << index << ".cpp\"\n";
        }
      }
      else
      {
        file << "add_executable(forge_project\n";
      }

      for (const auto& source : recipe.sources)
      {
        file << "  \"${FORGE_PROJECT_ROOT}/" << escape_cmake(source.generic_string()) << "\"\n";
      }

      for (const auto& header : recipe.public_headers)
      {
        file << "  \"${FORGE_PROJECT_ROOT}/" << escape_cmake(header.generic_string()) << "\"\n";
      }

      file
        << ")\n"
        << "target_compile_features(forge_project PUBLIC cxx_std_" << recipe.cpp_standard << ")\n";

      if (!recipe.public_headers.empty())
      {
        file
          << "target_include_directories(forge_project PUBLIC \"${FORGE_PROJECT_ROOT}/include\")\n";
      }

      for (std::size_t index = 0; index < dependencies.size(); ++index)
      {
        const auto& dependency = dependencies[index];
        file
          << "target_include_directories(forge_project PRIVATE \""
          << escape_cmake((dependency.root / "include").string()) << "\")\n";

        if (dependency.library)
        {
          file
            << "add_library(forge_dependency_" << index << " STATIC IMPORTED)\n"
            << "set_target_properties(forge_dependency_" << index
            << " PROPERTIES IMPORTED_LOCATION \""
            << escape_cmake(dependency.library->string()) << "\")\n"
            << "target_link_libraries(forge_project PRIVATE forge_dependency_" << index << ")\n";
        }
      }

      file
        << "set_target_properties(forge_project PROPERTIES OUTPUT_NAME \""
        << escape_cmake(recipe.name) << "\")\n";

      if (!file)
      {
        error << "forge: could not write '" << path.string() << "'\n";
        return false;
      }

      return true;
    }

    std::filesystem::path find_dependency_box(const std::filesystem::path& dependency_directory,
                                              const Recipe& recipe,
                                              std::ostream& error)
    {
      const auto boxes_directory = dependency_directory / ".forge" / "boxes";
      const auto prefix = recipe.name + "-" + recipe.version;
      std::filesystem::path result;
      std::filesystem::file_time_type result_time;
      std::error_code filesystem_error;

      for (const auto& entry : std::filesystem::directory_iterator { boxes_directory, filesystem_error })
      {
        if (filesystem_error)
        {
          break;
        }

        const auto filename = entry.path().filename().string();

        if (!entry.is_regular_file()
            || entry.path().extension() != ".cbox"
            || !filename.starts_with(prefix)
            || (filename.size() > prefix.size()
                && filename[prefix.size()] != '-'
                && filename[prefix.size()] != '+'))
        {
          continue;
        }

        const auto modified = entry.last_write_time(filesystem_error);

        if (filesystem_error)
        {
          break;
        }

        if (result.empty() || modified > result_time)
        {
          result = entry.path();
          result_time = modified;
        }
      }

      if (filesystem_error || result.empty())
      {
        error << "forge: could not locate box for dependency '" << recipe.name << "'\n";
      }

      return result;
    }

    bool resolve_dependencies(const std::filesystem::path& project_directory,
                              const Recipe& recipe,
                              const ProcessRunner& process_runner,
                              std::vector<ResolvedDependency>& resolved,
                              std::ostream& output,
                              std::ostream& error)
    {
      const auto dependencies_directory = project_directory / ".forge" / "deps";
      std::set<std::filesystem::path> dependency_paths;
      std::set<std::string> dependency_names;

      for (const auto& dependency : recipe.dependencies)
      {
        if (!is_safe_dependency_name(dependency.name))
        {
          error << "forge: dependency names must be safe path components\n";
          return false;
        }

        if (!dependency_names.insert(dependency.name).second)
        {
          error << "forge: duplicate dependency name '" << dependency.name << "'\n";
          return false;
        }

        std::error_code filesystem_error;
        const auto dependency_directory =
          std::filesystem::weakly_canonical(project_directory / dependency.path, filesystem_error);

        if (filesystem_error || !std::filesystem::is_directory(dependency_directory))
        {
          error << "forge: dependency '" << dependency.name << "' path does not exist\n";
          return false;
        }

        const auto canonical_project = std::filesystem::weakly_canonical(
          project_directory,
          filesystem_error
        );

        if (filesystem_error
            || dependency_directory == canonical_project
            || !dependency_paths.insert(dependency_directory).second)
        {
          error << "forge: dependency paths must be unique and cannot reference the project itself\n";
          return false;
        }

        Recipe dependency_recipe;

        if (!read_recipe(dependency_directory / "forge.recipe.toml", dependency_recipe, error))
        {
          return false;
        }

        if (dependency_recipe.name != dependency.name)
        {
          error << "forge: dependency name '" << dependency.name << "' does not match recipe name '"
                << dependency_recipe.name << "'\n";
          return false;
        }

        if (dependency_recipe.type != "static_library" && dependency_recipe.type != "header_only")
        {
          error << "forge: dependency '" << dependency.name
                << "' must be a static_library or header_only project\n";
          return false;
        }

        if (!dependency_recipe.dependencies.empty())
        {
          error << "forge: transitive dependencies are not supported yet\n";
          return false;
        }

        output << "Resolving dependency " << dependency.name << '\n';

        if (create_box(dependency_directory, process_runner, output, error) != 0)
        {
          return false;
        }

        const auto box = find_dependency_box(dependency_directory, dependency_recipe, error);

        if (box.empty())
        {
          return false;
        }

        std::filesystem::create_directories(dependencies_directory, filesystem_error);

        if (filesystem_error)
        {
          error << "forge: could not create dependency directory\n";
          return false;
        }

        const auto extracted = dependencies_directory / box.stem();
        const auto destination = dependencies_directory / dependency.name;
        std::filesystem::remove_all(extracted, filesystem_error);
        filesystem_error.clear();
        std::filesystem::remove_all(destination, filesystem_error);
        filesystem_error.clear();

        if (extract_box(box, dependencies_directory, process_runner, output, error) != 0)
        {
          return false;
        }

        std::filesystem::rename(extracted, destination, filesystem_error);

        if (filesystem_error)
        {
          error << "forge: could not install dependency '" << dependency.name << "'\n";
          return false;
        }

        ResolvedDependency resolved_dependency
          {
            dependency.name,
            destination,
            std::nullopt
          };

        if (dependency_recipe.type == "static_library")
        {
#ifdef _WIN32
          resolved_dependency.library = destination / "lib" / (dependency.name + ".lib");
#else
          resolved_dependency.library = destination / "lib" / ("lib" + dependency.name + ".a");
#endif
        }

        resolved.push_back(std::move(resolved_dependency));
      }

      return true;
    }

  } // namespace

  int build_project(const std::filesystem::path& project_directory,
                    std::ostream& output,
                    std::ostream& error)
  {
    return build_project(project_directory, run_process, output, error);
  }

  int build_project(const std::filesystem::path& project_directory,
                    const ProcessRunner& process_runner,
                    std::ostream& output,
                    std::ostream& error)
  {
    Recipe recipe;

    if (!read_recipe(project_directory / "forge.recipe.toml", recipe, error))
    {
      return 2;
    }

    if (recipe.type != "executable"
        && recipe.type != "static_library"
        && recipe.type != "header_only")
    {
      error << "forge: unsupported project type '" << recipe.type << "'\n";
      return 2;
    }

    if (recipe.sources.empty() && recipe.type != "header_only")
    {
      error << "forge: recipe contains no source files\n";
      return 2;
    }

    for (const auto& source : recipe.sources)
    {
      if (source.is_absolute() || source.string().starts_with(".."))
      {
        error << "forge: source paths must stay inside the project\n";
        return 2;
      }

      if (!std::filesystem::is_regular_file(project_directory / source))
      {
        error << "forge: source file '" << source.generic_string() << "' does not exist\n";
        return 2;
      }
    }

    if ((recipe.type == "static_library" || recipe.type == "header_only")
        && recipe.public_headers.empty())
    {
      error << "forge: library projects require public headers\n";
      return 2;
    }

    if (recipe.type == "header_only" && !recipe.sources.empty())
    {
      error << "forge: header-only projects cannot declare source files\n";
      return 2;
    }

    if (recipe.type != "executable" && !recipe.dependencies.empty())
    {
      error << "forge: only executable projects can declare dependencies until transitive "
               "dependencies are supported\n";
      return 2;
    }

    for (const auto& header : recipe.public_headers)
    {
      if (header.is_absolute()
          || header.string().starts_with("..")
          || header.begin() == header.end()
          || header.begin()->string() != "include")
      {
        error << "forge: public header paths must stay under include/\n";
        return 2;
      }

      if (!std::filesystem::is_regular_file(project_directory / header))
      {
        error << "forge: public header '" << header.generic_string() << "' does not exist\n";
        return 2;
      }
    }

    const auto forge_directory = project_directory / ".forge";
    const auto generated_directory = forge_directory / "generated";
    const auto build_directory = forge_directory / "build";
    std::vector<ResolvedDependency> dependencies;

    if (!resolve_dependencies(
      project_directory,
      recipe,
      process_runner,
      dependencies,
      output,
      error
    ))
    {
      return 2;
    }

    std::error_code filesystem_error;
    std::filesystem::create_directories(generated_directory, filesystem_error);

    if (filesystem_error)
    {
      error << "forge: could not create '" << generated_directory.string() << "'\n";
      return 2;
    }

    if ((recipe.type == "header_only"
         && !write_header_validation_sources(
           generated_directory / "header-validation",
           recipe,
           error
         ))
        || !write_generated_cmake(
          generated_directory / "CMakeLists.txt",
          recipe,
          dependencies,
          error
        ))
    {
      return 2;
    }

    output << "Configuring " << recipe.name << '\n' << std::flush;

    const std::vector<std::string> configure_arguments {
      "cmake",
      "-S",
      generated_directory.string(),
      "-B",
      build_directory.string(),
      "-G",
      "Ninja",
      "-DCMAKE_BUILD_TYPE=Debug",
      "-DFORGE_PROJECT_ROOT=" + project_directory.string()
    };

    if (process_runner(configure_arguments, project_directory, error) != 0)
    {
      error << "forge: CMake configuration failed\n";
      return 2;
    }

    output << "Building " << recipe.name << '\n' << std::flush;

    const std::vector<std::string> build_arguments {
      "cmake",
      "--build",
      build_directory.string()
    };

    if (process_runner(build_arguments, project_directory, error) != 0)
    {
      error << "forge: build failed\n";
      return 2;
    }

    if (recipe.type == "header_only")
    {
      output << "Validated " << recipe.public_headers.size() << " public header";

      if (recipe.public_headers.size() != 1)
      {
        output << 's';
      }

      output << '\n';
      return 0;
    }

    auto artifact = build_directory / recipe.name;

    if (recipe.type == "static_library")
    {
#ifdef _WIN32
      artifact += ".lib";
#else
      artifact = build_directory / ("lib" + recipe.name + ".a");
#endif
    }

    output << "Built " << artifact.string() << '\n';
    return 0;
  }

} // namespace forge
