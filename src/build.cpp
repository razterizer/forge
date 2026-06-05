#include "build.h"

#include "process.h"
#include "recipe.h"

#include <fstream>
#include <string>
#include <system_error>
#include <vector>

namespace forge
{
  namespace
  {

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

    bool write_generated_cmake(
      const std::filesystem::path& path,
      const Recipe& recipe,
      std::ostream& error
    )
    {
      std::ofstream file { path };

      if (!file)
      {
        error << "forge: could not create '" << path.string() << "'\n";
        return false;
      }

      file
        << "cmake_minimum_required(VERSION 3.25)\n"
        << "project(forge_project LANGUAGES CXX)\n\n"
        << "add_executable(forge_project\n";

      for (const auto& source : recipe.sources)
      {
        file << "  \"${FORGE_PROJECT_ROOT}/" << escape_cmake(source.generic_string()) << "\"\n";
      }

      file
        << ")\n"
        << "target_compile_features(forge_project PRIVATE cxx_std_" << recipe.cpp_standard << ")\n"
        << "set_target_properties(forge_project PROPERTIES OUTPUT_NAME \""
        << escape_cmake(recipe.name) << "\")\n";

      if (!file)
      {
        error << "forge: could not write '" << path.string() << "'\n";
        return false;
      }

      return true;
    }

  } // namespace

  int build_project(
    const std::filesystem::path& project_directory,
    std::ostream& output,
    std::ostream& error
  )
  {
    Recipe recipe;

    if (!read_recipe(project_directory / "forge.recipe.toml", recipe, error))
    {
      return 2;
    }

    if (recipe.type != "executable")
    {
      error << "forge: build currently supports executable projects only\n";
      return 2;
    }

    if (recipe.sources.empty())
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

    const auto forge_directory = project_directory / ".forge";
    const auto generated_directory = forge_directory / "generated";
    const auto build_directory = forge_directory / "build";
    std::error_code filesystem_error;
    std::filesystem::create_directories(generated_directory, filesystem_error);

    if (filesystem_error)
    {
      error << "forge: could not create '" << generated_directory.string() << "'\n";
      return 2;
    }

    if (!write_generated_cmake(generated_directory / "CMakeLists.txt", recipe, error))
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

    if (run_process(configure_arguments, project_directory, error) != 0)
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

    if (run_process(build_arguments, project_directory, error) != 0)
    {
      error << "forge: build failed\n";
      return 2;
    }

    output << "Built " << (build_directory / recipe.name).string() << '\n';
    return 0;
  }

} // namespace forge
