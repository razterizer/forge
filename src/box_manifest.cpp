#include "box_manifest.h"

#include "file_support.h"
#include "target_support.h"

#include <array>
#include <charconv>
#include <fstream>
#include <iterator>
#include <set>
#include <sstream>
#include <string_view>

namespace forge
{
  namespace
  {

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
        return false;

      result = std::string { value.substr(1, value.size() - 2) };
      return result.find('"') == std::string::npos && result.find('\\') == std::string::npos;
    }

    bool parse_integer(std::string_view value, int& result)
    {
      value = trim(value);
      const auto parsed = std::from_chars(value.data(), value.data() + value.size(), result);
      return parsed.ec == std::errc {} && parsed.ptr == value.data() + value.size();
    }

    bool parse_strings(std::string_view value, std::vector<std::string>& strings)
    {
      value = trim(value);

      if (value.size() < 2 || value.front() != '[' || value.back() != ']')
        return false;

      value = trim(value.substr(1, value.size() - 2));
      strings.clear();

      while (!value.empty())
      {
        if (value.front() != '"')
          return false;

        std::size_t end = 1;

        while (end < value.size() && value[end] != '"')
        {
          if (value[end] == '\\')
            ++end;

          ++end;
        }

        std::string parsed;

        if (end >= value.size() || !parse_string(value.substr(0, end + 1), parsed))
          return false;

        strings.push_back(std::move(parsed));
        value = trim(value.substr(end + 1));

        if (value.empty())
          break;

        if (value.front() != ',')
          return false;

        value = trim(value.substr(1));
      }

      return true;
    }

    bool parse_paths(std::string_view value, std::vector<std::filesystem::path>& paths)
    {
      std::vector<std::string> strings;

      if (!parse_strings(value, strings))
        return false;

      paths.clear();

      for (const auto& string : strings)
        paths.emplace_back(string);

      return true;
    }

    void write_string_array(std::ostream& output,
                            std::string_view name,
                            const std::vector<std::string>& values)
    {
      if (values.empty())
        return;

      output << name << " = [";

      for (std::size_t index = 0; index < values.size(); ++index)
        output << (index == 0 ? "\"" : ", \"") << values[index] << "\"";

      output << "]\n";
    }

    void write_path_array(std::ostream& output,
                          std::string_view name,
                          const std::vector<std::filesystem::path>& values)
    {
      if (values.empty())
        return;

      output << name << " = [";

      for (std::size_t index = 0; index < values.size(); ++index)
        output << (index == 0 ? "\"" : ", \"") << values[index].generic_string() << "\"";

      output << "]\n";
    }

