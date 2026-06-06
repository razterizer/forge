#include "release.h"

#include "build.h"
#include "recipe.h"

#include <array>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace forge
{
  namespace
  {

    bool is_safe_path_component(std::string_view value)
    {
      return
        !value.empty()
        && value != "."
        && value != ".."
        && value.find('/') == std::string_view::npos
        && value.find('\\') == std::string_view::npos;
    }

    bool copy_file(const std::filesystem::path& source,
                   const std::filesystem::path& destination,
                   std::ostream& error)
    {
      std::error_code filesystem_error;
      std::filesystem::copy_file(
        source,
        destination,
        std::filesystem::copy_options::overwrite_existing,
        filesystem_error
      );

      if (filesystem_error)
      {
        error << "forge: could not copy '" << source.string() << "'\n";
        return false;
      }

      return true;
    }

    bool copy_runtime_dependencies(const std::filesystem::path& source,
                                   const std::filesystem::path& destination,
                                   std::ostream& error)
    {
      if (!std::filesystem::is_directory(source))
      {
        return true;
      }

      std::error_code filesystem_error;
      std::filesystem::create_directories(destination, filesystem_error);

      if (filesystem_error)
      {
        error << "forge: could not create runtime dependency directory\n";
        return false;
      }

      for (const auto& entry : std::filesystem::directory_iterator { source, filesystem_error })
      {
        if (filesystem_error
            || !entry.is_regular_file()
            || !copy_file(entry.path(), destination / entry.path().filename(), error))
        {
          return false;
        }
      }

      return true;
    }

    bool copy_release_entry(const std::filesystem::path& source,
                            const std::filesystem::path& destination,
                            std::ostream& error)
    {
      if (source.filename() == ".forge" || source.filename() == ".git")
      {
        return true;
      }

      std::error_code filesystem_error;

      if (std::filesystem::is_symlink(source, filesystem_error))
      {
        error << "forge: release files cannot contain symbolic links\n";
        return false;
      }

      if (std::filesystem::is_regular_file(source, filesystem_error))
      {
        std::filesystem::create_directories(destination.parent_path(), filesystem_error);

        if (filesystem_error)
        {
          error << "forge: could not create release directory\n";
          return false;
        }

        return copy_file(source, destination, error);
      }

      if (!std::filesystem::is_directory(source, filesystem_error))
      {
        error << "forge: release file '" << source.string() << "' does not exist\n";
        return false;
      }

      std::filesystem::create_directories(destination, filesystem_error);

      if (filesystem_error)
      {
        error << "forge: could not create release directory\n";
        return false;
      }

      for (const auto& entry : std::filesystem::directory_iterator { source, filesystem_error })
      {
        if (filesystem_error
            || !copy_release_entry(entry.path(), destination / entry.path().filename(), error))
        {
          return false;
        }
      }

      if (filesystem_error)
      {
        error << "forge: could not inspect release directory\n";
        return false;
      }

      return true;
    }

    std::string_view trim(std::string_view value)
    {
      const auto first = value.find_first_not_of(" \t\r");

      if (first == std::string_view::npos)
      {
        return {};
      }

      return value.substr(first, value.find_last_not_of(" \t\r") - first + 1);
    }

    bool is_release_heading(std::string_view line, std::string_view version)
    {
      line = trim(line);

      if (!line.starts_with("##"))
      {
        return false;
      }

      line.remove_prefix(2);
      return trim(line) == version;
    }

    bool is_section_heading(std::string_view line)
    {
      line = trim(line);
      return line.starts_with("##") && !line.starts_with("###");
    }

    bool extract_release_notes(const std::filesystem::path& source,
                               std::string_view version,
                               std::optional<std::string>& notes,
                               std::ostream& error)
    {
      if (!std::filesystem::is_regular_file(source))
      {
        return true;
      }

      std::ifstream input { source };

      if (!input)
      {
        error << "forge: could not read '" << source.string() << "'\n";
        return false;
      }

      std::ostringstream extracted;
      std::string line;
      bool found = false;

      while (std::getline(input, line))
      {
        if (!found)
        {
          found = is_release_heading(line, version);
          continue;
        }

        if (is_section_heading(line))
        {
          break;
        }

        extracted << line << '\n';
      }

      if (!found)
      {
        error
          << "forge: release notes for version '" << version
          << "' were not found in '" << source.string() << "'\n";
        return false;
      }

      notes = extracted.str();
      return true;
    }

    bool write_release_notes(const std::filesystem::path& path,
                             const std::string& notes,
                             std::ostream& error)
    {
      std::ofstream output { path };

      if (!output)
      {
        error << "forge: could not write '" << path.string() << "'\n";
        return false;
      }

      output << notes;

      if (!output)
      {
        error << "forge: could not write '" << path.string() << "'\n";
        return false;
      }

      return true;
    }

  } // namespace

  int release_project(const std::filesystem::path& project_directory,
                      std::ostream& output,
                      std::ostream& error)
  {
    return release_project(project_directory, run_process, output, error);
  }

  int release_project(const std::filesystem::path& project_directory,
                      const ProcessRunner& process_runner,
                      std::ostream& output,
                      std::ostream& error)
  {
    if (build_project(project_directory, process_runner, output, error) != 0)
    {
      return 2;
    }

    Recipe recipe;

    if (!read_recipe(project_directory / "forge.recipe.toml", recipe, error))
    {
      return 2;
    }

    if (!is_safe_path_component(recipe.name) || !is_safe_path_component(recipe.version))
    {
      error << "forge: project name and version must be safe path components\n";
      return 2;
    }

    auto executable = project_directory / ".forge" / "build" / recipe.name;

#ifdef _WIN32
    executable += ".exe";
#endif

    if (!std::filesystem::is_regular_file(executable))
    {
      error << "forge: built executable '" << executable.string() << "' does not exist\n";
      return 2;
    }

    const auto package_name = recipe.name + "-" + recipe.version;
    const auto release_directory = project_directory / ".forge" / "release";
    const auto staging_directory = release_directory / package_name;
    const auto archive_path = release_directory / (package_name + ".zip");
    std::error_code filesystem_error;
    std::filesystem::remove_all(staging_directory, filesystem_error);
    filesystem_error.clear();
    std::filesystem::remove(archive_path, filesystem_error);
    filesystem_error.clear();
    std::filesystem::create_directories(staging_directory, filesystem_error);

    if (filesystem_error)
    {
      error << "forge: could not create '" << staging_directory.string() << "'\n";
      return 2;
    }

    if (!copy_file(executable, staging_directory / executable.filename(), error))
    {
      return 2;
    }

    if (!copy_runtime_dependencies(
      project_directory / ".forge" / "build" / "runtime",
      staging_directory / "runtime",
      error
    ))
    {
      return 2;
    }

    constexpr std::array optional_files {
      std::string_view { "README.md" },
      std::string_view { "LICENSE" }
    };

    for (const auto filename : optional_files)
    {
      const auto source = project_directory / filename;

      if (std::filesystem::is_regular_file(source)
          && !copy_file(source, staging_directory / filename, error))
      {
        return 2;
      }
    }

    for (const auto& release_file : recipe.release_files)
    {
      if (release_file.empty()
          || release_file.is_absolute()
          || release_file.string().starts_with(".."))
      {
        error << "forge: release file paths must stay inside the project\n";
        return 2;
      }

      if (!copy_release_entry(
        project_directory / release_file,
        staging_directory / release_file,
        error
      ))
      {
        return 2;
      }
    }

    std::optional<std::string> release_notes;

    if (!extract_release_notes(
      project_directory / "RELEASE_NOTES.md",
      recipe.version,
      release_notes,
      error
    ))
    {
      return 2;
    }

    if (release_notes
        && (!write_release_notes(staging_directory / "RELEASE_NOTES.md", *release_notes, error)
            || !write_release_notes(release_directory / "RELEASE_NOTES.md", *release_notes, error)))
    {
      return 2;
    }

    output << "Packaging " << package_name << '\n' << std::flush;

    const std::vector<std::string> archive_arguments {
      "cmake",
      "-E",
      "tar",
      "cf",
      archive_path.string(),
      "--format=zip",
      package_name
    };

    if (process_runner(archive_arguments, release_directory, error) != 0)
    {
      error << "forge: release archive creation failed\n";
      return 2;
    }

    output << "Released " << archive_path.string() << '\n';
    return 0;
  }

} // namespace forge
