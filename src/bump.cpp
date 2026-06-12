#include "bump.h"

#include "recipe.h"

#include <charconv>
#include <fstream>
#include <iterator>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>

namespace forge
{
  namespace
  {

    struct Version
    {
      int major = 0;
      int minor = 0;
      int patch = 0;
    };

    std::string_view trim(std::string_view value)
    {
      const auto first = value.find_first_not_of(" \t\r\n");

      if (first == std::string_view::npos)
      {
        return {};
      }

      return value.substr(first, value.find_last_not_of(" \t\r\n") - first + 1);
    }

    bool parse_number(std::string_view value, int& number)
    {
      const auto result = std::from_chars(value.data(), value.data() + value.size(), number);
      return result.ec == std::errc {}
        && result.ptr == value.data() + value.size()
        && number >= 0;
    }

    bool parse_version(std::string_view value, Version& version)
    {
      const auto first = value.find('.');
      const auto second = first == std::string_view::npos
        ? std::string_view::npos
        : value.find('.', first + 1);

      return first != std::string_view::npos
        && second != std::string_view::npos
        && value.find('.', second + 1) == std::string_view::npos
        && parse_number(value.substr(0, first), version.major)
        && parse_number(value.substr(first + 1, second - first - 1), version.minor)
        && parse_number(value.substr(second + 1), version.patch);
    }

    bool increment(int& value)
    {
      if (value == std::numeric_limits<int>::max())
      {
        return false;
      }

      ++value;
      return true;
    }

    std::string bumped_version(const Version& current,
                               std::string_view component,
                               std::ostream& error)
    {
      auto next = current;
      bool valid = false;

      if (component == "major")
      {
        valid = increment(next.major);
        next.minor = 0;
        next.patch = 0;
      }
      else if (component == "minor")
      {
        valid = increment(next.minor);
        next.patch = 0;
      }
      else if (component == "patch")
      {
        valid = increment(next.patch);
      }

      if (!valid)
      {
        error << "forge: version component cannot be incremented\n";
        return {};
      }

      return std::to_string(next.major)
        + "." + std::to_string(next.minor)
        + "." + std::to_string(next.patch);
    }

    bool read_file(const std::filesystem::path& path,
                   std::string& content,
                   std::ostream& error)
    {
      std::ifstream file { path };

      if (!file)
      {
        error << "forge: could not read '" << path.string() << "'\n";
        return false;
      }

      content = {
        std::istreambuf_iterator<char> { file },
        std::istreambuf_iterator<char> {}
      };
      return true;
    }

    bool replace_recipe_values(const std::string& content,
                               std::string_view version,
                               const std::optional<int>& build_number,
                               std::string& updated,
                               std::ostream& error)
    {
      updated.clear();
      std::string_view remaining = content;
      std::string section;
      bool replaced_version = false;
      bool replaced_build = !build_number;

      while (!remaining.empty())
      {
        const auto newline = remaining.find('\n');
        const auto line = remaining.substr(0, newline);
        const auto trimmed = trim(line);

        if (trimmed.starts_with("[") && trimmed.ends_with("]"))
        {
          section = std::string { trim(trimmed.substr(1, trimmed.size() - 2)) };
        }

        const auto equals = trimmed.find('=');
        const auto key = equals == std::string_view::npos
          ? std::string_view {}
          : trim(trimmed.substr(0, equals));

        if (section == "project" && key == "version")
        {
          updated += "version = \"" + std::string { version } + "\"";
          replaced_version = true;
        }
        else if (section == "build" && key == "number")
        {
          updated += "number = " + std::to_string(*build_number);
          replaced_build = true;
        }
        else
        {
          updated += line;
        }

        if (newline == std::string_view::npos)
        {
          break;
        }

        updated += '\n';
        remaining.remove_prefix(newline + 1);
      }

      if (!replaced_version || !replaced_build)
      {
        error << "forge: could not locate version fields in forge.recipe.toml\n";
        return false;
      }

      return true;
    }

    bool is_release_heading(std::string_view line, std::string_view version)
    {
      line = trim(line);

      if (!line.starts_with("##") || line.starts_with("###"))
      {
        return false;
      }

      return trim(line.substr(2)) == version;
    }