    bool is_safe_archive_path(const std::filesystem::path& path)
    {
      if (path.empty() || path.is_absolute() || path.has_root_path())
        return false;

      for (const auto& component : path)
      {
        if (component == "." || component == ".." || component.empty())
          return false;
      }

      return true;
    }


  } // namespace

    bool write_box_manifest(const std::filesystem::path& path,
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
        manifest << "build = " << *recipe.build_number << "\n";

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

      if (!recipe.macos_system_include_directories.empty()
          || !recipe.linux_system_include_directories.empty()
          || !recipe.windows_system_include_directories.empty()
          || !recipe.macos_system_library_directories.empty()
          || !recipe.linux_system_library_directories.empty()
          || !recipe.windows_system_library_directories.empty()
          || !recipe.macos_frameworks.empty()
          || !recipe.macos_libraries.empty()
          || !recipe.linux_libraries.empty()
          || !recipe.windows_libraries.empty())
      {
        manifest << "\n[requirements]\n";
        write_path_array(
          manifest,
          "macos_system_include_dirs",
          recipe.macos_system_include_directories
        );
        write_path_array(
          manifest,
          "linux_system_include_dirs",
          recipe.linux_system_include_directories
        );
        write_path_array(
          manifest,
          "windows_system_include_dirs",
          recipe.windows_system_include_directories
        );
        write_path_array(
          manifest,
          "macos_system_library_dirs",
          recipe.macos_system_library_directories
        );
        write_path_array(
          manifest,
          "linux_system_library_dirs",
          recipe.linux_system_library_directories
        );
        write_path_array(
          manifest,
          "windows_system_library_dirs",
          recipe.windows_system_library_directories
        );
        write_string_array(manifest, "macos_frameworks", recipe.macos_frameworks);
        write_string_array(manifest, "macos_libraries", recipe.macos_libraries);
        write_string_array(manifest, "linux_libraries", recipe.linux_libraries);
        write_string_array(manifest, "windows_libraries", recipe.windows_libraries);
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

    bool read_box_manifest(const std::filesystem::path& path,
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
          continue;

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
              && section != "requirements"
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
          valid = parse_integer(value, manifest.format);
        else if (identity == "package.name")
          valid = parse_string(value, manifest.name);
        else if (identity == "package.version")
          valid = parse_string(value, manifest.version);
        else if (identity == "package.build")
        {
          int build_number = 0;
          valid = parse_integer(value, build_number) && build_number >= 0;

          if (valid)
            manifest.build_number = build_number;
        }
        else if (identity == "package.type")
          valid = parse_string(value, manifest.type);
        else if (identity == "target.os")
          valid = parse_string(value, manifest.os);
        else if (identity == "target.arch")
          valid = parse_string(value, manifest.arch);
        else if (identity == "toolchain.compiler")
        {
          if (!manifest.toolchain)
            manifest.toolchain.emplace();
          valid = parse_string(value, manifest.toolchain->compiler);
        }
        else if (identity == "toolchain.compiler_version")
        {
          if (!manifest.toolchain)
            manifest.toolchain.emplace();
          valid = parse_string(value, manifest.toolchain->compiler_version);
        }
        else if (identity == "toolchain.cpp_std")
        {
          if (!manifest.toolchain)
            manifest.toolchain.emplace();
          valid = parse_integer(value, manifest.toolchain->cpp_standard);
        }
        else if (identity == "toolchain.configuration")
        {
          if (!manifest.toolchain)
            manifest.toolchain.emplace();
          valid = parse_string(value, manifest.toolchain->configuration);
        }
        else if (identity == "toolchain.runtime")
        {
          if (!manifest.toolchain)
            manifest.toolchain.emplace();
          valid = parse_string(value, manifest.toolchain->runtime);
        }
        else if (identity == "requirements.macos_system_include_dirs")
          valid = parse_paths(value, manifest.macos_system_include_directories);
        else if (identity == "requirements.linux_system_include_dirs")
          valid = parse_paths(value, manifest.linux_system_include_directories);
        else if (identity == "requirements.windows_system_include_dirs")
          valid = parse_paths(value, manifest.windows_system_include_directories);
        else if (identity == "requirements.macos_system_library_dirs")
          valid = parse_paths(value, manifest.macos_system_library_directories);
        else if (identity == "requirements.linux_system_library_dirs")
          valid = parse_paths(value, manifest.linux_system_library_directories);
        else if (identity == "requirements.windows_system_library_dirs")
          valid = parse_paths(value, manifest.windows_system_library_directories);
        else if (identity == "requirements.macos_frameworks")
          valid = parse_strings(value, manifest.macos_frameworks);
        else if (identity == "requirements.macos_libraries")
          valid = parse_strings(value, manifest.macos_libraries);
        else if (identity == "requirements.linux_libraries")
          valid = parse_strings(value, manifest.linux_libraries);
        else if (identity == "requirements.windows_libraries")
          valid = parse_strings(value, manifest.windows_libraries);
        else if (section == "artifact" && artifact_index && key == "path")
        {
          std::string artifact_path;
          valid = parse_string(value, artifact_path)
            && artifact_path.find('\\') == std::string::npos;
          manifest.artifacts[*artifact_index].path = artifact_path;
        }
        else if (section == "artifact" && artifact_index && key == "kind")
          valid = parse_string(value, manifest.artifacts[*artifact_index].kind);
        else if (section == "artifact" && artifact_index && key == "sha256")
          valid = parse_string(value, manifest.artifacts[*artifact_index].sha256);
        else if (section == "dependency" && dependency_index && key == "name")
          valid = parse_string(value, manifest.dependencies[*dependency_index].name);
        else if (section == "dependency" && dependency_index && key == "version")
          valid = parse_string(value, manifest.dependencies[*dependency_index].version);
        else if (section == "dependency" && dependency_index && key == "type")
          valid = parse_string(value, manifest.dependencies[*dependency_index].type);
        else if (section == "dependency" && dependency_index && key == "path")
        {
          std::string dependency_path;
          valid = parse_string(value, dependency_path)
            && dependency_path.find('\\') == std::string::npos;
          manifest.dependencies[*dependency_index].path = dependency_path;
        }
        else if (section == "dependency" && dependency_index && key == "sha256")
          valid = parse_string(value, manifest.dependencies[*dependency_index].sha256);
        else if (section == "component" && component_index && key == "name")
          valid = parse_string(value, manifest.components[*component_index].name);
        else if (section == "component" && component_index && key == "type")
          valid = parse_string(value, manifest.components[*component_index].type);
        else if (section == "component" && component_index && key == "path")
        {
          std::string component_path;
          valid = parse_string(value, component_path)
            && component_path.find('\\') == std::string::npos;
          manifest.components[*component_index].path = component_path;
        }
        else if (section == "component" && component_index && key == "sha256")
          valid = parse_string(value, manifest.components[*component_index].sha256);
        else
          valid = false;

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
        manifest.type = "dynamic_library";

      for (auto& artifact : manifest.artifacts)
      {
        if (artifact.kind == "shared_library")
          artifact.kind = "dynamic_library";
      }

      for (auto& dependency : manifest.dependencies)
      {
        if (dependency.type == "shared_library")
          dependency.type = "dynamic_library";
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
          ++executable_count;
        else if (artifact.kind == "static_library" && prefix == "lib")
          ++library_count;
        else if (artifact.kind == "dynamic_library" && prefix == "runtime")
          ++dynamic_library_count;
        else if (artifact.kind == "import_library" && prefix == "lib")
          ++import_library_count;
        else if (artifact.kind == "public_header" && prefix == "include")
          ++header_count;
        else if (artifact.kind == "runtime_asset" && prefix == "runtime-assets")
          ++runtime_asset_count;
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
                  || library_count + header_count + runtime_asset_count
                     != manifest.artifacts.size()))
          || (manifest.type == "dynamic_library"
              && (dynamic_library_count != 1
                  || import_library_count != (manifest.os == "windows" ? 1 : 0)
                  || header_count == 0
                  || dynamic_library_count + import_library_count + header_count
                     + runtime_asset_count
                     != manifest.artifacts.size()))
          || (manifest.type == "imported_library"
              && (header_count == 0
                  || library_count + dynamic_library_count + import_library_count == 0
                  || library_count + dynamic_library_count + import_library_count + header_count
                     != manifest.artifacts.size()))
          || (manifest.type == "header_only"
              && (header_count == 0
                  || header_count + runtime_asset_count != manifest.artifacts.size()))))
      {
        error << "forge: box manifest artifacts do not match package type\n";
        return false;
      }

      return true;
    }


} // namespace forge
