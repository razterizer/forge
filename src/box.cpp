#include "box.h"

#include "build.h"
#include "recipe.h"
#include "sha256.h"
#include "zip.h"

#include <algorithm>
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

    struct BoxArtifact
    {
      std::filesystem::path path;
      std::string kind;
      std::string sha256;
    };

    struct BoxManifest
    {
      int format = 0;
      std::string name;
      std::string version;
      std::optional<int> build_number;
      std::string type;
      std::string os;
      std::string arch;
      std::vector<BoxArtifact> artifacts;
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

#ifndef _WIN32
    std::filesystem::path shared_library_filename(std::string_view name)
    {
#ifdef __APPLE__
      return "lib" + std::string { name } + ".dylib";
#elif defined(__linux__)
      return "lib" + std::string { name } + ".so";
#else
      return {};
#endif
    }
#endif

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

    bool stage_artifact(const std::filesystem::path& source,
                        const std::filesystem::path& artifact_path,
                        std::string_view kind,
                        const std::filesystem::path& staging_directory,
                        std::vector<BoxArtifact>& artifacts,
                        std::ostream& error)
    {
      std::error_code filesystem_error;
      std::filesystem::create_directories(
        (staging_directory / artifact_path).parent_path(),
        filesystem_error
      );

      if (filesystem_error)
      {
        error << "forge: could not create artifact directory\n";
        return false;
      }

      const auto destination = staging_directory / artifact_path;
      std::string checksum;

      if (!copy_file(source, destination, error) || !sha256_file(destination, checksum, error))
      {
        return false;
      }

      artifacts.push_back(BoxArtifact
        {
          artifact_path,
          std::string { kind },
          checksum
        });
      return true;
    }

    bool write_manifest(const std::filesystem::path& path,
                        const Recipe& recipe,
                        const std::vector<BoxArtifact>& artifacts,
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
        << "type = \"" << recipe.type << "\"\n\n"
        << "[target]\n"
        << "os = \"" << target_os() << "\"\n"
        << "arch = \"" << target_arch() << "\"\n";

      for (const auto& artifact : artifacts)
      {
        manifest
          << "\n[[artifact]]\n"
          << "path = \"" << artifact.path.generic_string() << "\"\n"
          << "kind = \"" << artifact.kind << "\"\n"
          << "sha256 = \"" << artifact.sha256 << "\"\n";
      }

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
      std::optional<std::size_t> artifact_index;
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

        if (trimmed.starts_with("[[") && trimmed.ends_with("]]"))
        {
          section = std::string { trim(trimmed.substr(2, trimmed.size() - 4)) };

          if (section != "artifact")
          {
            error << "forge: unsupported box manifest section on line " << line_number << '\n';
            return false;
          }

          manifest.artifacts.emplace_back();
          artifact_index = manifest.artifacts.size() - 1;
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

          if (section == "artifact")
          {
            if (!manifest.artifacts.empty())
            {
              error << "forge: duplicate box manifest artifact section\n";
              return false;
            }

            manifest.artifacts.emplace_back();
            artifact_index = 0;
          }
          else
          {
            artifact_index.reset();
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
        const auto identity = section == "artifact" && artifact_index
          ? section + "." + std::to_string(*artifact_index) + "." + std::string { key }
          : section + "." + std::string { key };

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
        else if (section == "artifact" && artifact_index && key == "path")
        {
          std::string artifact_path;
          valid = parse_string(value, artifact_path)
            && artifact_path.find('\\') == std::string::npos;
          manifest.artifacts[*artifact_index].path = artifact_path;
        }
        else if (section == "artifact" && artifact_index && key == "kind")
        {
          valid = parse_string(value, manifest.artifacts[*artifact_index].kind);
        }
        else if (section == "artifact" && artifact_index && key == "sha256")
        {
          valid = parse_string(value, manifest.artifacts[*artifact_index].sha256);
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
        "target.arch"
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
          || (manifest.type != "executable"
              && manifest.type != "static_library"
              && manifest.type != "shared_library"
              && manifest.type != "header_only")
          || manifest.artifacts.empty())
      {
        error << "forge: box manifest contains invalid package or artifact values\n";
        return false;
      }

      std::set<std::filesystem::path> artifact_paths;
      std::size_t executable_count = 0;
      std::size_t library_count = 0;
      std::size_t shared_library_count = 0;
      std::size_t header_count = 0;

      for (std::size_t index = 0; index < manifest.artifacts.size(); ++index)
      {
        const auto& artifact = manifest.artifacts[index];
        const auto prefix = artifact.path.empty() ? std::string {} : artifact.path.begin()->string();

        if (!seen.contains("artifact." + std::to_string(index) + ".path")
            || !seen.contains("artifact." + std::to_string(index) + ".kind")
            || !seen.contains("artifact." + std::to_string(index) + ".sha256")
            || !is_safe_archive_path(artifact.path)
            || artifact.path.parent_path().empty()
            || !artifact_paths.insert(artifact.path).second
            || artifact.sha256.size() != 64
            || artifact.sha256.find_first_not_of("0123456789abcdef") != std::string::npos)
        {
          error << "forge: box manifest contains invalid package or artifact values\n";
          return false;
        }

        if (artifact.kind == "executable" && prefix == "bin")
        {
          ++executable_count;
        }
        else if (artifact.kind == "static_library" && prefix == "lib")
        {
          ++library_count;
        }
        else if (artifact.kind == "shared_library" && prefix == "runtime")
        {
          ++shared_library_count;
        }
        else if (artifact.kind == "public_header" && prefix == "include")
        {
          ++header_count;
        }
        else
        {
          error << "forge: box manifest contains invalid package or artifact values\n";
          return false;
        }
      }

      if ((manifest.type == "executable"
           && (executable_count != 1 || manifest.artifacts.size() != 1))
          || (manifest.type == "static_library"
              && (library_count != 1
                  || header_count == 0
                  || library_count + header_count != manifest.artifacts.size()))
          || (manifest.type == "shared_library"
              && (shared_library_count != 1
                  || header_count == 0
                  || shared_library_count + header_count != manifest.artifacts.size()))
          || (manifest.type == "header_only"
              && (header_count == 0 || header_count != manifest.artifacts.size())))
      {
        error << "forge: box manifest artifacts do not match package type\n";
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

      std::set<std::filesystem::path> expected_files { std::filesystem::path { "cbox.toml" } };
      std::set<std::filesystem::path> expected_directories;

      for (const auto& artifact : manifest.artifacts)
      {
        if (!std::filesystem::is_regular_file(directory / artifact.path))
        {
          error << "forge: box artifact '" << artifact.path.string() << "' is missing\n";
          return false;
        }

        expected_files.insert(artifact.path);
        auto parent = artifact.path.parent_path();

        while (!parent.empty())
        {
          expected_directories.insert(parent);
          parent = parent.parent_path();
        }
      }

      std::set<std::string> expected_archive_entries { "cbox.toml" };

      for (const auto& artifact : manifest.artifacts)
      {
        expected_archive_entries.insert(artifact.path.generic_string());
      }

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

      for (const auto& artifact : manifest.artifacts)
      {
        std::string checksum;

        if (!sha256_file(directory / artifact.path, checksum, error))
        {
          return false;
        }

        if (checksum != artifact.sha256)
        {
          error << "forge: box artifact checksum does not match cbox.toml\n";
          return false;
        }
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

    auto built_artifact = project_directory / ".forge" / "build" / recipe.name;

#ifdef _WIN32
    built_artifact += recipe.type == "static_library" ? ".lib" : ".exe";
#else
    if (recipe.type == "static_library")
    {
      built_artifact = project_directory / ".forge" / "build" / ("lib" + recipe.name + ".a");
    }
    else if (recipe.type == "shared_library")
    {
      built_artifact = project_directory / ".forge" / "build" / shared_library_filename(recipe.name);
    }
#endif

    if (recipe.type != "header_only" && !std::filesystem::is_regular_file(built_artifact))
    {
      error << "forge: built artifact '" << built_artifact.string() << "' does not exist\n";
      return 2;
    }

    const auto box_name =
      recipe.name + "-" + package_version(recipe) + "-" + target_os() + "-" + target_arch();
    const auto boxes_directory = project_directory / ".forge" / "boxes";
    const auto staging_directory = boxes_directory / "staging" / box_name;
    const auto archive_path = boxes_directory / (box_name + ".cbox");

    if (!prepare_empty_directory(staging_directory, error))
    {
      return 2;
    }

    std::vector<BoxArtifact> artifacts;

    if (recipe.type == "executable")
    {
      if (!stage_artifact(
        built_artifact,
        std::filesystem::path { "bin" } / built_artifact.filename(),
        "executable",
        staging_directory,
        artifacts,
        error
      ))
      {
        return 2;
      }
    }
    else if (recipe.type == "static_library")
    {
      if (!stage_artifact(
        built_artifact,
        std::filesystem::path { "lib" } / built_artifact.filename(),
        "static_library",
        staging_directory,
        artifacts,
        error
      ))
      {
        return 2;
      }

      for (const auto& header : recipe.public_headers)
      {
        if (!stage_artifact(
          project_directory / header,
          header,
          "public_header",
          staging_directory,
          artifacts,
          error
        ))
        {
          return 2;
        }
      }
    }
    else if (recipe.type == "shared_library")
    {
      if (!stage_artifact(
        built_artifact,
        std::filesystem::path { "runtime" } / built_artifact.filename(),
        "shared_library",
        staging_directory,
        artifacts,
        error
      ))
      {
        return 2;
      }

      for (const auto& header : recipe.public_headers)
      {
        if (!stage_artifact(
          project_directory / header,
          header,
          "public_header",
          staging_directory,
          artifacts,
          error
        ))
        {
          return 2;
        }
      }
    }
    else if (recipe.type == "header_only")
    {
      for (const auto& header : recipe.public_headers)
      {
        if (!stage_artifact(
          project_directory / header,
          header,
          "public_header",
          staging_directory,
          artifacts,
          error
        ))
        {
          return 2;
        }
      }
    }
    else
    {
      error << "forge: unsupported project type '" << recipe.type << "'\n";
      return 2;
    }

    if (!write_manifest(staging_directory / "cbox.toml", recipe, artifacts, error))
    {
      return 2;
    }

    std::error_code filesystem_error;
    std::filesystem::remove(archive_path, filesystem_error);
    output << "Creating box " << box_name << '\n' << std::flush;

    std::vector<std::string> archive_arguments {
      "cmake",
      "-E",
      "tar",
      "cf",
      archive_path.string(),
      "--format=zip",
      "cbox.toml"
    };

    if (recipe.type == "executable")
    {
      archive_arguments.push_back("bin");
    }
    else if (recipe.type == "static_library")
    {
      archive_arguments.push_back("include");
      archive_arguments.push_back("lib");
    }
    else if (recipe.type == "shared_library")
    {
      archive_arguments.push_back("include");
      archive_arguments.push_back("runtime");
    }
    else
    {
      archive_arguments.push_back("include");
    }

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

  int publish_box(const std::filesystem::path& box_path,
                  const std::filesystem::path& project_directory,
                  std::ostream& output,
                  std::ostream& error)
  {
    return publish_box(box_path, project_directory, run_process, output, error);
  }

  int publish_box(const std::filesystem::path& box_path,
                  const std::filesystem::path& project_directory,
                  const ProcessRunner& process_runner,
                  std::ostream& output,
                  std::ostream& error)
  {
    if (!std::filesystem::is_regular_file(project_directory / "forge.recipe.toml"))
    {
      error << "forge: forge.recipe.toml was not found in the current directory\n";
      return 2;
    }

    const auto resolved_box = resolve_box_path(box_path, project_directory);
    BoxMetadata metadata;

    if (!read_box_metadata(resolved_box, project_directory, process_runner, metadata, error))
    {
      return 2;
    }

    const auto boxes_directory = project_directory / "boxes";
    const auto published_box = boxes_directory / resolved_box.filename();
    const auto checksum_path = boxes_directory / (resolved_box.filename().string() + ".sha256");
    std::string source_checksum;

    if (!sha256_file(resolved_box, source_checksum, error))
    {
      return 2;
    }

    std::error_code filesystem_error;
    std::filesystem::create_directories(boxes_directory, filesystem_error);

    if (filesystem_error)
    {
      error << "forge: could not create '" << boxes_directory.string() << "'\n";
      return 2;
    }

    if (std::filesystem::is_regular_file(published_box))
    {
      std::string published_checksum;

      if (!sha256_file(published_box, published_checksum, error))
      {
        return 2;
      }

      if (published_checksum != source_checksum)
      {
        error << "forge: published box '" << published_box.string()
              << "' already exists with different contents\n";
        return 2;
      }
    }
    else
    {
      std::filesystem::copy_file(resolved_box, published_box, filesystem_error);

      if (filesystem_error)
      {
        error << "forge: could not publish '" << published_box.string() << "'\n";
        return 2;
      }
    }

    std::ofstream checksum_file { checksum_path };

    if (!checksum_file)
    {
      error << "forge: could not write '" << checksum_path.string() << "'\n";
      return 2;
    }

    checksum_file << source_checksum << "  " << published_box.filename().string() << '\n';

    if (!checksum_file)
    {
      error << "forge: could not write '" << checksum_path.string() << "'\n";
      return 2;
    }

    output
      << "Published locally " << published_box.string() << '\n'
      << "Checksum " << source_checksum << '\n';
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

    if (!prepare_empty_directory(destination, error)
        || !copy_file(validation_directory / "cbox.toml", destination / "cbox.toml", error))
    {
      return 2;
    }

    for (const auto& artifact : manifest.artifacts)
    {
      std::error_code filesystem_error;
      std::filesystem::create_directories(
        (destination / artifact.path).parent_path(),
        filesystem_error
      );

      if (filesystem_error
          || !copy_file(
            validation_directory / artifact.path,
            destination / artifact.path,
            error
          ))
      {
        return 2;
      }
    }

    output << "Extracted " << destination.string() << '\n';
    return 0;
  }

  bool read_box_metadata(const std::filesystem::path& box_path,
                         const std::filesystem::path& working_directory,
                         const ProcessRunner& process_runner,
                         BoxMetadata& metadata,
                         std::ostream& error)
  {
    const auto resolved_box = resolve_box_path(box_path, working_directory);

    if (!validate_box_path(resolved_box, error))
    {
      return false;
    }

    const auto validation_directory = working_directory / ".forge" / "cache" / "box-metadata";
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
      return false;
    }

    metadata =
      {
        manifest.name,
        manifest.version,
        manifest.build_number,
        manifest.type,
        manifest.os,
        manifest.arch,
        {}
      };

    for (const auto& artifact : manifest.artifacts)
    {
      metadata.artifacts.push_back({ artifact.path, artifact.kind });
    }

    return true;
  }

} // namespace forge
