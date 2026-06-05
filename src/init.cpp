#include "init.h"

#include <algorithm>
#include <fstream>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

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

    bool discover_sources(const std::filesystem::path& project_directory,
                          std::vector<std::string>& sources,
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
          sources.push_back(entry.path().lexically_relative(project_directory).generic_string());
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
      return true;
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

  } // namespace

  int init_project(const std::filesystem::path& project_directory,
                   std::ostream& output,
                   std::ostream& error)
  {
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

    if (!discover_sources(project_directory, sources, error))
    {
      return 2;
    }

    const auto project_name = escape_toml_string(project_directory.filename().string());
    const auto formatted_sources = format_sources(sources);
    const std::string recipe =
      "[project]\n"
      "name = \"" + project_name + "\"\n"
      "version = \"0.1.0\"\n"
      "type = \"executable\"\n"
      "cpp_std = 20\n"
      "\n"
      "[sources]\n"
      "paths = " + formatted_sources + "\n";

    if (!write_file(recipe_path, recipe, error))
    {
      return 2;
    }

    output
      << "Created " << recipe_path.string() << '\n'
      << "Found " << sources.size() << " C++ source file";

    if (sources.size() != 1)
    {
      output << 's';
    }

    output << '\n';
    return 0;
  }

} // namespace forge
