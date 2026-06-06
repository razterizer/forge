#include "release.h"

#include "build.h"
#include "recipe.h"

#include <array>
#include <chrono>
#include <ctime>
#include <fstream>
#include <iomanip>
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

    std::string target()
    {
      std::string os;
      std::string architecture;

#ifdef _WIN32
      os = "windows";
#elif __APPLE__
      os = "macos";
#elif __linux__
      os = "linux";
#else
      os = "unknown";
#endif

#if defined(__aarch64__) || defined(_M_ARM64)
      architecture = "arm64";
#elif defined(__x86_64__) || defined(_M_X64)
      architecture = "x86_64";
#elif defined(__i386__) || defined(_M_IX86)
      architecture = "x86";
#else
      architecture = "unknown";
#endif

      return os + "-" + architecture;
    }

    std::string current_date()
    {
      const auto now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
      std::tm utc {};

#ifdef _WIN32
      gmtime_s(&utc, &now);
#else
      gmtime_r(&now, &utc);
#endif

      std::ostringstream date;
      date << std::put_time(&utc, "%Y-%m-%d");
      return date.str();
    }

    void replace_placeholder(std::string& value,
                             std::string_view placeholder,
                             std::string_view replacement)
    {
      std::size_t position = 0;

      while ((position = value.find(placeholder, position)) != std::string::npos)
      {
        value.replace(position, placeholder.size(), replacement);
        position += replacement.size();
      }
    }

    bool expand_tag_format(std::string_view format,
                           const Recipe& recipe,
                           std::string& tag,
                           std::ostream& error)
    {
      tag = format;
      replace_placeholder(tag, "<name>", recipe.name);
      replace_placeholder(tag, "<version>", recipe.version);
      replace_placeholder(tag, "<curr-date>", current_date());
      replace_placeholder(tag, "<target>", target());
      replace_placeholder(tag, "<configuration>", "release");

      if (tag.find("<build-nr>") != std::string::npos)
      {
        if (!recipe.build_number)
        {
          error << "forge: tag format uses <build-nr>, but the recipe has no build number\n";
          return false;
        }

        replace_placeholder(tag, "<build-nr>", std::to_string(*recipe.build_number));
      }

      if (tag.empty()
          || tag.find('<') != std::string::npos
          || tag.find('>') != std::string::npos)
      {
        error << "forge: tag format contains an unknown or incomplete placeholder\n";
        return false;
      }

      return true;
    }

    bool preflight_tag(const std::filesystem::path& project_directory,
                       std::string_view tag,
                       bool force_tag,
                       const ProcessRunner& process_runner,
                       std::ostream& error)
    {
      if (process_runner({ "git", "check-ref-format", "refs/tags/" + std::string { tag } },
                         project_directory,
                         error) != 0)
      {
        error << "forge: expanded tag '" << tag << "' is not a valid Git tag\n";
        return false;
      }

      if (process_runner(
        { "git", "cat-file", "-e", "HEAD^{commit}" },
        project_directory,
        error
      ) != 0)
      {
        error << "forge: release tagging requires a Git repository with at least one commit\n";
        return false;
      }

      if (process_runner({ "git", "diff-index", "--quiet", "HEAD", "--" },
                         project_directory,
                         error) != 0)
      {
        error << "forge: release tagging requires a clean working tree\n";
        return false;
      }

      const auto existing = process_runner(
        { "git", "show-ref", "--verify", "--quiet", "refs/tags/" + std::string { tag } },
        project_directory,
        error
      );

      if (existing == 0)
      {
        if (!force_tag)
        {
          error
            << "forge: tag '" << tag << "' already exists locally\n"
            << "forge: bump the recipe version before publishing another release\n";
          return false;
        }
      }
      else if (existing != 1)
      {
        error << "forge: could not inspect existing Git tags\n";
        return false;
      }

      return true;
    }

    bool create_and_push_tag(const std::filesystem::path& project_directory,
                             std::string_view version,
                             const std::optional<std::string>& release_notes,
                             std::string_view tag,
                             bool force_tag,
                             const ProcessRunner& process_runner,
                             std::ostream& output,
                             std::ostream& error)
    {
      std::vector<std::string> tag_arguments { "git", "tag" };

      if (force_tag)
      {
        tag_arguments.push_back("--force");
      }

      tag_arguments.insert(tag_arguments.end(), { "-a", std::string { tag } });

      if (release_notes)
      {
        tag_arguments.insert(tag_arguments.end(), { "-m", *release_notes });
      }
      else
      {
        tag_arguments.insert(tag_arguments.end(), { "-m", "Release " + std::string { version } });
      }

      if (process_runner(tag_arguments, project_directory, error) != 0)
      {
        error << "forge: could not create tag '" << tag << "'\n";
        return false;
      }

      std::vector<std::string> push_arguments { "git", "push" };

      if (force_tag)
      {
        push_arguments.push_back("--force");
      }

      push_arguments.insert(
        push_arguments.end(),
        { "origin", "refs/tags/" + std::string { tag } }
      );

      if (process_runner(push_arguments, project_directory, error) != 0)
      {
        error << "forge: could not push tag '" << tag << "'; the local tag remains\n";
        return false;
      }

      output
        << (force_tag ? "Force-tagged and pushed " : "Tagged and pushed ")
        << tag
        << '\n';
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
    const auto extracted_notes_path = release_directory / "RELEASE_NOTES.md";
    std::error_code filesystem_error;
    std::filesystem::remove_all(staging_directory, filesystem_error);
    filesystem_error.clear();
    std::filesystem::remove(archive_path, filesystem_error);
    filesystem_error.clear();
    std::filesystem::remove(extracted_notes_path, filesystem_error);
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

  int release_git(const std::filesystem::path& project_directory,
                  const GitReleaseOptions& options,
                  std::ostream& output,
                  std::ostream& error)
  {
    return release_git(project_directory, options, run_process, output, error);
  }

  int release_git(const std::filesystem::path& project_directory,
                  const GitReleaseOptions& options,
                  const ProcessRunner& process_runner,
                  std::ostream& output,
                  std::ostream& error)
  {
    Recipe recipe;

    if (!read_recipe(project_directory / "forge.recipe.toml", recipe, error))
    {
      return 2;
    }

    std::string tag;

    if (!expand_tag_format(options.tag_format.value_or("release-<version>"), recipe, tag, error)
        || !preflight_tag(project_directory, tag, options.force_tag, process_runner, error))
    {
      return 2;
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

    return create_and_push_tag(
      project_directory,
      recipe.version,
      release_notes,
      tag,
      options.force_tag,
      process_runner,
      output,
      error
    ) ? 0 : 2;
  }

} // namespace forge
