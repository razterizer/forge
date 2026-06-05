#include "box.h"

#include "build.h"
#include "recipe.h"

#include <fstream>
#include <iterator>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace forge
{
  namespace
  {

    std::string target_os()
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

    bool write_manifest(const std::filesystem::path& path,
                        const Recipe& recipe,
                        std::string_view executable_name,
                        std::ostream& error)
    {
      std::ofstream manifest { path };

      if (!manifest)
      {
        error << "forge: could not create '" << path.string() << "'\n";
        return false;
      }

      manifest
        << "[cbox]\n"
        << "format = 1\n\n"
        << "[package]\n"
        << "name = \"" << recipe.name << "\"\n"
        << "version = \"" << recipe.version << "\"\n";

      if (recipe.build_number)
      {
        manifest << "build = " << *recipe.build_number << "\n";
      }

      manifest
        << "type = \"executable\"\n\n"
        << "[target]\n"
        << "os = \"" << target_os() << "\"\n"
        << "arch = \"" << target_arch() << "\"\n\n"
        << "[artifact]\n"
        << "path = \"bin/" << executable_name << "\"\n"
        << "kind = \"executable\"\n";

      return static_cast<bool>(manifest);
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

    std::filesystem::path resolve_box_path(const std::filesystem::path& path,
                                           const std::filesystem::path& working_directory)
    {
      return path.is_absolute() ? path : working_directory / path;
    }

    bool prepare_empty_directory(const std::filesystem::path& path,
                                 std::ostream& error)
    {
      std::error_code filesystem_error;
      std::filesystem::remove_all(path, filesystem_error);
      filesystem_error.clear();
      std::filesystem::create_directories(path, filesystem_error);

      if (filesystem_error)
      {
        error << "forge: could not create '" << path.string() << "'\n";
        return false;
      }

      return true;
    }

  } // namespace

  int create_box(const std::filesystem::path& project_directory,
                 std::ostream& output,
                 std::ostream& error)
  {
    return create_box(project_directory, run_process, output, error);
  }

  int create_box(const std::filesystem::path& project_directory,
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

    const auto box_name =
      recipe.name + "-" + package_version(recipe) + "-" + target_os() + "-" + target_arch();
    const auto boxes_directory = project_directory / ".forge" / "boxes";
    const auto staging_directory = boxes_directory / "staging" / box_name;
    const auto archive_path = boxes_directory / (box_name + ".cbox");

    if (!prepare_empty_directory(staging_directory / "bin", error))
    {
      return 2;
    }

    if (!copy_file(executable, staging_directory / "bin" / executable.filename(), error)
        || !write_manifest(staging_directory / "cbox.toml", recipe, executable.filename().string(), error))
    {
      return 2;
    }

    std::error_code filesystem_error;
    std::filesystem::remove(archive_path, filesystem_error);
    output << "Creating box " << box_name << '\n' << std::flush;

    const std::vector<std::string> archive_arguments {
      "cmake",
      "-E",
      "tar",
      "cf",
      archive_path.string(),
      "--format=zip",
      "cbox.toml",
      "bin"
    };

    if (process_runner(archive_arguments, staging_directory, error) != 0)
    {
      error << "forge: box archive creation failed\n";
      return 2;
    }

    output << "Created " << archive_path.string() << '\n';
    return 0;
  }

  int inspect_box(const std::filesystem::path& box_path,
                  const std::filesystem::path& working_directory,
                  std::ostream& output,
                  std::ostream& error)
  {
    return inspect_box(box_path, working_directory, run_process, output, error);
  }

  int inspect_box(const std::filesystem::path& box_path,
                  const std::filesystem::path& working_directory,
                  const ProcessRunner& process_runner,
                  std::ostream& output,
                  std::ostream& error)
  {
    const auto resolved_box = resolve_box_path(box_path, working_directory);

    if (!std::filesystem::is_regular_file(resolved_box))
    {
      error << "forge: box '" << resolved_box.string() << "' does not exist\n";
      return 2;
    }

    const auto inspect_directory = working_directory / ".forge" / "cache" / "box-inspect";

    if (!prepare_empty_directory(inspect_directory, error))
    {
      return 2;
    }

    const std::vector<std::string> extract_arguments {
      "cmake",
      "-E",
      "tar",
      "xf",
      resolved_box.string(),
      "cbox.toml"
    };

    if (process_runner(extract_arguments, inspect_directory, error) != 0)
    {
      error << "forge: could not read box manifest\n";
      return 2;
    }

    std::ifstream manifest { inspect_directory / "cbox.toml" };

    if (!manifest)
    {
      error << "forge: box does not contain cbox.toml\n";
      return 2;
    }

    output << std::string {
      std::istreambuf_iterator<char> { manifest },
      std::istreambuf_iterator<char> {}
    };
    return 0;
  }

  int extract_box(const std::filesystem::path& box_path,
                  const std::filesystem::path& working_directory,
                  std::ostream& output,
                  std::ostream& error)
  {
    return extract_box(box_path, working_directory, run_process, output, error);
  }

  int extract_box(const std::filesystem::path& box_path,
                  const std::filesystem::path& working_directory,
                  const ProcessRunner& process_runner,
                  std::ostream& output,
                  std::ostream& error)
  {
    const auto resolved_box = resolve_box_path(box_path, working_directory);

    if (!std::filesystem::is_regular_file(resolved_box))
    {
      error << "forge: box '" << resolved_box.string() << "' does not exist\n";
      return 2;
    }

    const auto destination = working_directory / resolved_box.stem();

    if (std::filesystem::exists(destination))
    {
      error << "forge: '" << destination.string() << "' already exists\n";
      return 2;
    }

    if (!prepare_empty_directory(destination, error))
    {
      return 2;
    }

    const std::vector<std::string> extract_arguments {
      "cmake",
      "-E",
      "tar",
      "xf",
      resolved_box.string()
    };

    if (process_runner(extract_arguments, destination, error) != 0)
    {
      error << "forge: box extraction failed\n";
      return 2;
    }

    output << "Extracted " << destination.string() << '\n';
    return 0;
  }

} // namespace forge