    bool prepare_release_notes(const std::optional<std::string>& existing,
                               std::string_view release_heading,
                               std::string& updated,
                               std::ostream& error)
    {
      const auto section =
        "## " + std::string { release_heading } + "\n\n"
        "- Describe changes.\n\n";

      if (!existing)
      {
        updated = "# Release notes\n\n" + section;
        return true;
      }

      std::string_view remaining = *existing;
      std::size_t insertion = existing->size();
      bool found_insertion = false;

      while (!remaining.empty())
      {
        const auto offset = existing->size() - remaining.size();
        const auto newline = remaining.find('\n');
        const auto line = remaining.substr(0, newline);

        if (is_release_heading(line, release_heading))
        {
          error << "forge: release notes for version '" << release_heading << "' already exist\n";
          return false;
        }

        if (!found_insertion
            && trim(line).starts_with("##")
            && !trim(line).starts_with("###"))
        {
          insertion = offset;
          found_insertion = true;
        }

        if (newline == std::string_view::npos)
        {
          break;
        }

        remaining.remove_prefix(newline + 1);
      }

      updated = existing->substr(0, insertion);

      if (!updated.empty() && updated.back() != '\n')
      {
        updated += '\n';
      }

      if (!updated.empty() && !updated.ends_with("\n\n"))
      {
        updated += '\n';
      }

      updated += section;
      updated += existing->substr(insertion);
      return true;
    }

    bool write_file(const std::filesystem::path& path,
                    const std::string& content,
                    std::ostream& error)
    {
      std::error_code filesystem_error;
      std::filesystem::create_directories(path.parent_path(), filesystem_error);

      if (filesystem_error)
      {
        error << "forge: could not create directory for '" << path.string() << "'\n";
        return false;
      }

      std::ofstream file { path };
      file << content;

      if (!file)
      {
        error << "forge: could not write '" << path.string() << "'\n";
        return false;
      }

      return true;
    }

    bool is_safe_project_path(const std::filesystem::path& path)
    {
      if (path.empty() || path.is_absolute())
      {
        return false;
      }

      for (const auto& component : path)
      {
        if (component == "..")
        {
          return false;
        }
      }

      return true;
    }

    std::string version_header(std::string_view prefix,
                               std::string_view version,
                               const Version& parsed,
                               const std::optional<int>& build_number)
    {
      const auto qualified_version =
        build_number
          ? std::string { version } + "." + std::to_string(*build_number)
          : std::string { version };
      const auto macro = std::string { prefix } + "_VERSION_";
      return
        "#pragma once\n"
        "#define " + macro + "STR \"" + qualified_version + "\"\n"
        "#define " + macro + "MAJOR " + std::to_string(parsed.major) + "\n"
        "#define " + macro + "MINOR " + std::to_string(parsed.minor) + "\n"
        "#define " + macro + "PATCH " + std::to_string(parsed.patch) + "\n"
        "#define " + macro + "BUILD " + std::to_string(build_number.value_or(0)) + "\n";
    }

