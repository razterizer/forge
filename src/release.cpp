#include "release.h"

#include "box.h"
#include "build.h"
#include "recipe.h"
#include "runtime_assets.h"

#include <algorithm>
#include <array>
#include <cctype>
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

    std::optional<std::string> selected_workflow_release_profile(const Recipe& recipe)
    {
      const auto name = std::string { workflow_release_profile };
      return recipe.dependency_profiles.contains(name) || recipe.build_profiles.contains(name)
        ? std::optional<std::string> { name }
        : std::nullopt;
    }

    std::vector<std::string> selected_workflow_release_profiles(const Recipe& recipe)
    {
      std::vector<std::string> profiles;

      for (const auto& variant : recipe.release_variants)
      {
        profiles.push_back(variant.profile);
      }

      if (profiles.empty())
      {
        if (const auto profile = selected_workflow_release_profile(recipe))
        {
          profiles.push_back(*profile);
        }
      }

      return profiles;
    }

    std::string target_os()
    {
#ifdef __APPLE__
      return "macos";
#elif defined(__linux__)
      return "linux";
#elif defined(_WIN32)
      return "windows";
#else
      return "unknown";
#endif
    }

    std::string target_arch()
    {
#if defined(__aarch64__) || defined(_M_ARM64)
      return "arm64";
#elif defined(__x86_64__) || defined(_M_X64)
      return "x86_64";
#elif defined(__i386__) || defined(_M_IX86)
      return "x86";
#else
      return "unknown";
#endif
    }

    std::string current_target()
    {
      return target_os() + "-" + target_arch();
    }

    std::string hosted_platform()
    {
#ifdef _WIN32
      return "windows";
#elif __APPLE__
      return "macos";
#elif __linux__
      return "linux";
#else
      return "unknown";
#endif
    }

    bool has_current_import_profile(const Recipe& recipe)
    {
      const auto target = current_target();
      return std::find_if(
        recipe.imports.begin(),
        recipe.imports.end(),
        [&target](const ImportProfile& profile)
        {
          return profile.target == target;
        }
      ) != recipe.imports.end();
    }

    bool dependency_matches_current_target(const Dependency& dependency)
    {
      const auto target = current_target();
      return dependency.targets.empty()
        || std::find(
          dependency.targets.begin(),
          dependency.targets.end(),
          target
        ) != dependency.targets.end();
    }

    bool validate_workflow_release_dependencies(Recipe recipe, std::ostream& error)
    {
      const auto profiles = selected_workflow_release_profiles(recipe);

      if (profiles.empty())
      {
        return true;
      }

      for (const auto& profile : profiles)
      {
        Recipe profiled = recipe;

        if (!select_dependency_profile(profiled, profile, true, error))
        {
          return false;
        }

        for (const auto& dependency : profiled.dependencies)
        {
          if (!dependency_matches_current_target(dependency))
          {
            continue;
          }

          const auto local_git =
            !dependency.git.empty()
            && !dependency.git.starts_with("https://")
            && !dependency.git.starts_with("http://")
            && !dependency.git.starts_with("ssh://")
            && !dependency.git.starts_with("git@");

          if (!dependency.path.empty() || !dependency.box.empty() || local_git)
          {
            error
              << "forge: workflow release dependency '" << dependency.name
              << "' in profile '" << profile
              << "' uses a local "
              << (!dependency.path.empty() ? "project path"
                  : !dependency.box.empty() ? "box path"
                  : "Git location")
              << "; add a reproducible entry under [profile." << profile << ".dependencies]\n";
            return false;
          }
        }
      }

      return true;
    }

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

    bool is_project_relative_path(const std::filesystem::path& path)
    {
      const auto generic = path.generic_string();

      return
        !path.empty()
        && !path.is_absolute()
        && generic != ".."
        && generic.rfind("../", 0) != 0;
    }

    std::filesystem::path selected_release_readme(const Recipe& recipe)
    {
      const auto platform = hosted_platform();

      if (platform == "linux")
      {
        return recipe.release_readme.linux;
      }

      if (platform == "macos")
      {
        return recipe.release_readme.macos;
      }

      if (platform == "windows")
      {
        return recipe.release_readme.windows;
      }

      return {};
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

    std::string release_notes_heading(const Recipe& recipe)
    {
      auto heading = recipe.version;

      if (recipe.release_notes_build_number_format && recipe.build_number)
      {
        heading += *recipe.release_notes_build_number_format == "dotted"
          ? "." + std::to_string(*recipe.build_number)
          : "+build." + std::to_string(*recipe.build_number);
      }

      return heading;
    }

    std::string package_version(const Recipe& recipe)
    {
      auto version = recipe.version;

      if (recipe.build_number)
      {
        version += "+build." + std::to_string(*recipe.build_number);
      }

      return version;
    }

    std::string release_slug(std::string_view value)
    {
      std::string slug;
      bool previous_separator = false;

      for (const auto character : value)
      {
        const auto byte = static_cast<unsigned char>(character);

        if (std::isalnum(byte))
        {
          slug += static_cast<char>(std::tolower(byte));
          previous_separator = false;
        }
        else if (!slug.empty() && !previous_separator)
        {
          slug += '-';
          previous_separator = true;
        }
      }

      while (!slug.empty() && slug.back() == '-')
      {
        slug.pop_back();
      }

      return slug.empty() ? "release" : slug;
    }

    std::string release_bundle_base(const Recipe& recipe)
    {
      if (recipe.release_bundle_name)
      {
        return *recipe.release_bundle_name;
      }

      for (const auto& target : recipe.targets)
      {
        if (!target.test
            && (target.type == "header_only"
                || target.type == "static_library"
                || target.type == "dynamic_library"
                || target.type == "imported_library"))
        {
          return release_slug(target.name);
        }
      }

      return release_slug(recipe.name);
    }

    bool has_platform_specific_requirements(const Recipe& recipe)
    {
      return std::any_of(
          recipe.dependencies.begin(),
          recipe.dependencies.end(),
          [](const Dependency& dependency)
          {
            return !dependency.targets.empty();
          }
        )
        || !recipe.macos_system_include_directories.empty()
        || !recipe.linux_system_include_directories.empty()
        || !recipe.windows_system_include_directories.empty()
        || !recipe.macos_system_library_directories.empty()
        || !recipe.linux_system_library_directories.empty()
        || !recipe.windows_system_library_directories.empty()
        || !recipe.macos_frameworks.empty()
        || !recipe.macos_libraries.empty()
        || !recipe.linux_libraries.empty()
        || !recipe.windows_libraries.empty();
    }

    std::string hosted_target();

    std::string hosted_box_filename(const Recipe& recipe)
    {
      auto filename = recipe.name + "-" + package_version(recipe);

      if (recipe.type == "header_only"
          && recipe.dependencies.empty()
          && recipe.selected_internal_dependencies.empty()
          && !has_platform_specific_requirements(recipe))
      {
        filename += "-ho";
      }
      else
      {
        filename += "-" + hosted_target();
      }

      return filename + ".cbox";
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

    std::string hosted_target()
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
      replace_placeholder(tag, "<version>", release_notes_heading(recipe));
      replace_placeholder(tag, "<curr-date>", current_date());
      replace_placeholder(tag, "<target>", hosted_target());
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
    return release_project(project_directory, std::nullopt, run_process, output, error);
  }

  int release_project(const std::filesystem::path& project_directory,
                      const std::optional<std::string>& target,
                      std::ostream& output,
                      std::ostream& error)
  {
    return release_project(project_directory, target, run_process, output, error);
  }

  int release_project(const std::filesystem::path& project_directory,
                      const ProcessRunner& process_runner,
                      std::ostream& output,
                      std::ostream& error)
  {
    return release_project(project_directory, std::nullopt, process_runner, output, error);
  }

  int release_project(const std::filesystem::path& project_directory,
                      const std::optional<std::string>& target,
                      const ProcessRunner& process_runner,
                      std::ostream& output,
                      std::ostream& error)
  {
    return release_project(project_directory, target, std::nullopt, process_runner, output, error);
  }

  namespace
  {

    int release_project_impl(const std::filesystem::path& project_directory,
                             const std::optional<std::string>& target,
                             const std::optional<std::string>& profile,
                             const std::optional<std::string>& executable_suffix,
                             bool merge_release,
                             const ProcessRunner& process_runner,
                             std::ostream& output,
                             std::ostream& error)
  {
    Recipe recipe;

    if (!read_recipe(project_directory / "forge.recipe.toml", recipe, error))
    {
      return 2;
    }

    if (!select_recipe_target(recipe, target, error))
    {
      return 2;
    }

    if (recipe.type != "executable")
    {
      error << "forge: release target '" << recipe.name << "' is not executable\n";
      return 2;
    }

    BuildOptions options;
    options.target = target;
    options.profile = profile;

    if (profile == workflow_release_profile)
    {
      options.configuration = "Release";
    }

    if (build_project(project_directory, options, process_runner, output, error) != 0)
    {
      return 2;
    }

    if (!is_safe_path_component(recipe.name) || !is_safe_path_component(recipe.version))
    {
      error << "forge: project name and version must be safe path components\n";
      return 2;
    }

    auto build_directory = project_directory / ".forge" / "build";

    if (recipe.selected_target)
    {
      build_directory /= *recipe.selected_target;
    }

    auto executable = build_directory / recipe.name;

#ifdef _WIN32
    executable += ".exe";
#endif

    if (!std::filesystem::is_regular_file(executable))
    {
      error << "forge: built executable '" << executable.string() << "' does not exist\n";
      return 2;
    }

    const auto package_name = recipe.name + "-" + package_version(recipe);
    const auto release_directory = project_directory / ".forge" / "release";
    const auto staging_directory = release_directory / package_name;
    const auto archive_path = release_directory / (package_name + ".zip");
    const auto extracted_notes_path = release_directory / "RELEASE_NOTES.md";
    std::error_code filesystem_error;
    const auto existing_merge_staging =
      merge_release && std::filesystem::is_directory(staging_directory);

    if (!merge_release)
    {
      std::filesystem::remove_all(staging_directory, filesystem_error);
      filesystem_error.clear();
    }
    std::filesystem::remove(archive_path, filesystem_error);
    filesystem_error.clear();
    if (!merge_release)
    {
      std::filesystem::remove(extracted_notes_path, filesystem_error);
      filesystem_error.clear();
    }
    std::filesystem::create_directories(staging_directory, filesystem_error);

    if (filesystem_error)
    {
      error << "forge: could not create '" << staging_directory.string() << "'\n";
      return 2;
    }

    auto staged_executable_name = executable.filename().string();

    if (executable_suffix)
    {
#ifdef _WIN32
      staged_executable_name = recipe.name + "_" + *executable_suffix + ".exe";
#else
      staged_executable_name = recipe.name + "_" + *executable_suffix;
#endif
    }

    if (!copy_file(executable, staging_directory / staged_executable_name, error))
    {
      return 2;
    }

    if (!copy_runtime_dependencies(
      build_directory / "runtime",
#ifdef _WIN32
      staging_directory,
#else
      staging_directory / "runtime",
#endif
      error
    ))
    {
      return 2;
    }

    std::vector<RuntimeAsset> runtime_assets;

    if (!existing_merge_staging
        && (!collect_runtime_assets(project_directory, recipe.runtime_files, runtime_assets, error)
        || (!runtime_assets.empty()
            && !stage_runtime_assets(
              runtime_assets,
              staging_directory,
              staging_directory / ".forge" / "runtime-assets.txt",
              error
            ))))
    {
      return 2;
    }

    std::filesystem::remove_all(staging_directory / ".forge", filesystem_error);
    filesystem_error.clear();

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
      if (!is_project_relative_path(release_file))
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

    const auto release_readme = selected_release_readme(recipe);

    if (!release_readme.empty())
    {
      if (!is_project_relative_path(release_readme))
      {
        error << "forge: release README paths must stay inside the project\n";
        return 2;
      }

      if (!copy_file(project_directory / release_readme, staging_directory / "README.txt", error))
      {
        return 2;
      }
    }

    std::optional<std::string> release_notes;

    if (!extract_release_notes(
      project_directory / "RELEASE_NOTES.md",
      release_notes_heading(recipe),
      release_notes,
      error
    ))
    {
      return 2;
    }

    if (!merge_release
        && release_notes
        && (!write_release_notes(staging_directory / "RELEASE_NOTES.md", *release_notes, error)
            || !write_release_notes(release_directory / "RELEASE_NOTES.md", *release_notes, error)))
    {
      return 2;
    }

    if (merge_release)
    {
      output << "Staged " << package_name << " (" << *executable_suffix << ")\n";
      return 0;
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

  } // namespace

  int release_project(const std::filesystem::path& project_directory,
                      const std::optional<std::string>& target,
                      const std::optional<std::string>& profile,
                      const ProcessRunner& process_runner,
                      std::ostream& output,
                      std::ostream& error)
  {
    return release_project_impl(
      project_directory,
      target,
      profile,
      std::nullopt,
      false,
      process_runner,
      output,
      error
    );
  }

    int create_hosted_executable_bundle(const std::filesystem::path& project_directory,
                                        const Recipe& recipe,
                                        const ProcessRunner& process_runner,
                                        std::ostream& output,
                                        std::ostream& error)
    {
      std::vector<std::string> executable_targets;

      for (const auto& target : recipe.targets)
      {
        if (!target.test && target.type == "executable")
        {
          executable_targets.push_back(target.name);
        }
      }

      if (executable_targets.size() < 2)
      {
        return 0;
      }

      const auto release_directory = project_directory / ".forge" / "release";
      const auto bundle_name = release_bundle_base(recipe)
        + "-release-" + release_notes_heading(recipe)
        + "-" + hosted_platform();
      const auto bundle_directory = release_directory / bundle_name;
      const auto bundle_archive = release_directory / (bundle_name + ".zip");
      std::error_code filesystem_error;
      std::filesystem::remove_all(bundle_directory, filesystem_error);
      filesystem_error.clear();
      std::filesystem::remove(bundle_archive, filesystem_error);
      filesystem_error.clear();
      std::filesystem::create_directories(bundle_directory, filesystem_error);

      if (filesystem_error)
      {
        error << "forge: could not create '" << bundle_directory.string() << "'\n";
        return 2;
      }

      for (const auto& target : executable_targets)
      {
        Recipe target_recipe;

        if (!read_recipe(project_directory / "forge.recipe.toml", target_recipe, error)
            || !select_recipe_target(target_recipe, target, error))
        {
          return 2;
        }

        const auto package_name = target_recipe.name + "-" + package_version(target_recipe);
        const auto hosted_archive = release_directory / (package_name + "-" + hosted_target() + ".zip");
        const auto staged_package = release_directory / package_name;

        if (!std::filesystem::is_directory(staged_package)
            || !copy_release_entry(staged_package, bundle_directory / package_name, error))
        {
          error << "forge: could not add executable release '" << package_name
                << "' to hosted bundle\n";
          return 2;
        }

        std::filesystem::remove(hosted_archive, filesystem_error);
        filesystem_error.clear();
      }

      const auto notes = release_directory / "RELEASE_NOTES.md";

      if (std::filesystem::is_regular_file(notes)
          && !copy_file(notes, bundle_directory / "RELEASE_NOTES.md", error))
      {
        return 2;
      }

      output << "Packaging " << bundle_name << '\n' << std::flush;

      const std::vector<std::string> archive_arguments {
        "cmake",
        "-E",
        "tar",
        "cf",
        bundle_archive.string(),
        "--format=zip",
        bundle_name
      };

      if (process_runner(archive_arguments, release_directory, error) != 0)
      {
        error << "forge: hosted executable bundle creation failed\n";
        return 2;
      }

      output << "Released " << bundle_archive.string() << '\n';
      return 0;
    }

  int prepare_release(const std::filesystem::path& project_directory,
                      std::ostream& output,
                      std::ostream& error)
  {
    return prepare_release(project_directory, PrepareReleaseOptions {}, run_process, output, error);
  }

  int prepare_release(const std::filesystem::path& project_directory,
                      const std::optional<std::string>& target,
                      std::ostream& output,
                      std::ostream& error)
  {
    PrepareReleaseOptions options;
    options.target = target;
    return prepare_release(project_directory, options, run_process, output, error);
  }

  int prepare_release(const std::filesystem::path& project_directory,
                      const ProcessRunner& process_runner,
                      std::ostream& output,
                      std::ostream& error)
  {
    return prepare_release(project_directory, PrepareReleaseOptions {}, process_runner, output, error);
  }

  int prepare_release(const std::filesystem::path& project_directory,
                      const std::optional<std::string>& target,
                      const ProcessRunner& process_runner,
                      std::ostream& output,
                      std::ostream& error)
  {
    PrepareReleaseOptions options;
    options.target = target;
    return prepare_release(project_directory, options, process_runner, output, error);
  }

  int prepare_release(const std::filesystem::path& project_directory,
                      const PrepareReleaseOptions& options,
                      const ProcessRunner& process_runner,
                      std::ostream& output,
                      std::ostream& error)
  {
    Recipe recipe;

    if (!read_recipe(project_directory / "forge.recipe.toml", recipe, error))
    {
      return 2;
    }

    if (!validate_workflow_release_dependencies(recipe, error))
    {
      return 2;
    }

    const auto workflow_profile = options.profile ? options.profile : selected_workflow_release_profile(recipe);

    if (!options.target && recipe.targets.size() > 1)
    {
      for (const auto& release_target : recipe.targets)
      {
        if (release_target.test)
        {
          continue;
        }

        if (release_target.type == "executable" && !recipe.release_variants.empty())
        {
          bool first_variant = true;

          for (const auto& variant : recipe.release_variants)
          {
            auto target_options = options;
            target_options.target = release_target.name;
            target_options.profile = variant.profile;
            target_options.executable_suffix = variant.suffix;
            target_options.merge_executable_release = true;

            if (first_variant)
            {
              std::error_code filesystem_error;
              std::filesystem::remove_all(
                project_directory / ".forge" / "release"
                  / (release_target.name + "-" + package_version(recipe)),
                filesystem_error
              );
            }

            if (prepare_release(project_directory, target_options, process_runner, output, error) != 0)
            {
              return 2;
            }

            first_variant = false;
          }
        }
        else
        {
          auto target_options = options;
          target_options.target = release_target.name;

          if (prepare_release(project_directory, target_options, process_runner, output, error) != 0)
          {
            return 2;
          }
        }
      }

      if (create_hosted_executable_bundle(project_directory, recipe, process_runner, output, error) != 0)
      {
        return 2;
      }

      return 0;
    }

    if (!select_recipe_target(recipe, options.target, error))
    {
      return 2;
    }

    if (!select_dependency_profile(recipe, workflow_profile, true, error))
    {
      return 2;
    }

    if (options.skip_unsupported
        && recipe.type == "imported_library"
        && !has_current_import_profile(recipe))
    {
      output << "Skipped release preparation: imported_library has no import profile for "
             << current_target() << '\n';
      return 0;
    }

    const auto release_directory = project_directory / ".forge" / "release";
    std::error_code filesystem_error;
    std::filesystem::create_directories(release_directory, filesystem_error);

    if (filesystem_error)
    {
      error << "forge: could not create '" << release_directory.string() << "'\n";
      return 2;
    }

    if (recipe.type == "executable")
    {
      if (release_project_impl(
        project_directory,
        options.target,
        workflow_profile,
        options.executable_suffix,
        options.merge_executable_release,
        process_runner,
        output,
        error
      ) != 0)
      {
        return 2;
      }

      if (options.merge_executable_release)
      {
        return 0;
      }

      const auto archive = release_directory / (recipe.name + "-" + package_version(recipe) + ".zip");
      const auto hosted_archive =
        release_directory / (recipe.name + "-" + package_version(recipe)
                             + "-" + hosted_target() + ".zip");
      std::filesystem::remove(hosted_archive, filesystem_error);
      filesystem_error.clear();
      std::filesystem::rename(archive, hosted_archive, filesystem_error);

      if (filesystem_error)
      {
        error << "forge: could not prepare hosted release archive\n";
        return 2;
      }
    }
    else if (recipe.type == "static_library"
             || recipe.type == "dynamic_library"
             || recipe.type == "imported_library"
             || recipe.type == "header_only")
    {
      if (create_box(
        project_directory,
        options.target,
        workflow_profile,
        process_runner,
        output,
        error
      ) != 0)
      {
        return 2;
      }

      const auto boxes_directory = project_directory / ".forge" / "boxes";
      const auto box = boxes_directory / hosted_box_filename(recipe);

      if (!std::filesystem::is_regular_file(box))
      {
        error << "forge: could not locate the created box\n";
        return 2;
      }

      if (publish_box(box, project_directory, process_runner, output, error) != 0)
      {
        return 2;
      }
    }
    else
    {
      error << "forge: unsupported project type '" << recipe.type << "'\n";
      return 2;
    }

    const auto notes_path = release_directory / "RELEASE_NOTES.md";
    std::optional<std::string> release_notes;

    if (!extract_release_notes(
      project_directory / "RELEASE_NOTES.md",
      release_notes_heading(recipe),
      release_notes,
      error
    ))
    {
      return 2;
    }

    if (!write_release_notes(
      notes_path,
      release_notes.value_or("Release " + recipe.version + "\n"),
      error
    ))
    {
      return 2;
    }

    output << "Prepared release assets\n";
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
      release_notes_heading(recipe),
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
