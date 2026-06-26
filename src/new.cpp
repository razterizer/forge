#include "new.h"

#include "file_support.h"
#include "github.h"
#include "recipe.h"
#include "versioning.h"

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
          escaped += '\\';

        escaped += character;
      }

      return escaped;
    }

  } // namespace

  int new_project(const std::filesystem::path& parent_directory,
                  std::string_view project_name,
                  std::ostream& output,
                  std::ostream& error)
  {
    return new_project(parent_directory, project_name, NewOptions {}, output, error);
  }

  int new_project(const std::filesystem::path& parent_directory,
                  std::string_view project_name,
                  const NewOptions& options,
                  std::ostream& output,
                  std::ostream& error)
  {
    if (!is_valid_project_name(project_name))
    {
      error << "forge: project name must be a single directory name\n";
      return 2;
    }

    const auto initial_version = options.initial_version
      ? parse_initial_version(*options.initial_version)
      : parse_initial_version("0.1.0");

    if (!initial_version)
    {
      error << "forge: initial version must use <major>.<minor>.<patch>[.<build>]\n";
      return 2;
    }

    if (options.version_header_path && !is_safe_project_path(*options.version_header_path))
    {
      error << "forge: version header path must stay inside the project\n";
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
    std::string recipe =
      "#:schema " + std::string { recipe_schema_url } + "\n"
      "\n"
      "[project]\n"
      "name = \"" + escaped_name + "\"\n"
      "version = \"" + initial_version->version + "\"\n"
      "type = \"executable\"\n"
      "cpp_std = 20\n"
      "\n"
      "[sources]\n"
      "paths = [\"main.cpp\"]\n";

    if (initial_version->build_number)
    {
      recipe += "\n[build]\nnumber = " + std::to_string(*initial_version->build_number)
        + "\n\n[release]\nbuild_number_format = \"dotted\"\n";
    }

    if (options.version_header_path)
    {
      recipe += "\n[version_header]\npath = \""
        + escape_toml_string(options.version_header_path->generic_string())
        + "\"\nprefix = \"" + version_macro_prefix(project_name) + "\"\n";
    }

    recipe += "\n[profile.workflow-release.build]\nconfiguration = \"Release\"\n";

    constexpr std::string_view main_source =
      "#include <iostream>\n"
      "\n"
      "int main()\n"
      "{\n"
      "  std::cout << \"Hello from Forge!\\n\";\n"
      "}\n";

    const auto version_header_path = options.version_header_path
      ? project_directory / *options.version_header_path
      : std::filesystem::path {};

    if (!write_file(recipe_path, recipe, error, true)
        || !write_file(main_path, main_source, error, true)
        || (options.version_header_path
            && !write_file(
              version_header_path,
              generated_version_header(version_macro_prefix(project_name), *initial_version),
              error,
              true
            ))
        || !generate_github_release_support(
          project_directory,
          qualified_initial_version(*initial_version),
          error
        ))
    {
      std::filesystem::remove_all(project_directory, filesystem_error);
      return 2;
    }

    output
      << "Created project '" << project_name << "'\n"
      << "  " << recipe_path.string() << '\n'
      << "  " << main_path.string() << '\n';

    if (options.version_header_path)
      output << "  " << version_header_path.string() << '\n';

    return 0;
  }

} // namespace forge
