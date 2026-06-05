#include "box.h"

#include "build.h"
#include "recipe.h"
#include "sha256.h"
#include "zip.h"

#include <array>
#include <charconv>
#include <fstream>
#include <iterator>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace forge
{
  namespace
  {

    struct BoxManifest
    {
      int format = 0;
      std::string name;
      std::string version;
      std::optional<int> build_number;
      std::string type;
      std::string os;
      std::string arch;
      std::filesystem::path artifact_path;
      std::string artifact_kind;
      std::string artifact_sha256;
    };

    std::string_view trim(std::string_view value)
    {
      const auto first = value.find_first_not_of(" \t\r\n");

      if (first == std::string_view::npos)
      {
        return {};
      }

      const auto last = value.find_last_not_of(" \t\r\n");
      return value.substr(first, last - first + 1);
    }

    bool parse_string(std::string_view value, std::string& result)
    {
      value = trim(value);

      if (value.size() < 2 || value.front() != '"' || value.back() != '"')
      {
        return false;
      }

      result = std::string { value.substr(1, value.size() - 2) };
      return result.find('"') == std::string::npos && result.find('\\') == std::string::npos;
    }

    bool parse_integer(std::string_view value, int& result)
    {
      value = trim(value);
      const auto parsed = std::from_chars(value.data(), value.data() + value.size(), result);
      return parsed.ec == std::errc {} && parsed.ptr == value.data() + value.size();
    }

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

    bool is_safe_archive_path(const std::filesystem::path& path)
    {
      if (path.empty() || path.is_absolute() || path.has_root_path())
      {
        return false;
      }

      for (const auto& component : path)
      {
        if (component == "." || component == ".." || component.empty())
        {
          return false;
        }
      }

      return true;
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
                        std::string_view checksum,
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
        << "kind = \"executable\"\n"
        << "sha256 = \"" << checksum << "\"\n";

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

    bool read_manifest(const std::filesystem::path& path,
                       BoxManifest& manifest,
                       std::string& content,
                       std::ostream& error)
    {
      std::ifstream file { path };

      if (!file)
      {
        error << "forge: box does not contain cbox.toml\n";
        return false;
      }

      content = std::string {
        std::istreambuf_iterator<char> { file },
        std::istreambuf_iterator<char> {}
      };
      std::istringstream input { content };
      std::set<std::string> seen;
      std::string section;
      std::string line;
      std::size_t line_number = 0;

      while (std::getline(input, line))
      {
        ++line_number;
        const auto trimmed = trim(line);

        if (trimmed.empty() || trimmed.front() == '#')
        {
          continue;
        }

        if (trimmed.front() == '[' && trimmed.back() == ']')
        {
          section = std::string { trim(trimmed.substr(1, trimmed.size() - 2)) };

          if (section != "cbox" && section != "package" && section != "target" && section != "artifact")
          {
            error << "forge: unsupported box manifest section on line " << line_number << '\n';
            return false;
          }

          continue;
        }

        const auto equals = trimmed.find('=');

        if (equals == std::string_view::npos)
        {
          error << "forge: invalid box manifest line " << line_number << '\n';
          return false;
        }

        const auto key = trim(trimmed.substr(0, equals));
        const auto value = trim(trimmed.substr(equals + 1));
        const auto identity = section + "." + std::string { key };

        if (!seen.insert(identity).second)
        {
          error << "forge: duplicate box manifest field '" << identity << "'\n";
          return false;
        }

        bool valid = true;

        if (identity == "cbox.format")
        {
          valid = parse_integer(value, manifest.format);
        }
        else if (identity == "package.name")
        {
          valid = parse_string(value, manifest.name);
        }
        else if (identity == "package.version")
        {
          valid = parse_string(value, manifest.version);
        }
        else if (identity == "package.build")
        {
          int build_number = 0;
          valid = parse_integer(value, build_number) && build_number >= 0;

          if (valid)
          {
            manifest.build_number = build_number;
          }
        }
        else if (identity == "package.type")
        {
          valid = parse_string(value, manifest.type);
        }
        else if (identity == "target.os")
        {
          valid = parse_string(value, manifest.os);
        }
        else if (identity == "target.arch")
        {
          valid = parse_string(value, manifest.arch);
        }
        else if (identity == "artifact.path")
        {
          std::string artifact_path;
          valid = parse_string(value, artifact_path)
            && artifact_path.find('\\') == std::string::npos;
          manifest.artifact_path = artifact_path;
        }
        else if (identity == "artifact.kind")
        {
          valid = parse_string(value, manifest.artifact_kind);
        }
        else if (identity == "artifact.sha256")
        {
          valid = parse_string(value, manifest.artifact_sha256);
        }
        else
        {
          valid = false;
        }

        if (!valid)
        {
          error << "forge: invalid or unsupported box manifest field on line " << line_number << '\n';
          return false;
        }
      }

      const std::array required_fields {
        "cbox.format",
        "package.name",
        "package.version",
        "package.type",
        "target.os",
        "target.arch",
        "artifact.path",
        "artifact.kind",
        "artifact.sha256"
      };

      for (const auto field : required_fields)
      {
        if (!seen.contains(field))
        {
          error << "forge: box manifest is missing '" << field << "'\n";
          return false;
        }
      }

      if (manifest.format != 1)
      {
        error << "forge: unsupported box format " << manifest.format << '\n';
        return false;
      }

      if (!is_safe_path_component(manifest.name)
          || !is_safe_path_component(manifest.version)
          || manifest.type != "executable"
          || manifest.artifact_kind != "executable"
          || !is_safe_archive_path(manifest.artifact_path)
          || manifest.artifact_path.parent_path().empty()
          || manifest.artifact_path.begin()->string() != "bin"
          || manifest.artifact_sha256.size() != 64
          || manifest.artifact_sha256.find_first_not_of("0123456789abcdef") != std::string::npos)
      {
        error << "forge: box manifest contains invalid package or artifact values\n";
        return false;
      }

      return true;
    }

    bool validate_extracted_box(const std::filesystem::path& directory,
                                const std::vector<std::string>& archive_entries,
                                BoxManifest& manifest,
                                std::string& manifest_content,
                                std::ostream& error)
    {
      if (!read_manifest(directory / "cbox.toml", manifest, manifest_content, error))
      {
        return false;
      }

      const auto artifact = directory / manifest.artifact_path;

      if (!std::filesystem::is_regular_file(artifact))
      {
        error << "forge: box artifact '" << manifest.artifact_path.string() << "' is missing\n";
        return false;
      }

      std::set<std::filesystem::path> expected_files {
        std::filesystem::path { "cbox.toml" },
        manifest.artifact_path
      };
      std::set<std::filesystem::path> expected_directories;
      auto parent = manifest.artifact_path.parent_path();

      while (!parent.empty())
      {
        expected_directories.insert(parent);
        parent = parent.parent_path();
      }

      std::set<std::string> expected_archive_entries {
        "cbox.toml",
        manifest.artifact_path.generic_string()
      };

      for (const auto& directory_path : expected_directories)
      {
        expected_archive_entries.insert(directory_path.generic_string() + "/");
      }

      for (const auto& entry : archive_entries)
      {
        if (!expected_archive_entries.contains(entry))
        {
          error << "forge: box contains unexpected archive entry '" << entry << "'\n";
          return false;
        }
      }

      for (const auto& entry : expected_archive_entries)
      {
        if (std::find(archive_entries.begin(), archive_entries.end(), entry) == archive_entries.end())
        {
          error << "forge: box is missing archive entry '" << entry << "'\n";
          return false;
        }
      }

      for (const auto& entry : std::filesystem::recursive_directory_iterator { directory })
      {
        const auto relative = entry.path().lexically_relative(directory);

        if (entry.is_symlink())
        {
          error << "forge: box contains unsupported symbolic link '" << relative.string() << "'\n";
          return false;
        }

        if (entry.is_regular_file() && !expected_files.contains(relative))
        {
          error << "forge: box contains unexpected file '" << relative.string() << "'\n";
          return false;
        }

        if (entry.is_directory() && !expected_directories.contains(relative))
        {
          error << "forge: box contains unexpected directory '" << relative.string() << "'\n";
          return false;
        }

        if (!entry.is_directory() && !entry.is_regular_file())
        {
          error << "forge: box contains unsupported entry '" << relative.string() << "'\n";
          return false;
        }
      }

      std::string checksum;

      if (!sha256_file(artifact, checksum, error))
      {
        return false;
      }

      if (checksum != manifest.artifact_sha256)
      {
        error << "forge: box artifact checksum does not match cbox.toml\n";
        return false;
      }

      return true;
    }

    bool unpack_and_validate_box(const std::filesystem::path& resolved_box,
                                 const std::filesystem::path& validation_directory,
                                 const ProcessRunner& process_runner,
                                 BoxManifest& manifest,
                                 std::string& manifest_content,
                                 std::ostream& error)
    {
      if (!prepare_empty_directory(validation_directory, error))
      {
        return false;
      }

      std::vector<std::string> archive_entries;

      if (!read_zip_entries(resolved_box, archive_entries, error))
      {
        return false;
      }

      const std::vector<std::string> extract_arguments {
        "cmake",
        "-E",
        "tar",
        "xf",
        resolved_box.string()
      };

      if (process_runner(extract_arguments, validation_directory, error) != 0)
      {
        error << "forge: box extraction failed\n";
        return false;
      }

      return validate_extracted_box(
        validation_directory,
        archive_entries,
        manifest,
        manifest_content,
        error
      );
    }

    bool validate_box_path(const std::filesystem::path& resolved_box,
                           std::ostream& error)
    {
      if (!std::filesystem::is_regular_file(resolved_box))
      {
        error << "forge: box '" << resolved_box.string() << "' does not exist\n";
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

    const auto staged_executable = staging_directory / "bin" / executable.filename();
    std::string checksum;

    if (!copy_file(executable, staged_executable, error)
        || !sha256_file(staged_executable, checksum, error)
        || !write_manifest(
          staging_directory / "cbox.toml",
          recipe,
          executable.filename().string(),
          checksum,
          error
        ))
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

    if (!validate_box_path(resolved_box, error))
    {
      return 2;
    }

    const auto inspect_directory = working_directory / ".forge" / "cache" / "box-inspect";
    BoxManifest manifest;
    std::string manifest_content;

    if (!unpack_and_validate_box(
      resolved_box,
      inspect_directory,
      process_runner,
      manifest,
      manifest_content,
      error
    ))
    {
      return 2;
    }

    output << manifest_content;
    return 0;
  }

  int verify_box(const std::filesystem::path& box_path,
                 const std::filesystem::path& working_directory,
                 std::ostream& output,
                 std::ostream& error)
  {
    return verify_box(box_path, working_directory, run_process, output, error);
  }

  int verify_box(const std::filesystem::path& box_path,
                 const std::filesystem::path& working_directory,
                 const ProcessRunner& process_runner,
                 std::ostream& output,
                 std::ostream& error)
  {
    const auto resolved_box = resolve_box_path(box_path, working_directory);

    if (!validate_box_path(resolved_box, error))
    {
      return 2;
    }

    const auto validation_directory = working_directory / ".forge" / "cache" / "box-verify";
    BoxManifest manifest;
    std::string manifest_content;

    if (!unpack_and_validate_box(
      resolved_box,
      validation_directory,
      process_runner,
      manifest,
      manifest_content,
      error
    ))
    {
      return 2;
    }

    output << "Verified " << resolved_box.string() << '\n';
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

    if (!validate_box_path(resolved_box, error))
    {
      return 2;
    }

    const auto destination = working_directory / resolved_box.stem();

    if (std::filesystem::exists(destination))
    {
      error << "forge: '" << destination.string() << "' already exists\n";
      return 2;
    }

    const auto validation_directory = working_directory / ".forge" / "cache" / "box-extract";
    BoxManifest manifest;
    std::string manifest_content;

    if (!unpack_and_validate_box(
      resolved_box,
      validation_directory,
      process_runner,
      manifest,
      manifest_content,
      error
    ))
    {
      return 2;
    }

    if (!prepare_empty_directory(destination / manifest.artifact_path.parent_path(), error)
        || !copy_file(validation_directory / "cbox.toml", destination / "cbox.toml", error)
        || !copy_file(
          validation_directory / manifest.artifact_path,
          destination / manifest.artifact_path,
          error
        ))
    {
      return 2;
    }

    output << "Extracted " << destination.string() << '\n';
    return 0;
  }

} // namespace forge
