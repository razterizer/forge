#include "new.h"

#include <fstream>
#include <string>
#include <system_error>

namespace forge
{
  namespace
  {

    bool is_valid_project_name(std::string_view project_name)
    {
      return
        !project_name.empty()
        && project_name != "."
        && project_name != ".."
        && project_name.find('/') == std::string_view::npos
        && project_name.find('\\') == std::string_view::npos;
    }

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

  int new_project(
    const std::filesystem::path& parent_directory,
    std::string_view project_name,
    std::ostream& output,
    std::ostream& error
  )
  {
    if (!is_valid_project_name(project_name))
    {
      error << "forge: project name must be a single directory name\n";
      return 2;
    }

    const auto project_directory = parent_directory / project_name;
    std::error_code filesystem_error;

    if (std::filesystem::exists(project_directory, filesystem_error))
    {
      error << "forge: '" << project_directory.string() << "' already exists\n";
      return 2;
    }

    if (filesystem_error)
    {
      error << "forge: could not inspect '" << project_directory.string() << "'\n";
      return 2;
    }

    if (!std::filesystem::create_directory(project_directory, filesystem_error))
    {
      error << "forge: could not create '" << project_directory.string() << "'\n";
      return 2;
    }

    const auto recipe_path = project_directory / "forge.recipe.toml";
    const auto main_path = project_directory / "main.cpp";
    const auto escaped_name = escape_toml_string(project_name);
    const std::string recipe =
      "[project]\n"
      "name = \"" + escaped_name + "\"\n"
      "version = \"0.1.0\"\n"
      "type = \"executable\"\n"
      "cpp_std = 20\n"
      "\n"
      "[sources]\n"
      "paths = [\"main.cpp\"]\n";

    constexpr std::string_view main_source =
      "#include <iostream>\n"
      "\n"
      "int main()\n"
      "{\n"
      "  std::cout << \"Hello from Forge!\\n\";\n"
      "}\n";

    if (!write_file(recipe_path, recipe, error) || !write_file(main_path, main_source, error))
    {
      std::filesystem::remove_all(project_directory, filesystem_error);
      return 2;
    }

    output
      << "Created project '" << project_name << "'\n"
      << "  " << recipe_path.string() << '\n'
      << "  " << main_path.string() << '\n';
    return 0;
  }

} // namespace forge