    bool replace_files(const std::filesystem::path& recipe_path,
                       const std::string& recipe,
                       const std::filesystem::path& notes_path,
                       const std::string& notes,
                       std::ostream& error)
    {
      const auto recipe_temporary = recipe_path.string() + ".tmp";
      const auto notes_temporary = notes_path.string() + ".tmp";
      const auto recipe_backup = recipe_path.string() + ".bak";
      const auto notes_backup = notes_path.string() + ".bak";

      if (!write_file(recipe_temporary, recipe, error)
          || !write_file(notes_temporary, notes, error))
      {
        std::error_code filesystem_error;
        std::filesystem::remove(recipe_temporary, filesystem_error);
        std::filesystem::remove(notes_temporary, filesystem_error);
        return false;
      }

      std::error_code filesystem_error;
      std::filesystem::remove(recipe_backup, filesystem_error);
      filesystem_error.clear();
      std::filesystem::remove(notes_backup, filesystem_error);
      filesystem_error.clear();
      std::filesystem::rename(recipe_path, recipe_backup, filesystem_error);

      if (filesystem_error)
      {
        error << "forge: could not prepare forge.recipe.toml for replacement\n";
        return false;
      }

      const auto had_notes = std::filesystem::is_regular_file(notes_path);

      if (had_notes)
      {
        std::filesystem::rename(notes_path, notes_backup, filesystem_error);
      }

      if (filesystem_error)
      {
        std::filesystem::rename(recipe_backup, recipe_path, filesystem_error);
        error << "forge: could not prepare RELEASE_NOTES.md for replacement\n";
        return false;
      }

      std::filesystem::rename(recipe_temporary, recipe_path, filesystem_error);

      if (!filesystem_error)
      {
        std::filesystem::rename(notes_temporary, notes_path, filesystem_error);
      }

      if (filesystem_error)
      {
        std::error_code rollback_error;
        std::filesystem::remove(recipe_path, rollback_error);
        std::filesystem::rename(recipe_backup, recipe_path, rollback_error);
        std::filesystem::remove(notes_path, rollback_error);

        if (had_notes)
        {
          std::filesystem::rename(notes_backup, notes_path, rollback_error);
        }

        error << "forge: could not replace project version files\n";
        return false;
      }

      std::filesystem::remove(recipe_backup, filesystem_error);
      std::filesystem::remove(notes_backup, filesystem_error);
      return true;
    }

  } // namespace

  int bump_project(const std::filesystem::path& project_directory,
                   std::string_view component,
                   std::ostream& output,
                   std::ostream& error)
  {
    const auto recipe_path = project_directory / "forge.recipe.toml";
    const auto notes_path = project_directory / "RELEASE_NOTES.md";
    Recipe recipe;

    if (!read_recipe(recipe_path, recipe, error))
    {
      return 2;
    }

    Version current;

    if (!parse_version(recipe.version, current))
    {
      error << "forge: project version must use <major>.<minor>.<patch>\n";
      return 2;
    }

    if (!recipe.version_header_path.empty() && !is_safe_project_path(recipe.version_header_path))
    {
      error << "forge: version header path must stay inside the project\n";
      return 2;
    }

    const auto version = bumped_version(current, component, error);

    if (version.empty())
    {
      return 2;
    }

    std::optional<int> build_number = recipe.build_number;

    if (build_number && !increment(*build_number))
    {
      error << "forge: build number cannot be incremented\n";
      return 2;
    }

    std::string recipe_content;
    std::string updated_recipe;

    if (!read_file(recipe_path, recipe_content, error)
        || !replace_recipe_values(recipe_content, version, build_number, updated_recipe, error))
    {
      return 2;
    }

    std::optional<std::string> notes_content;

    if (std::filesystem::is_regular_file(notes_path))
    {
      notes_content.emplace();

      if (!read_file(notes_path, *notes_content, error))
      {
        return 2;
      }
    }

    std::string updated_notes;
    auto release_heading = version;

    if (recipe.release_notes_build_number_format && build_number)
    {
      release_heading += *recipe.release_notes_build_number_format == "dotted"
        ? "." + std::to_string(*build_number)
        : "+build." + std::to_string(*build_number);
    }

    if (!prepare_release_notes(notes_content, release_heading, updated_notes, error)
        || !replace_files(recipe_path, updated_recipe, notes_path, updated_notes, error))
    {
      return 2;
    }

    if (!recipe.version_header_path.empty())
    {
      Version parsed;
      parse_version(version, parsed);
      const auto header_path = project_directory / recipe.version_header_path;

      if (!write_file(
        header_path,
        version_header(recipe.version_header_prefix, version, parsed, build_number),
        error
      ))
      {
        return 2;
      }
    }

    output << "Bumped " << recipe.version << " to " << version;

    if (build_number)
    {
      output << " (build " << *build_number << ')';
    }

    output << '\n' << "Prepared RELEASE_NOTES.md section " << release_heading << '\n';

    if (!recipe.version_header_path.empty())
    {
      output << "Generated " << recipe.version_header_path.generic_string() << '\n';
    }

    return 0;
  }

} // namespace forge
