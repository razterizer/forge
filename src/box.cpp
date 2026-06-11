#include "box.h"

#include "build.h"
#include "recipe.h"
#include "runtime_assets.h"
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

    struct BoxDependency
    {
      std::string name;
      std::string version;
      std::string type;
      std::filesystem::path path;
      std::string sha256;
    };

    struct BoxComponent
    {
      std::string name;
      std::string type;
      std::filesystem::path path;
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
      std::optional<ToolchainIdentity> toolchain;
      std::vector<BoxArtifact> artifacts;
      std::vector<BoxDependency> dependencies;
      std::vector<BoxComponent> components;
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

    bool read_toolchain(const std::filesystem::path& path,
                        ToolchainIdentity& toolchain,
                        std::ostream& error)
    {
      std::ifstream file { path };

      if (!file)
      {
        error << "forge: could not read toolchain identity '" << path.string() << "'\n";
        return false;
      }

      std::string line;

      while (std::getline(file, line))
      {
        const auto content = trim(line);
        const auto equals = content.find('=');

        if (content.empty() || content.front() == '#' || equals == std::string_view::npos)
        {
          continue;
        }

        const auto key = trim(content.substr(0, equals));
        const auto value = trim(content.substr(equals + 1));
        bool valid = true;

        if (key == "compiler")
        {
          valid = parse_string(value, toolchain.compiler);
        }
        else if (key == "compiler_version")
        {
          valid = parse_string(value, toolchain.compiler_version);
        }
        else if (key == "cpp_std")
        {
          valid = parse_integer(value, toolchain.cpp_standard);
        }
        else if (key == "configuration")
        {
          valid = parse_string(value, toolchain.configuration);
        }
        else if (key == "runtime")
        {
          valid = parse_string(value, toolchain.runtime);
        }
        else
        {
          valid = false;
        }

        if (!valid)
        {
          error << "forge: invalid toolchain identity\n";
          return false;
        }
      }

      if (toolchain.compiler.empty()
          || toolchain.compiler_version.empty()
          || toolchain.cpp_standard == 0
          || toolchain.configuration.empty()
          || toolchain.runtime.empty())
      {
        error << "forge: incomplete toolchain identity\n";
        return false;
      }

      return true;
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

    std::string current_target()
    {
      return target_os() + "-" + target_arch();
    }

    std::filesystem::path dynamic_library_filename(std::string_view name)
    {
#ifdef __APPLE__
      return "lib" + std::string { name } + ".dylib";
#elif defined(__linux__)
      return "lib" + std::string { name } + ".so";
#elif defined(_WIN32)
      return std::string { name } + ".dll";
#else
      return {};
#endif
    }

#ifdef _WIN32
    std::filesystem::path import_library_filename(std::string_view name)
    {
      return std::string { name } + ".lib";
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

    bool is_safe_project_path(const std::filesystem::path& path)
    {
      if (path.empty() || path.is_absolute() || path.has_root_path())
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

    bool stage_imported_file(const std::filesystem::path& project_directory,
                             const std::filesystem::path& source_path,
                             const std::filesystem::path& artifact_path,
                             std::string_view kind,
                             const std::filesystem::path& staging_directory,
                             std::vector<BoxArtifact>& artifacts,
                             std::ostream& error)
    {
      if (!is_safe_project_path(source_path)
          || std::filesystem::is_symlink(project_directory / source_path)
          || !std::filesystem::is_regular_file(project_directory / source_path))
      {
        error << "forge: imported artifact '" << source_path.generic_string()
              << "' must be a project-relative file\n";
        return false;
      }

      for (const auto& artifact : artifacts)
      {
        if (artifact.path == artifact_path)
        {
          error << "forge: imported artifacts collide at '" << artifact_path.generic_string()
                << "'\n";
          return false;
        }
      }

      return stage_artifact(
        project_directory / source_path,
        artifact_path,
        kind,
        staging_directory,
        artifacts,
        error
      );
    }

    bool stage_imported_headers(const std::filesystem::path& project_directory,
                                const std::filesystem::path& source_path,
                                const std::filesystem::path& staging_directory,
                                std::vector<BoxArtifact>& artifacts,
                                std::ostream& error)
    {
      if (!is_safe_project_path(source_path))
      {
        error << "forge: imported header path '" << source_path.generic_string()
              << "' must stay inside the project\n";
        return false;
      }

      const auto source = project_directory / source_path;

      if (std::filesystem::is_regular_file(source))
      {
        return stage_imported_file(
          project_directory,
          source_path,
          std::filesystem::path { "include" } / source_path.filename(),
          "public_header",
          staging_directory,
          artifacts,
          error
        );
      }

      if (!std::filesystem::is_directory(source))
      {
        error << "forge: imported header path '" << source_path.generic_string()
              << "' does not exist\n";
        return false;
      }

      for (const auto& entry : std::filesystem::recursive_directory_iterator { source })
      {
        if (entry.is_symlink())
        {
          error << "forge: imported header directories may contain only regular files\n";
          return false;
        }

        if (entry.is_directory())
        {
          continue;
        }

        if (!entry.is_regular_file())
        {
          error << "forge: imported header directories may contain only regular files\n";
          return false;
        }

        const auto relative = entry.path().lexically_relative(source);

        if (!stage_imported_file(
          project_directory,
          entry.path().lexically_relative(project_directory),
          std::filesystem::path { "include" } / relative,
          "public_header",
          staging_directory,
          artifacts,
          error
        ))
        {
          return false;
        }
      }

      return true;
    }

    bool stage_imported_libraries(const std::filesystem::path& project_directory,
                                  const std::vector<std::filesystem::path>& sources,
                                  const std::filesystem::path& destination,
                                  std::string_view kind,
                                  const std::filesystem::path& staging_directory,
                                  std::vector<BoxArtifact>& artifacts,
                                  std::ostream& error)
    {
      for (const auto& source : sources)
      {
        if (!stage_imported_file(
          project_directory,
          source,
          destination / source.filename(),
          kind,
          staging_directory,
          artifacts,
          error
        ))
        {
          return false;
        }
      }

      return true;
    }

    bool write_manifest(const std::filesystem::path& path,
                        const Recipe& recipe,
                        const std::optional<ToolchainIdentity>& toolchain,
                        const std::vector<BoxArtifact>& artifacts,
                        const std::vector<BoxDependency>& dependencies,
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
        << "format = 2\n\n"
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

      if (toolchain)
      {
        manifest
          << "\n[toolchain]\n"
          << "compiler = \"" << toolchain->compiler << "\"\n"
          << "compiler_version = \"" << toolchain->compiler_version << "\"\n"
          << "cpp_std = " << toolchain->cpp_standard << "\n"
          << "configuration = \"" << toolchain->configuration << "\"\n"
          << "runtime = \"" << toolchain->runtime << "\"\n";
      }

      for (const auto& artifact : artifacts)
      {
        manifest
          << "\n[[artifact]]\n"
          << "path = \"" << artifact.path.generic_string() << "\"\n"
          << "kind = \"" << artifact.kind << "\"\n"
          << "sha256 = \"" << artifact.sha256 << "\"\n";
      }

      for (const auto& dependency : dependencies)
      {
        manifest
          << "\n[[dependency]]\n"
          << "name = \"" << dependency.name << "\"\n"
          << "version = \"" << dependency.version << "\"\n"
          << "type = \"" << dependency.type << "\"\n"
          << "path = \"" << dependency.path.generic_string() << "\"\n"
          << "sha256 = \"" << dependency.sha256 << "\"\n";
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

    std::string box_filename(const Recipe& recipe)
    {
      auto filename = recipe.name + "-" + package_version(recipe);

      if (recipe.type == "header_only")
      {
        filename += "-ho";
      }
      else
      {
        filename += "-" + target_os() + "-" + target_arch();
      }

      return filename + ".cbox";
    }

    std::filesystem::path resolve_box_path(const std::filesystem::path& path,
                                           const std::filesystem::path& working_directory)
    {
      if (path.is_absolute())
      {
        return path;
      }

      const auto relative_path = working_directory / path;

      if (std::filesystem::is_regular_file(relative_path) || path.has_parent_path())
      {
        return relative_path;
      }

      const auto generated_box = working_directory / ".forge" / "boxes" / path;

      if (std::filesystem::is_regular_file(generated_box))
      {
        return generated_box;
      }

      const auto published_box = working_directory / "boxes" / path;
      return std::filesystem::is_regular_file(published_box) ? published_box : relative_path;
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
      std::optional<std::size_t> dependency_index;
      std::optional<std::size_t> component_index;
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

          if (section != "artifact" && section != "dependency" && section != "component")
          {
            error << "forge: unsupported box manifest section on line " << line_number << '\n';
            return false;
          }

          if (section == "artifact")
          {
            manifest.artifacts.emplace_back();
            artifact_index = manifest.artifacts.size() - 1;
            dependency_index.reset();
            component_index.reset();
          }
          else if (section == "dependency")
          {
            manifest.dependencies.emplace_back();
            dependency_index = manifest.dependencies.size() - 1;
            artifact_index.reset();
            component_index.reset();
          }
          else
          {
            manifest.components.emplace_back();
            component_index = manifest.components.size() - 1;
            artifact_index.reset();
            dependency_index.reset();
          }

          continue;
        }

        if (trimmed.front() == '[' && trimmed.back() == ']')
        {
          section = std::string { trim(trimmed.substr(1, trimmed.size() - 2)) };

          if (section != "cbox"
              && section != "package"
              && section != "target"
              && section != "toolchain"
              && section != "artifact"
              && section != "dependency"
              && section != "component")
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
            dependency_index.reset();
            component_index.reset();
          }
          else if (section == "dependency")
          {
            if (!manifest.dependencies.empty())
            {
              error << "forge: duplicate box manifest dependency section\n";
              return false;
            }

            manifest.dependencies.emplace_back();
            dependency_index = 0;
            artifact_index.reset();
            component_index.reset();
          }
          else if (section == "component")
          {
            if (!manifest.components.empty())
            {
              error << "forge: duplicate box manifest component section\n";
              return false;
            }

            manifest.components.emplace_back();
            component_index = 0;
            artifact_index.reset();
            dependency_index.reset();
          }
          else
          {
            artifact_index.reset();
            dependency_index.reset();
            component_index.reset();
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
        auto identity = section + "." + std::string { key };

        if (section == "artifact" && artifact_index)
        {
          identity = section + "." + std::to_string(*artifact_index) + "." + std::string { key };
        }
        else if (section == "dependency" && dependency_index)
        {
          identity = section + "." + std::to_string(*dependency_index) + "." + std::string { key };
        }
        else if (section == "component" && component_index)
        {
          identity = section + "." + std::to_string(*component_index) + "." + std::string { key };
        }

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
        else if (identity == "toolchain.compiler")
        {
          if (!manifest.toolchain)
          {
            manifest.toolchain.emplace();
          }
          valid = parse_string(value, manifest.toolchain->compiler);
        }
        else if (identity == "toolchain.compiler_version")
        {
          if (!manifest.toolchain)
          {
            manifest.toolchain.emplace();
          }
          valid = parse_string(value, manifest.toolchain->compiler_version);
        }
        else if (identity == "toolchain.cpp_std")
        {
          if (!manifest.toolchain)
          {
            manifest.toolchain.emplace();
          }
          valid = parse_integer(value, manifest.toolchain->cpp_standard);
        }
        else if (identity == "toolchain.configuration")
        {
          if (!manifest.toolchain)
          {
            manifest.toolchain.emplace();
          }
          valid = parse_string(value, manifest.toolchain->configuration);
        }
        else if (identity == "toolchain.runtime")
        {
          if (!manifest.toolchain)
          {
            manifest.toolchain.emplace();
          }
          valid = parse_string(value, manifest.toolchain->runtime);
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
        else if (section == "dependency" && dependency_index && key == "name")
        {
          valid = parse_string(value, manifest.dependencies[*dependency_index].name);
        }
        else if (section == "dependency" && dependency_index && key == "version")
        {
          valid = parse_string(value, manifest.dependencies[*dependency_index].version);
        }
        else if (section == "dependency" && dependency_index && key == "type")
        {
          valid = parse_string(value, manifest.dependencies[*dependency_index].type);
        }
        else if (section == "dependency" && dependency_index && key == "path")
        {
          std::string dependency_path;
          valid = parse_string(value, dependency_path)
            && dependency_path.find('\\') == std::string::npos;
          manifest.dependencies[*dependency_index].path = dependency_path;
        }
        else if (section == "dependency" && dependency_index && key == "sha256")
        {
          valid = parse_string(value, manifest.dependencies[*dependency_index].sha256);
        }
        else if (section == "component" && component_index && key == "name")
        {
          valid = parse_string(value, manifest.components[*component_index].name);
        }
        else if (section == "component" && component_index && key == "type")
        {
          valid = parse_string(value, manifest.components[*component_index].type);
        }
        else if (section == "component" && component_index && key == "path")
        {
          std::string component_path;
          valid = parse_string(value, component_path)
            && component_path.find('\\') == std::string::npos;
          manifest.components[*component_index].path = component_path;
        }
        else if (section == "component" && component_index && key == "sha256")
        {
          valid = parse_string(value, manifest.components[*component_index].sha256);
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

      if (manifest.format != 3 && !seen.contains("package.type"))
      {
        error << "forge: box manifest is missing 'package.type'\n";
        return false;
      }

      if (manifest.type == "shared_library")
      {
        manifest.type = "dynamic_library";
      }

      for (auto& artifact : manifest.artifacts)
      {
        if (artifact.kind == "shared_library")
        {
          artifact.kind = "dynamic_library";
        }
      }

      for (auto& dependency : manifest.dependencies)
      {
        if (dependency.type == "shared_library")
        {
          dependency.type = "dynamic_library";
        }
      }

      if (manifest.format != 1 && manifest.format != 2 && manifest.format != 3)
      {
        error << "forge: unsupported box format " << manifest.format << '\n';
        return false;
      }

      if (manifest.toolchain
          && (manifest.toolchain->compiler.empty()
              || manifest.toolchain->compiler_version.empty()
              || manifest.toolchain->cpp_standard == 0
              || manifest.toolchain->configuration.empty()
              || manifest.toolchain->runtime.empty()))
      {
        error << "forge: box manifest contains incomplete toolchain identity\n";
        return false;
      }

      if (!is_safe_path_component(manifest.name)
          || !is_safe_path_component(manifest.version)
          || (manifest.format != 3
              && manifest.type != "executable"
              && manifest.type != "static_library"
              && manifest.type != "dynamic_library"
              && manifest.type != "imported_library"
              && manifest.type != "header_only")
          || (manifest.format == 3 ? manifest.components.empty() : manifest.artifacts.empty()))
      {
        error << "forge: box manifest contains invalid package or artifact values\n";
        return false;
      }

      std::set<std::filesystem::path> artifact_paths;
      std::set<std::string> dependency_names;
      std::set<std::string> component_names;
      std::size_t executable_count = 0;
      std::size_t library_count = 0;
      std::size_t dynamic_library_count = 0;
      std::size_t import_library_count = 0;
      std::size_t header_count = 0;
      std::size_t runtime_asset_count = 0;

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
        else if (artifact.kind == "dynamic_library" && prefix == "runtime")
        {
          ++dynamic_library_count;
        }
        else if (artifact.kind == "import_library" && prefix == "lib")
        {
          ++import_library_count;
        }
        else if (artifact.kind == "public_header" && prefix == "include")
        {
          ++header_count;
        }
        else if (artifact.kind == "runtime_asset" && prefix == "runtime-assets")
        {
          ++runtime_asset_count;
        }
        else
        {
          error << "forge: box manifest contains invalid package or artifact values\n";
          return false;
        }
      }

      if (manifest.format == 1 && !manifest.dependencies.empty())
      {
        error << "forge: format 1 boxes cannot declare dependencies\n";
        return false;
      }

      for (std::size_t index = 0; index < manifest.dependencies.size(); ++index)
      {
        const auto& dependency = manifest.dependencies[index];
        const auto prefix = dependency.path.empty() ? std::string {} : dependency.path.begin()->string();

        if (!seen.contains("dependency." + std::to_string(index) + ".name")
            || !seen.contains("dependency." + std::to_string(index) + ".version")
            || !seen.contains("dependency." + std::to_string(index) + ".type")
            || !seen.contains("dependency." + std::to_string(index) + ".path")
            || !seen.contains("dependency." + std::to_string(index) + ".sha256")
            || !is_safe_path_component(dependency.name)
            || !is_safe_path_component(dependency.version)
            || (dependency.type != "static_library"
                && dependency.type != "dynamic_library"
                && dependency.type != "imported_library"
                && dependency.type != "header_only")
            || !is_safe_archive_path(dependency.path)
            || dependency.path.parent_path().empty()
            || prefix != "dependencies"
            || dependency.path.extension() != ".cbox"
            || !artifact_paths.insert(dependency.path).second
            || !dependency_names.insert(dependency.name).second
            || dependency.name == manifest.name
            || dependency.sha256.size() != 64
            || dependency.sha256.find_first_not_of("0123456789abcdef") != std::string::npos)
        {
          error << "forge: box manifest contains invalid dependency values\n";
          return false;
        }
      }

      for (std::size_t index = 0; index < manifest.components.size(); ++index)
      {
        const auto& component = manifest.components[index];
        const auto prefix = component.path.empty() ? std::string {} : component.path.begin()->string();

        if (!seen.contains("component." + std::to_string(index) + ".name")
            || !seen.contains("component." + std::to_string(index) + ".type")
            || !seen.contains("component." + std::to_string(index) + ".path")
            || !seen.contains("component." + std::to_string(index) + ".sha256")
            || !is_safe_path_component(component.name)
            || (component.type != "executable"
                && component.type != "static_library"
                && component.type != "dynamic_library"
                && component.type != "imported_library"
                && component.type != "header_only")
            || !is_safe_archive_path(component.path)
            || component.path.parent_path().empty()
            || prefix != "components"
            || component.path.extension() != ".cbox"
            || !artifact_paths.insert(component.path).second
            || !component_names.insert(component.name).second
            || component.sha256.size() != 64
            || component.sha256.find_first_not_of("0123456789abcdef") != std::string::npos)
        {
          error << "forge: box manifest contains invalid component values\n";
          return false;
        }
      }

      if (manifest.format == 3
          && (!manifest.artifacts.empty() || !manifest.dependencies.empty()))
      {
        error << "forge: format 3 container boxes may contain only components\n";
        return false;
      }

      if (manifest.format != 3
          && ((manifest.type == "executable"
           && (executable_count != 1
               || executable_count + runtime_asset_count != manifest.artifacts.size()))
          || (manifest.type == "static_library"
              && (library_count != 1
                  || header_count == 0
                  || library_count + header_count != manifest.artifacts.size()))
          || (manifest.type == "dynamic_library"
              && (dynamic_library_count != 1
                  || import_library_count != (manifest.os == "windows" ? 1 : 0)
                  || header_count == 0
                  || dynamic_library_count + import_library_count + header_count
                     != manifest.artifacts.size()))
          || (manifest.type == "imported_library"
              && (header_count == 0
                  || library_count + dynamic_library_count + import_library_count == 0
                  || library_count + dynamic_library_count + import_library_count + header_count
                     != manifest.artifacts.size()))
          || (manifest.type == "header_only"
              && (header_count == 0 || header_count != manifest.artifacts.size()))))
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

      for (const auto& dependency : manifest.dependencies)
      {
        if (!std::filesystem::is_regular_file(directory / dependency.path))
        {
          error << "forge: box dependency '" << dependency.path.string() << "' is missing\n";
          return false;
        }

        expected_files.insert(dependency.path);
        auto parent = dependency.path.parent_path();

        while (!parent.empty())
        {
          expected_directories.insert(parent);
          parent = parent.parent_path();
        }
      }

      for (const auto& component : manifest.components)
      {
        if (!std::filesystem::is_regular_file(directory / component.path))
        {
          error << "forge: box component '" << component.path.string() << "' is missing\n";
          return false;
        }

        expected_files.insert(component.path);
        auto parent = component.path.parent_path();

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

      for (const auto& dependency : manifest.dependencies)
      {
        expected_archive_entries.insert(dependency.path.generic_string());
      }

      for (const auto& component : manifest.components)
      {
        expected_archive_entries.insert(component.path.generic_string());
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

      for (const auto& dependency : manifest.dependencies)
      {
        std::string checksum;

        if (!sha256_file(directory / dependency.path, checksum, error))
        {
          return false;
        }

        if (checksum != dependency.sha256)
        {
          error << "forge: box dependency checksum does not match cbox.toml\n";
          return false;
        }
      }

      for (const auto& component : manifest.components)
      {
        std::string checksum;

        if (!sha256_file(directory / component.path, checksum, error))
        {
          return false;
        }

        if (checksum != component.sha256)
        {
          error << "forge: box component checksum does not match cbox.toml\n";
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

    int create_container_box(const std::filesystem::path& project_directory,
                             const Recipe& recipe,
                             const ProcessRunner& process_runner,
                             std::ostream& output,
                             std::ostream& error)
    {
      const auto header_only = std::ranges::all_of(
        recipe.targets,
        [](const RecipeTarget& target)
        {
          return target.type == "header_only";
        }
      );
      const auto box_name =
        recipe.name + "-" + package_version(recipe)
        + (header_only ? "-ho" : "-" + target_os() + "-" + target_arch());
      const auto boxes_directory = project_directory / ".forge" / "boxes";
      const auto staging_directory = boxes_directory / "staging" / ("container-" + box_name);
      const auto archive_path = boxes_directory / (box_name + ".cbox");

      if (!prepare_empty_directory(staging_directory, error))
      {
        return 2;
      }

      std::vector<BoxComponent> components;

      for (const auto& target : recipe.targets)
      {
        if (create_box(
          project_directory,
          target.name,
          process_runner,
          output,
          error
        ) != 0)
        {
          return 2;
        }

        Recipe selected = recipe;

        if (!select_recipe_target(selected, target.name, error))
        {
          return 2;
        }

        const auto source = boxes_directory / box_filename(selected);
        const auto component_path =
          std::filesystem::path { "components" } / (target.name + ".cbox");
        const auto destination = staging_directory / component_path;
        std::error_code filesystem_error;
        std::filesystem::create_directories(destination.parent_path(), filesystem_error);
        std::string checksum;

        if (filesystem_error
            || !copy_file(source, destination, error)
            || !sha256_file(destination, checksum, error))
        {
          error << "forge: could not package component '" << target.name << "'\n";
          return 2;
        }

        components.push_back({ target.name, target.type, component_path, checksum });
      }

      std::ofstream manifest { staging_directory / "cbox.toml" };

      if (!manifest)
      {
        error << "forge: could not create container manifest\n";
        return 2;
      }

      manifest
        << "[cbox]\n"
        << "format = 3\n\n"
        << "[package]\n"
        << "name = \"" << recipe.name << "\"\n"
        << "version = \"" << recipe.version << "\"\n";

      if (recipe.build_number)
      {
        manifest << "build = " << *recipe.build_number << "\n";
      }

      manifest
        << "\n[target]\n"
        << "os = \"" << target_os() << "\"\n"
        << "arch = \"" << target_arch() << "\"\n";

      for (const auto& component : components)
      {
        manifest
          << "\n[[component]]\n"
          << "name = \"" << component.name << "\"\n"
          << "type = \"" << component.type << "\"\n"
          << "path = \"" << component.path.generic_string() << "\"\n"
          << "sha256 = \"" << component.sha256 << "\"\n";
      }

      manifest.close();
      std::error_code filesystem_error;
      std::filesystem::remove(archive_path, filesystem_error);
      output << "Creating box " << box_name << '\n' << std::flush;

      if (process_runner(
        {
          "cmake",
          "-E",
          "tar",
          "cf",
          archive_path.string(),
          "--format=zip",
          "cbox.toml",
          "components"
        },
        staging_directory,
        error
      ) != 0)
      {
        error << "forge: box archive creation failed\n";
        return 2;
      }

      output << "Created " << archive_path.string() << '\n';
      return 0;
    }

  } // namespace

  int create_box(const std::filesystem::path& project_directory,
                 std::ostream& output,
                 std::ostream& error)
  {
    return create_box(project_directory, std::nullopt, run_process, output, error);
  }

  int create_box(const std::filesystem::path& project_directory,
                 const std::optional<std::string>& target,
                 std::ostream& output,
                 std::ostream& error)
  {
    return create_box(project_directory, target, run_process, output, error);
  }

  int create_box(const std::filesystem::path& project_directory,
                 const ProcessRunner& process_runner,
                 std::ostream& output,
                 std::ostream& error)
  {
    return create_box(project_directory, std::nullopt, process_runner, output, error);
  }

  int create_box(const std::filesystem::path& project_directory,
                 const std::optional<std::string>& target,
                 const ProcessRunner& process_runner,
                 std::ostream& output,
                 std::ostream& error)
  {
    Recipe recipe;

    if (!read_recipe(project_directory / "forge.recipe.toml", recipe, error))
    {
      return 2;
    }

    if (!target && recipe.targets.size() > 1)
    {
      return create_container_box(project_directory, recipe, process_runner, output, error);
    }

    if (!select_recipe_target(recipe, target, error))
    {
      return 2;
    }

    BuildOptions options;
    options.target = target;

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

    auto built_artifact = build_directory / recipe.name;
    std::optional<std::filesystem::path> built_import_library;

#ifdef _WIN32
    if (recipe.type == "static_library")
    {
      built_artifact += ".lib";
    }
    else if (recipe.type == "dynamic_library")
    {
      built_artifact = build_directory / dynamic_library_filename(recipe.name);
      built_import_library = build_directory / import_library_filename(recipe.name);
    }
    else
    {
      built_artifact += ".exe";
    }
#else
    if (recipe.type == "static_library")
    {
      built_artifact = build_directory / ("lib" + recipe.name + ".a");
    }
    else if (recipe.type == "dynamic_library")
    {
      built_artifact = build_directory / dynamic_library_filename(recipe.name);
    }
#endif

    if (recipe.type != "header_only"
        && recipe.type != "imported_library"
        && !std::filesystem::is_regular_file(built_artifact))
    {
      error << "forge: built artifact '" << built_artifact.string() << "' does not exist\n";
      return 2;
    }

    if (built_import_library && !std::filesystem::is_regular_file(*built_import_library))
    {
      error << "forge: built import library '" << built_import_library->string()
            << "' does not exist\n";
      return 2;
    }

    const auto archive_filename = box_filename(recipe);
    const auto box_name = std::filesystem::path { archive_filename }.stem().string();
    const auto boxes_directory = project_directory / ".forge" / "boxes";
    const auto staging_directory = boxes_directory / "staging" / box_name;
    const auto archive_path = boxes_directory / archive_filename;

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

      std::vector<RuntimeAsset> runtime_assets;

      if (!collect_runtime_assets(project_directory, recipe.runtime_files, runtime_assets, error))
      {
        return 2;
      }

      for (const auto& asset : runtime_assets)
      {
        if (!stage_artifact(
          asset.source,
          std::filesystem::path { "runtime-assets" } / asset.path,
          "runtime_asset",
          staging_directory,
          artifacts,
          error
        ))
        {
          return 2;
        }
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
    else if (recipe.type == "dynamic_library")
    {
      if (!stage_artifact(
        built_artifact,
        std::filesystem::path { "runtime" } / built_artifact.filename(),
        "dynamic_library",
        staging_directory,
        artifacts,
        error
      ))
      {
        return 2;
      }

      if (built_import_library
          && !stage_artifact(
            *built_import_library,
            std::filesystem::path { "lib" } / built_import_library->filename(),
            "import_library",
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
    else if (recipe.type == "imported_library")
    {
      if (!recipe.dependencies.empty())
      {
        error << "forge: imported_library dependencies are not supported yet\n";
        return 2;
      }

      const auto profile = std::find_if(
        recipe.imports.begin(),
        recipe.imports.end(),
        [](const ImportProfile& candidate)
        {
          return candidate.target == current_target();
        }
      );

      if (profile == recipe.imports.end())
      {
        error << "forge: imported_library has no import profile for " << current_target() << '\n';
        return 2;
      }

      if (profile->public_headers.empty()
          || (profile->static_libraries.empty()
              && profile->dynamic_libraries.empty()
              && profile->import_libraries.empty()))
      {
        error << "forge: imported_library profile requires headers and at least one library\n";
        return 2;
      }

      for (const auto& headers : profile->public_headers)
      {
        if (!stage_imported_headers(
          project_directory,
          headers,
          staging_directory,
          artifacts,
          error
        ))
        {
          return 2;
        }
      }

      if (!stage_imported_libraries(
        project_directory,
        profile->static_libraries,
        "lib",
        "static_library",
        staging_directory,
        artifacts,
        error
      )
          || !stage_imported_libraries(
            project_directory,
            profile->dynamic_libraries,
            "runtime",
            "dynamic_library",
            staging_directory,
            artifacts,
            error
          )
          || !stage_imported_libraries(
            project_directory,
            profile->import_libraries,
            "lib",
            "import_library",
            staging_directory,
            artifacts,
            error
          ))
      {
        return 2;
      }
    }
    else
    {
      error << "forge: unsupported project type '" << recipe.type << "'\n";
      return 2;
    }

    std::vector<BoxDependency> dependencies;
    std::set<std::string> dependency_names;

    const auto stage_dependency =
      [&](const std::filesystem::path& source, std::string_view dependency_name) -> bool
      {
        if (!dependency_names.insert(std::string { dependency_name }).second)
        {
          error << "forge: box dependency name '" << dependency_name << "' is duplicated\n";
          return false;
        }

        const auto dependency_path =
          std::filesystem::path { "dependencies" } / (std::string { dependency_name } + ".cbox");
        const auto destination = staging_directory / dependency_path;
        BoxMetadata metadata;
        std::string checksum;
        std::error_code dependency_error;
        std::filesystem::create_directories(destination.parent_path(), dependency_error);

        if (dependency_error
            || !copy_file(source, destination, error)
            || !sha256_file(destination, checksum, error)
            || !read_box_metadata(source, project_directory, process_runner, metadata, error))
        {
          error << "forge: could not package dependency '" << dependency_name << "'\n";
          return false;
        }

        dependencies.push_back(BoxDependency
          {
            metadata.name,
            metadata.version,
            metadata.type,
            dependency_path,
            checksum
          });
        return true;
      };

    for (const auto& dependency_name : recipe.selected_internal_dependencies)
    {
      Recipe dependency_recipe;

      if (!read_recipe(project_directory / "forge.recipe.toml", dependency_recipe, error)
          || !select_recipe_target(dependency_recipe, dependency_name, error)
          || create_box(
            project_directory,
            dependency_name,
            process_runner,
            output,
            error
          ) != 0)
      {
        return 2;
      }

      const auto source = project_directory / ".forge" / "boxes"
        / box_filename(dependency_recipe);

      if (!stage_dependency(source, dependency_name))
      {
        return 2;
      }
    }

    for (const auto& dependency : recipe.dependencies)
    {
      const auto source = project_directory / ".forge" / "dependency-boxes" / (dependency.name + ".cbox");

      if (!stage_dependency(source, dependency.name))
      {
        return 2;
      }
    }

    std::optional<ToolchainIdentity> toolchain;

    if (recipe.type == "imported_library")
    {
      const auto profile = std::find_if(
        recipe.imports.begin(),
        recipe.imports.end(),
        [](const ImportProfile& candidate)
        {
          return candidate.target == current_target();
        }
      );

      if (profile == recipe.imports.end())
      {
        error << "forge: imported_library has no import profile for " << current_target() << '\n';
        return 2;
      }

      toolchain =
        {
          profile->compiler,
          profile->compiler_version,
          profile->cpp_standard,
          profile->configuration,
          profile->runtime
        };

      if (toolchain->compiler.empty()
          || toolchain->compiler_version.empty()
          || toolchain->cpp_standard == 0
          || toolchain->configuration.empty()
          || toolchain->runtime.empty())
      {
        error << "forge: imported_library profile requires a complete toolchain identity\n";
        return 2;
      }
    }
    else if (recipe.type != "header_only"
             && !read_toolchain(
               build_directory / "forge-toolchain.toml",
               toolchain.emplace(),
               error
             ))
    {
      return 2;
    }

    if (!write_manifest(
      staging_directory / "cbox.toml",
      recipe,
      toolchain,
      artifacts,
      dependencies,
      error
    ))
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

      if (std::filesystem::is_directory(staging_directory / "runtime-assets"))
      {
        archive_arguments.push_back("runtime-assets");
      }
    }
    else if (recipe.type == "static_library")
    {
      archive_arguments.push_back("include");
      archive_arguments.push_back("lib");
    }
    else if (recipe.type == "dynamic_library")
    {
      archive_arguments.push_back("include");
      if (built_import_library)
      {
        archive_arguments.push_back("lib");
      }
      archive_arguments.push_back("runtime");
    }
    else if (recipe.type == "imported_library")
    {
      archive_arguments.push_back("include");

      if (std::filesystem::is_directory(staging_directory / "lib"))
      {
        archive_arguments.push_back("lib");
      }

      if (std::filesystem::is_directory(staging_directory / "runtime"))
      {
        archive_arguments.push_back("runtime");
      }
    }
    else
    {
      archive_arguments.push_back("include");
    }

    if (!dependencies.empty())
    {
      archive_arguments.push_back("dependencies");
    }

    if (process_runner(archive_arguments, staging_directory, error) != 0)
    {
      error << "forge: box archive creation failed\n";
      return 2;
    }

    output << "Created " << archive_path.string() << '\n';
    return 0;
  }

  int list_boxes(const std::filesystem::path& project_directory,
                 std::ostream& output,
                 std::ostream& error)
  {
    if (!std::filesystem::is_regular_file(project_directory / "forge.recipe.toml"))
    {
      error << "forge: forge.recipe.toml was not found in the current directory\n";
      return 2;
    }

    const auto list_directory =
      [&output](std::string_view heading, const std::filesystem::path& directory)
      {
        std::vector<std::filesystem::path> boxes;
        std::error_code filesystem_error;

        if (std::filesystem::is_directory(directory))
        {
          for (const auto& entry : std::filesystem::directory_iterator { directory, filesystem_error })
          {
            if (filesystem_error)
            {
              break;
            }

            if (entry.is_regular_file() && entry.path().extension() == ".cbox")
            {
              boxes.push_back(entry.path().filename());
            }
          }
        }

        std::ranges::sort(boxes);

        if (!boxes.empty())
        {
          output << heading << ":\n";

          for (const auto& box : boxes)
          {
            output << "  " << box.string() << '\n';
          }
        }

        return boxes.size();
      };

    const auto generated = list_directory("Generated boxes", project_directory / ".forge" / "boxes");
    const auto published = list_directory("Published boxes", project_directory / "boxes");

    if (generated + published == 0)
    {
      output << "No boxes found\n";
    }

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

    BoxMetadata metadata;

    if (!read_box_metadata(resolved_box, working_directory, process_runner, metadata, error))
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

    for (const auto& dependency : manifest.dependencies)
    {
      std::error_code filesystem_error;
      std::filesystem::create_directories(
        (destination / dependency.path).parent_path(),
        filesystem_error
      );

      if (filesystem_error
          || !copy_file(
            validation_directory / dependency.path,
            destination / dependency.path,
            error
          ))
      {
        return 2;
      }
    }

    for (const auto& component : manifest.components)
    {
      std::error_code filesystem_error;
      std::filesystem::create_directories(
        (destination / component.path).parent_path(),
        filesystem_error
      );

      if (filesystem_error
          || !copy_file(
            validation_directory / component.path,
            destination / component.path,
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

    std::string box_checksum;

    if (!sha256_file(resolved_box, box_checksum, error))
    {
      return false;
    }

    const auto cache_root = std::filesystem::is_directory(working_directory)
      ? working_directory
      : working_directory.parent_path();
    const auto validation_directory =
      cache_root / ".forge" / "cache" / "box-metadata" / box_checksum;
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
        manifest.toolchain,
        {},
        {},
        {}
      };

    for (const auto& artifact : manifest.artifacts)
    {
      metadata.artifacts.push_back({ artifact.path, artifact.kind });
    }

    for (const auto& dependency : manifest.dependencies)
    {
      BoxMetadata child;

      if (!read_box_metadata(
        validation_directory / dependency.path,
        validation_directory,
        process_runner,
        child,
        error
      ))
      {
        return false;
      }

      if (child.name != dependency.name
          || child.version != dependency.version
          || child.type != dependency.type
          || child.os != manifest.os
          || child.arch != manifest.arch)
      {
        error << "forge: embedded dependency '" << dependency.name
              << "' does not match its declaration\n";
        return false;
      }

      metadata.dependencies.push_back(
        {
          dependency.name,
          dependency.version,
          dependency.type,
          validation_directory / dependency.path,
          dependency.sha256
        }
      );
    }

    for (const auto& component : manifest.components)
    {
      BoxMetadata child;

      if (!read_box_metadata(
        validation_directory / component.path,
        validation_directory,
        process_runner,
        child,
        error
      )
          || child.name != component.name
          || child.type != component.type)
      {
        error << "forge: embedded component '" << component.name
              << "' does not match its declaration\n";
        return false;
      }

      metadata.components.push_back(
        {
          component.name,
          component.type,
          component.path,
          component.sha256
        }
      );
    }

    return true;
  }

  bool resolve_box_component(const std::filesystem::path& box_path,
                             const std::filesystem::path& working_directory,
                             const std::optional<std::string>& requested,
                             const ProcessRunner& process_runner,
                             std::filesystem::path& component_box,
                             BoxMetadata& metadata,
                             std::ostream& error,
                             BoxMetadata* container_metadata)
  {
    const auto resolved_box = resolve_box_path(box_path, working_directory);
    BoxMetadata container;

    if (!read_box_metadata(resolved_box, working_directory, process_runner, container, error))
    {
      return false;
    }

    if (container_metadata != nullptr)
    {
      *container_metadata = container;
    }

    if (container.components.empty())
    {
      component_box = resolved_box;
      metadata = std::move(container);
      return true;
    }

    const auto component = requested
      ? std::find_if(
          container.components.begin(),
          container.components.end(),
          [&requested](const BoxComponentMetadata& candidate)
          {
            return candidate.name == *requested;
          }
        )
      : std::find_if(
          container.components.begin(),
          container.components.end(),
          [&container](const BoxComponentMetadata& candidate)
          {
            return candidate.name == container.name
              && candidate.type != "executable";
          }
        );

    if (component == container.components.end())
    {
      error << "forge: box contains multiple components; specify one of:";

      for (const auto& candidate : container.components)
      {
        if (candidate.type != "executable")
        {
          error << ' ' << candidate.name;
        }
      }

      error << '\n';
      return false;
    }

    std::string checksum;

    if (!sha256_file(resolved_box, checksum, error))
    {
      return false;
    }

    const auto validation_directory =
      working_directory / ".forge" / "cache" / "box-metadata" / checksum;
    component_box = validation_directory / component->path;
    return read_box_metadata(component_box, validation_directory, process_runner, metadata, error);
  }

} // namespace forge
