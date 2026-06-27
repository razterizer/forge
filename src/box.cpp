#include "box.h"

#include "box_manifest.h"

#include "build.h"
#include "file_support.h"
#include "recipe.h"
#include "runtime_assets.h"
#include "sha256.h"
#include "target_support.h"
#include "zip.h"

#include <algorithm>
#include <charconv>
#include <fstream>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

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
          continue;

        const auto key = trim(content.substr(0, equals));
        const auto value = trim(content.substr(equals + 1));
        bool valid = true;

        if (key == "compiler")
          valid = parse_string(value, toolchain.compiler);
        else if (key == "compiler_version")
          valid = parse_string(value, toolchain.compiler_version);
        else if (key == "cpp_std")
          valid = parse_integer(value, toolchain.cpp_standard);
        else if (key == "configuration")
          valid = parse_string(value, toolchain.configuration);
        else if (key == "runtime")
          valid = parse_string(value, toolchain.runtime);
        else
          valid = false;

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
        return false;

      artifacts.push_back(BoxArtifact
        {
          artifact_path,
          std::string { kind },
          checksum
        });
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
          continue;

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

    std::string box_variant_suffix(const Recipe& recipe,
                                   const std::optional<std::string>& profile)
    {
      if (!profile)
      {
        return {};
      }

      const auto variant = std::find_if(
        recipe.box_variants.begin(),
        recipe.box_variants.end(),
        [&profile](const ReleaseVariant& candidate)
        {
          return candidate.profile == *profile;
        }
      );

      return variant == recipe.box_variants.end() ? std::string {} : variant->suffix;
    }

    std::string box_filename(const Recipe& recipe,
                             std::string_view variant = {},
                             bool force_target_qualified = false)
    {
      auto filename = recipe.name + "-" + package_version(recipe);

      if (!variant.empty())
      {
        filename += "-" + std::string { variant };
      }

      if (recipe.type == "header_only"
          && !force_target_qualified
          && !has_platform_specific_requirements(recipe))
      {
        filename += "-ho";
      }
      else
        filename += "-" + target_os() + "-" + target_arch();

      return filename + ".cbox";
    }

    bool dependencies_require_target_qualified_box(
      const std::filesystem::path& project_directory,
      const Recipe& recipe,
      const ProcessRunner& process_runner,
      std::ostream& error)
    {
      const auto dependency_boxes_directory = project_directory / ".forge" / "dependency-boxes";

      for (const auto& dependency : recipe.dependencies)
      {
        if (!dependency_matches_current_target(dependency))
          continue;

        BoxMetadata metadata;
        const auto box = dependency_boxes_directory / (dependency.name + ".cbox");

        if (!read_box_metadata(box, project_directory, process_runner, metadata, error))
          return true;

        if (metadata.type != "header_only"
            || !metadata.macos_system_include_directories.empty()
            || !metadata.linux_system_include_directories.empty()
            || !metadata.windows_system_include_directories.empty()
            || !metadata.macos_system_library_directories.empty()
            || !metadata.linux_system_library_directories.empty()
            || !metadata.windows_system_library_directories.empty()
            || !metadata.macos_frameworks.empty()
            || !metadata.macos_libraries.empty()
            || !metadata.linux_libraries.empty()
            || !metadata.windows_libraries.empty())
        {
          return true;
        }
      }

      return false;
    }

    std::filesystem::path resolve_box_path(const std::filesystem::path& path,
                                           const std::filesystem::path& working_directory)
    {
      if (path.is_absolute())
        return path;

      const auto relative_path = working_directory / path;

      if (std::filesystem::is_regular_file(relative_path) || path.has_parent_path())
        return relative_path;

      const auto generated_box = working_directory / ".forge" / "boxes" / path;

      if (std::filesystem::is_regular_file(generated_box))
        return generated_box;

      const auto published_box = working_directory / "boxes" / path;
      return std::filesystem::is_regular_file(published_box) ? published_box : relative_path;
    }

    bool is_portable_header_only_filename(const std::filesystem::path& path)
    {
      return path.filename().string().ends_with("-ho.cbox");
    }

    std::filesystem::path box_metadata_cache_root(const std::filesystem::path& working_directory)
    {
      auto directory = std::filesystem::is_directory(working_directory)
        ? working_directory
        : working_directory.parent_path();

      for (auto candidate = directory; !candidate.empty(); candidate = candidate.parent_path())
      {
        if (candidate.filename() == "box-metadata"
            && candidate.parent_path().filename() == "cache"
            && candidate.parent_path().parent_path().filename() == ".forge")
        {
          return candidate;
        }

        if (candidate == candidate.parent_path())
          break;
      }

      return directory / ".forge" / "cache" / "box-metadata";
    }

    bool validate_extracted_box(const std::filesystem::path& directory,
                                const std::vector<std::string>& archive_entries,
                                BoxManifest& manifest,
                                std::string& manifest_content,
                                std::ostream& error)
    {
      if (!read_box_manifest(directory / "cbox.toml", manifest, manifest_content, error))
        return false;

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
        expected_archive_entries.insert(artifact.path.generic_string());

      for (const auto& dependency : manifest.dependencies)
        expected_archive_entries.insert(dependency.path.generic_string());

      for (const auto& component : manifest.components)
        expected_archive_entries.insert(component.path.generic_string());

      for (const auto& directory_path : expected_directories)
        expected_archive_entries.insert(directory_path.generic_string() + "/");

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
          return false;

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
          return false;

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
          return false;

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
        return false;

      std::vector<std::string> archive_entries;

      if (!read_zip_entries(resolved_box, archive_entries, error))
        return false;

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
                             const std::optional<std::string>& profile,
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
        return 2;

      std::vector<BoxComponent> components;

      for (const auto& target : recipe.targets)
      {
        if (create_box(
          project_directory,
          target.name,
          profile,
          process_runner,
          output,
          error
        ) != 0)
        {
          return 2;
        }

        Recipe selected = recipe;

        if (!select_recipe_target(selected, target.name, error))
          return 2;

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
        manifest << "build = " << *recipe.build_number << "\n";

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
    return create_box(project_directory, target, std::nullopt, process_runner, output, error);
  }

  int create_box(const std::filesystem::path& project_directory,
                 const std::optional<std::string>& target,
                 const std::optional<std::string>& profile,
                 const ProcessRunner& process_runner,
                 std::ostream& output,
                 std::ostream& error)
  {
    Recipe recipe;

    if (!read_recipe(project_directory / "forge.recipe.toml", recipe, error))
      return 2;

    if (!target && recipe.targets.size() > 1)
      return create_container_box(project_directory, recipe, profile, process_runner, output, error);

    if (!select_recipe_target(recipe, target, error))
      return 2;

    if (!select_dependency_profile(recipe, profile, true, error))
      return 2;

    BuildOptions options;
    options.target = target;
    options.profile = profile;

    if (profile == workflow_release_profile)
      options.configuration = "Release";

    if (build_project(project_directory, options, process_runner, output, error) != 0)
      return 2;

    if (!is_safe_path_component(recipe.name) || !is_safe_path_component(recipe.version))
    {
      error << "forge: project name and version must be safe path components\n";
      return 2;
    }

    auto build_directory = project_directory / ".forge" / "build";

    if (recipe.selected_target)
      build_directory /= *recipe.selected_target;

    auto built_artifact = build_directory / recipe.name;
    std::optional<std::filesystem::path> built_import_library;

#ifdef _WIN32
    if (recipe.type == "static_library")
      built_artifact += ".lib";
    else if (recipe.type == "dynamic_library")
    {
      built_artifact = build_directory / dynamic_library_filename(recipe.name);
      built_import_library = build_directory / import_library_filename(recipe.name);
    }
    else
      built_artifact += ".exe";
#else
    if (recipe.type == "static_library")
      built_artifact = build_directory / ("lib" + recipe.name + ".a");
    else if (recipe.type == "dynamic_library")
      built_artifact = build_directory / dynamic_library_filename(recipe.name);
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

    const auto force_target_qualified =
      recipe.type == "header_only"
      && (!recipe.selected_internal_dependencies.empty()
          || dependencies_require_target_qualified_box(
            project_directory,
            recipe,
            process_runner,
            error
          ));
      const auto archive_filename = box_filename(
        recipe,
        box_variant_suffix(recipe, profile),
        force_target_qualified
      );
    const auto box_name = std::filesystem::path { archive_filename }.stem().string();
    const auto boxes_directory = project_directory / ".forge" / "boxes";
    const auto staging_directory = boxes_directory / "staging" / box_name;
    const auto archive_path = boxes_directory / archive_filename;

    if (!prepare_empty_directory(staging_directory, error))
      return 2;

    std::vector<BoxArtifact> artifacts;
    std::vector<RuntimeAsset> runtime_assets;

    if (!recipe.runtime_files.empty()
        && !collect_runtime_assets(project_directory, recipe.runtime_files, runtime_assets, error))
    {
      return 2;
    }

    const auto stage_runtime_asset_artifacts =
      [&runtime_assets, &staging_directory, &artifacts, &error]()
      {
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
            return false;
          }
        }

        return true;
      };

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

      if (!stage_runtime_asset_artifacts())
        return 2;
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

      if (!stage_runtime_asset_artifacts())
        return 2;
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

      if (!stage_runtime_asset_artifacts())
        return 2;
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

      if (!stage_runtime_asset_artifacts())
        return 2;
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
            profile,
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
        return 2;
    }

    for (const auto& dependency : recipe.dependencies)
    {
      if (!dependency_matches_current_target(dependency))
        continue;

      const auto source = project_directory / ".forge" / "dependency-boxes" / (dependency.name + ".cbox");

      if (!stage_dependency(source, dependency.name))
        return 2;
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

    if (!write_box_manifest(
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
      archive_arguments.push_back("bin");
    else if (recipe.type == "static_library")
    {
      archive_arguments.push_back("include");
      archive_arguments.push_back("lib");
    }
    else if (recipe.type == "dynamic_library")
    {
      archive_arguments.push_back("include");
      if (built_import_library)
        archive_arguments.push_back("lib");
      archive_arguments.push_back("runtime");
    }
    else if (recipe.type == "imported_library")
    {
      archive_arguments.push_back("include");

      if (std::filesystem::is_directory(staging_directory / "lib"))
        archive_arguments.push_back("lib");

      if (std::filesystem::is_directory(staging_directory / "runtime"))
        archive_arguments.push_back("runtime");
    }
    else
      archive_arguments.push_back("include");

    if (std::filesystem::is_directory(staging_directory / "runtime-assets"))
      archive_arguments.push_back("runtime-assets");

    if (!dependencies.empty())
      archive_arguments.push_back("dependencies");

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

    const auto version =
      [](const BoxMetadata& metadata)
      {
        auto result = metadata.version;

        if (metadata.build_number)
          result += "+build." + std::to_string(*metadata.build_number);

        return result;
      };
    const auto list_directory =
      [&output, &error, &project_directory, &version](
        std::string_view heading,
        const std::filesystem::path& directory,
        bool& valid
      )
      {
        std::vector<std::filesystem::path> boxes;
        std::error_code filesystem_error;

        if (std::filesystem::is_directory(directory))
        {
          for (const auto& entry : std::filesystem::directory_iterator { directory, filesystem_error })
          {
            if (filesystem_error)
              break;

            if (entry.is_regular_file() && entry.path().extension() == ".cbox")
              boxes.push_back(entry.path().filename());
          }
        }

        std::ranges::sort(boxes);

        if (!boxes.empty())
        {
          output << heading << ":\n";

          for (const auto& box : boxes)
          {
            BoxMetadata metadata;

            if (!read_box_metadata(
              directory / box,
              project_directory,
              run_process,
              metadata,
              error
            ))
            {
              valid = false;
              return boxes.size();
            }

            output << "  " << box.string() << "  " << metadata.name
                   << ' ' << version(metadata);

            if (metadata.type == "header_only" && is_portable_header_only_filename(box))
              output << " [any]";
            else if (!metadata.os.empty() && !metadata.arch.empty())
              output << " [" << metadata.os << '-' << metadata.arch << ']';
            else
              output << " [portable]";

            if (metadata.components.empty())
              output << "  " << metadata.type;
            else
            {
              output << "  components:";

              for (const auto& component : metadata.components)
                output << ' ' << component.name << " (" << component.type << ')';
            }

            output << '\n';
          }
        }

        return boxes.size();
      };

    bool valid = true;
    const auto generated = list_directory(
      "Generated boxes",
      project_directory / ".forge" / "boxes",
      valid
    );
    const auto published = valid
      ? list_directory("Published boxes", project_directory / "boxes", valid)
      : 0;

    if (!valid)
      return 2;

    if (generated + published == 0)
      output << "No boxes found\n";

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
      return 2;

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

    output << "Box: " << resolved_box.filename().string() << '\n'
           << "Package: " << manifest.name << ' ' << manifest.version;

    if (manifest.build_number)
      output << "+build." << *manifest.build_number;

    output << '\n';

    if (manifest.type == "header_only" && is_portable_header_only_filename(resolved_box))
      output << "Target: any\n";
    else if (!manifest.os.empty() && !manifest.arch.empty())
      output << "Target: " << manifest.os << '-' << manifest.arch << '\n';
    else
      output << "Target: portable\n";

    if (manifest.components.empty())
      output << "Type: " << manifest.type << '\n';
    else
    {
      output << "Components:\n";

      for (const auto& component : manifest.components)
        output << "  " << component.name << " (" << component.type << ")\n";
    }

    output << "\nManifest:\n" << manifest_content;
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
      return 2;

    BoxMetadata metadata;

    if (!read_box_metadata(resolved_box, working_directory, process_runner, metadata, error))
      return 2;

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
      return 2;

    const auto boxes_directory = project_directory / "boxes";
    const auto published_box = boxes_directory / resolved_box.filename();
    const auto checksum_path = boxes_directory / (resolved_box.filename().string() + ".sha256");
    std::string source_checksum;

    if (!sha256_file(resolved_box, source_checksum, error))
      return 2;

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
        return 2;

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
      return 2;

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
      return false;

    std::string box_checksum;

    if (!sha256_file(resolved_box, box_checksum, error))
      return false;

    const auto validation_directory =
      box_metadata_cache_root(working_directory) / box_checksum;
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
        manifest.macos_system_include_directories,
        manifest.linux_system_include_directories,
        manifest.windows_system_include_directories,
        manifest.macos_system_library_directories,
        manifest.linux_system_library_directories,
        manifest.windows_system_library_directories,
        manifest.macos_frameworks,
        manifest.macos_libraries,
        manifest.linux_libraries,
        manifest.windows_libraries,
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
          || (child.type != "header_only"
              && (child.os != manifest.os || child.arch != manifest.arch)))
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
          || child.type != component.type
          || (child.type != "header_only"
              && (child.os != manifest.os || child.arch != manifest.arch)))
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
      return false;

    if (container_metadata != nullptr)
      *container_metadata = container;

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
          error << ' ' << candidate.name;
      }

      error << '\n';
      return false;
    }

    std::string checksum;

    if (!sha256_file(resolved_box, checksum, error))
      return false;

    const auto validation_directory =
      box_metadata_cache_root(working_directory) / checksum;
    component_box = validation_directory / component->path;
    return read_box_metadata(component_box, validation_directory, process_runner, metadata, error);
  }

} // namespace forge
