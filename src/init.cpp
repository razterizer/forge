#include "init.h"

#include <fstream>
#include <string>
#include <string_view>
#include <system_error>

namespace forge
{
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

    bool write_file(
      const std::filesystem::path& path,
      std::string_view contents,
      std::ostream& error
    )
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

  } // namespace

  int init_project(
    const std::filesystem::path& project_directory,
    std::ostream& output,
    std::ostream& error
  )
  {
    const auto recipe_path = project_directory / "forge.recipe.toml";
    const auto source_directory = project_directory / "src";
    const auto main_path = source_directory / "main.cpp";

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

    if (std::filesystem::exists(main_path, filesystem_error))
    {
      error << "forge: '" << main_path.string() << "' already exists\n";
      return 2;
    }

    if (filesystem_error)
    {
      error << "forge: could not inspect '" << main_path.string() << "'\n";
      return 2;
    }

    std::filesystem::create_directories(source_directory, filesystem_error);

    if (filesystem_error)
    {
      error << "forge: could not create '" << source_directory.string() << "'\n";
      return 2;
    }

    const auto project_name = escape_toml_string(project_directory.filename().string());
    const std::string recipe =
      "[project]\n"
      "name = \"" + project_name + "\"\n"
      "version = \"0.1.0\"\n"
      "type = \"executable\"\n"
      "cpp_std = 20\n"
      "\n"
      "[sources]\n"
      "paths = [\"src/*.cpp\"]\n";

    constexpr std::string_view main_source =
      "#include <iostream>\n"
      "\n"
      "int main()\n"
      "{\n"
      "  std::cout << \"Hello from Forge!\\n\";\n"
      "}\n";

    if (!write_file(recipe_path, recipe, error) || !write_file(main_path, main_source, error))
    {
      return 2;
    }

    output
      << "Created " << recipe_path.string() << '\n'
      << "Created " << main_path.string() << '\n';
    return 0;
  }

} // namespace forge

