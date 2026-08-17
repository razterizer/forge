#include "build.h"

#include "box.h"
#include "file_support.h"
#include "fprocess.h"
#include "project_scan.h"
#include "recipe.h"
#include "run_cache.h"
#include "runtime_assets.h"
#include "sha256.h"
#include "target_support.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <chrono>
#include <fstream>
#include <functional>
#include <iostream>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

#ifdef __APPLE__
#include <unistd.h>
#endif

namespace forge
{
  namespace
  {

    struct ResolvedLibrary
    {
      std::filesystem::path path;
      std::optional<std::filesystem::path> runtime;
    };

    struct ResolvedDependency
    {
      std::string name;
      std::string type;
      std::filesystem::path root;
      std::optional<ToolchainIdentity> toolchain;
      bool has_static_library = false;
      std::vector<std::filesystem::path> include_directories;
      std::vector<std::filesystem::path> macos_system_include_directories;
      std::vector<std::filesystem::path> linux_system_include_directories;
      std::vector<std::filesystem::path> windows_system_include_directories;
      std::vector<std::filesystem::path> macos_system_library_directories;
      std::vector<std::filesystem::path> linux_system_library_directories;
      std::vector<std::filesystem::path> windows_system_library_directories;
      std::vector<std::string> macos_frameworks;
      std::vector<std::string> macos_libraries;
      std::vector<std::string> macos_brew_packages;
      std::vector<std::string> linux_libraries;
      std::vector<std::string> linux_apt_packages;
      std::vector<std::string> windows_libraries;
      std::vector<ResolvedLibrary> libraries;
      std::vector<std::filesystem::path> runtimes;
      std::vector<RuntimeAsset> runtime_assets;
    };

    struct DependencyNode
    {
      std::filesystem::path directory;
      Recipe recipe;
      std::optional<std::string> target;
      std::optional<std::string> profile;
      std::optional<std::string> system_profile;
      std::string configuration;
      std::filesystem::path box;
      std::optional<BoxMetadata> box_metadata;
    };

    struct LockedDependency
    {
      std::string name;
      std::string github;
      std::string package;
      std::string component;
      std::string variant;
      std::string version;
      std::string target;
      std::string url;
      std::string sha256;
    };

    struct DependencyLock
    {
      std::map<std::string, LockedDependency> entries;
      bool loaded = false;
      bool dirty = false;
    };

    std::set<std::string> selector_dependency_names(const Recipe& recipe)
    {
      std::set<std::string> names;

      for (const auto& rule : recipe.dependency_rules)
      {
        for (const auto& dependency : rule.dependencies)
          names.insert(dependency.name);
      }

      return names;
    }

    void erase_system_provider_values(std::vector<std::string>& values,
                                      std::initializer_list<std::string_view> names)
    {
      std::erase_if(values, [&names](const std::string& value)
      {
        return std::ranges::find(names, value) != names.end();
      });
    }

    void replace_system_provider_requirements(Recipe& recipe)
    {
      const auto erase = [&recipe](std::initializer_list<std::string_view> macos_libraries,
                                   std::initializer_list<std::string_view> macos_brew_packages,
                                   std::initializer_list<std::string_view> linux_libraries,
                                   std::initializer_list<std::string_view> linux_apt_packages,
                                   std::initializer_list<std::string_view> windows_libraries)
      {
        erase_system_provider_values(recipe.macos_libraries, macos_libraries);
        erase_system_provider_values(recipe.macos_brew_packages, macos_brew_packages);
        erase_system_provider_values(recipe.linux_libraries, linux_libraries);
        erase_system_provider_values(recipe.linux_apt_packages, linux_apt_packages);
        erase_system_provider_values(recipe.windows_libraries, windows_libraries);
      };

      for (const auto& dependency : recipe.dependencies)
      {
        for (const auto& capability : dependency.provides)
        {
          if (capability == "SDL2")
            erase({ "SDL2" }, { "sdl2-compat" }, { "SDL2" }, { "libsdl2-dev" }, { "SDL2" });
          else if (capability == "glfw" || capability == "glfw3")
            erase({ "glfw" }, { "glfw" }, { "glfw" }, { "libglfw3-dev" }, { "glfw3" });
          else if (capability == "EnTT" || capability == "entt")
            erase({}, { "entt" }, {}, { "libentt-dev" }, {});
          else if (capability == "glm")
            erase({}, { "glm" }, {}, { "libglm-dev" }, {});
          else if (capability == "yaml-cpp")
            erase({ "yaml-cpp" }, { "yaml-cpp" }, { "yaml-cpp" }, { "libyaml-cpp-dev" }, { "yaml-cpp" });
          else if (capability == "spdlog")
            erase(
              { "spdlog" },
              { "fmt", "spdlog" },
              { "spdlog" },
              { "libfmt-dev", "libspdlog-dev" },
              { "spdlog" }
            );
          else if (capability == "fmt")
            erase({}, { "fmt" }, {}, { "libfmt-dev" }, {});
          else if (capability == "nfd" || capability == "nativefiledialog-extended")
            erase({ "nfd" }, { "nativefiledialog-extended" }, { "nfd" }, {}, { "nfd" });
          else
            erase({ capability }, { capability }, { capability }, { capability }, { capability });
        }
      }
    }

    bool validate_selected_dependency_includes(
      const std::filesystem::path& project_directory,
      const Recipe& recipe,
      const RecipeSelection& selection,
      const std::set<std::string>& selectable_dependencies,
      std::ostream& error
    )
    {
      if (selectable_dependencies.empty())
        return true;

      std::vector<std::string> sources;
      std::vector<std::string> headers;

      for (const auto& source : recipe.sources)
        sources.push_back(source.generic_string());

      for (const auto& header : recipe.public_headers)
        headers.push_back(header.generic_string());

      const auto unresolved = unresolved_includes(project_directory, sources, headers);

      for (const auto& [include, source] : unresolved)
      {
        const auto separator = include.find_first_of("/\\");
        const auto dependency_name = include.substr(0, separator);

        if (!selectable_dependencies.contains(dependency_name))
          continue;

        const auto selected = std::ranges::find_if(
          recipe.dependencies,
          [&dependency_name](const Dependency& dependency)
          {
            return dependency.name == dependency_name;
          }
        );

        if (selected != recipe.dependencies.end())
          continue;

        error << "forge: selected dependency rules omit '" << dependency_name
              << "', required by include '" << include << "' in '" << source << "'\n"
              << "forge: no matching dependency for style '" << selection.style
              << "', platform '" << selection.platform
              << "', config '" << selection.configuration
              << "', profile '" << selection.profile << "'\n";
        return false;
      }

      return true;
    }

    bool is_binary_compatible_toolchain(const ToolchainIdentity& dependency,
                                        const ToolchainIdentity& project)
    {
      return dependency.compiler == project.compiler
        // A consumer may use a newer language mode than a compiled library.
        // The inverse is unsafe because it can expose public headers requiring
        // language features unavailable to the consumer.
        && dependency.cpp_standard <= project.cpp_standard
        && dependency.configuration == project.configuration
        && dependency.runtime == project.runtime;
    }

    void add_unique_path(std::vector<std::filesystem::path>& paths,
                         const std::filesystem::path& path)
    {
      if (std::find(paths.begin(), paths.end(), path) == paths.end())
        paths.push_back(path);
    }

    void add_dependency_include_directories(ResolvedDependency& dependency,
                                            const std::optional<BoxMetadata>& metadata)
    {
      add_unique_path(dependency.include_directories, dependency.root / "include");

      if (!metadata)
        return;

      for (const auto& artifact : metadata->artifacts)
      {
        if (artifact.kind != "public_header")
          continue;

        std::filesystem::path prefix;
        bool first = true;

        for (const auto& component : artifact.path)
        {
          prefix /= component;

          if (first)
          {
            first = false;
            continue;
          }

          if (component == std::filesystem::path { "include" })
            add_unique_path(dependency.include_directories, dependency.root / prefix);
        }
      }
    }

    // Dependency graph, lockfile, and cache state are owned by one resolver
    // for the duration of a root build.  Nested builds join that resolver so
    // they cannot accidentally choose a different cached dependency variant.
    struct DependencyResolver
    {
      std::map<std::filesystem::path, DependencyNode> nodes;
      std::map<std::string, std::filesystem::path> names;
      std::set<std::filesystem::path> active_projects;
      DependencyLock lock;
      std::filesystem::path root_project;
      BuildOptions options;
      bool profile_is_legacy = false;
      bool update_dependency_found = false;
    };

    thread_local DependencyResolver* dependency_session = nullptr;

    class DependencyResolverScope
    {
    public:
      DependencyResolverScope()
      {
        if (dependency_session == nullptr)
        {
          dependency_session = &owned_session_;
          owns_session_ = true;
        }
      }

      ~DependencyResolverScope()
      {
        if (owns_session_)
          dependency_session = nullptr;
      }

    private:
      DependencyResolver owned_session_;
      bool owns_session_ = false;
    };

    class ActiveProjectScope
    {
    public:
      explicit ActiveProjectScope(std::filesystem::path project)
        : project_ { std::move(project) }
      {
        inserted_ = dependency_session->active_projects.insert(project_).second;
      }

      ~ActiveProjectScope()
      {
        if (inserted_)
          dependency_session->active_projects.erase(project_);
      }

      bool inserted() const
      {
        return inserted_;
      }

    private:
      std::filesystem::path project_;
      bool inserted_ = false;
    };

    std::string escape_cmake(std::string_view value)
    {
      std::string escaped;

      for (const char character : value)
      {
        if (character == '\\')
        {
          escaped += '/';
          continue;
        }

        if (character == '"' || character == '$')
          escaped += '\\';

        escaped += character;
      }

      return escaped;
    }

    bool is_safe_dependency_name(std::string_view name)
    {
      return
        !name.empty()
        && name != "."
        && name != ".."
        && name.find('/') == std::string_view::npos
        && name.find('\\') == std::string_view::npos;
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

    std::string dependency_target()
    {
      if (dependency_session != nullptr && dependency_session->options.update_target)
        return *dependency_session->options.update_target;

      return current_target();
    }

    bool dependency_matches_target(const Dependency& dependency)
    {
      return forge::dependency_matches_target(dependency, dependency_target());
    }

    std::string dependency_target_os()
    {
      return target_os_from_target(dependency_target());
    }

    std::string dependency_target_arch()
    {
      return target_arch_from_target(dependency_target());
    }

    bool is_sha256(std::string_view value)
    {
      return value.size() == 64
        && value.find_first_not_of("0123456789abcdef") == std::string_view::npos;
    }

    bool is_safe_url_component(std::string_view value)
    {
      return !value.empty()
        && value != "."
        && value != ".."
        && value.find_first_not_of(
          "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789._+-"
        ) == std::string_view::npos;
    }

    bool is_github_repository(std::string_view value)
    {
      const auto separator = value.find('/');
      return separator != std::string_view::npos
        && separator == value.rfind('/')
        && is_safe_url_component(value.substr(0, separator))
        && is_safe_url_component(value.substr(separator + 1));
    }

    bool is_numeric_identifier(std::string_view value)
    {
      return !value.empty()
        && value.find_first_not_of("0123456789") == std::string_view::npos
        && (value.size() == 1 || value.front() != '0');
    }

    bool is_github_package_version(std::string_view value)
    {
      const auto build = value.find("+build.");

      if (build != std::string_view::npos)
      {
        if (!is_numeric_identifier(value.substr(build + std::string_view { "+build." }.size())))
          return false;

        value = value.substr(0, build);
      }

      const auto prerelease = value.find('-');
      const auto core = value.substr(0, prerelease);
      std::size_t offset = 0;

      for (int component = 0; component < 3; ++component)
      {
        const auto separator = core.find('.', offset);
        const auto end = component == 2 ? core.size() : separator;

        if ((component != 2 && separator == std::string_view::npos)
            || !is_numeric_identifier(core.substr(offset, end - offset)))
        {
          return false;
        }

        offset = end + 1;
      }

      return prerelease == std::string_view::npos
        || is_safe_url_component(value.substr(prerelease + 1));
    }

    bool download_file(const std::filesystem::path& parent_directory,
                       std::string_view url,
                       const std::filesystem::path& destination,
                       bool always_download,
                       const ProcessRunner& process_runner,
                       std::ostream& error)
    {
      if (!always_download && std::filesystem::is_regular_file(destination))
        return true;

      const auto script = destination.parent_path() / "download.cmake";
      auto status_path = destination;
      status_path += ".status";
      std::ofstream file { script };

      if (!file)
      {
        error << "forge: could not create the dependency download script\n";
        return false;
      }

      file
        << "file(DOWNLOAD \"${URL}\" \"${DESTINATION}.tmp\" STATUS status TLS_VERIFY ON)\n"
        << "list(GET status 0 code)\n"
        << "file(WRITE \"${STATUS_FILE}\" \"${code}\")\n"
        << "if(NOT code EQUAL 0)\n"
        << "  file(REMOVE \"${DESTINATION}.tmp\")\n"
        << "  return()\n"
        << "endif()\n"
        << "file(REMOVE \"${DESTINATION}\")\n"
        << "file(RENAME \"${DESTINATION}.tmp\" \"${DESTINATION}\")\n";
      file.close();

      std::error_code filesystem_error;
      std::filesystem::remove(status_path, filesystem_error);
      const auto result = process_runner(
        {
          "cmake",
          "-DURL=" + std::string { url },
          "-DDESTINATION=" + destination.generic_string(),
          "-DSTATUS_FILE=" + status_path.generic_string(),
          "-P",
          script.string()
        },
        parent_directory,
        error
      );

      if (result != 0)
        return false;

      std::ifstream status_file { status_path };
      int status = 0;

      if (status_file)
      {
        if (!(status_file >> status))
          return false;

        std::filesystem::remove(status_path, filesystem_error);
      }

      return status == 0;
    }

    bool download_dependency_box(const std::filesystem::path& parent_directory,
                                 const Dependency& dependency,
                                 const ProcessRunner& process_runner,
                                 std::filesystem::path& box,
                                 std::ostream& output,
                                 std::ostream& error)
    {
      if (!is_sha256(dependency.sha256))
      {
        error << "forge: dependency '" << dependency.name << "' has an invalid SHA-256 checksum\n";
        return false;
      }

      const auto cache_directory = parent_directory / ".forge" / "cache" / "downloads";
      box = cache_directory / (dependency.sha256 + ".cbox");
      std::error_code filesystem_error;
      std::filesystem::create_directories(cache_directory, filesystem_error);

      if (filesystem_error)
      {
        error << "forge: could not create the dependency download cache\n";
        return false;
      }

      if (!std::filesystem::is_regular_file(box))
      {
        output << "Downloading dependency " << dependency.name << '\n' << std::flush;

        if (!download_file(
          parent_directory,
          dependency.url,
          box,
          false,
          process_runner,
          error
        ))
        {
          error << "forge: could not download dependency '" << dependency.name << "'\n";
          return false;
        }
      }

      std::string checksum;

      if (!sha256_file(box, checksum, error) || checksum != dependency.sha256)
      {
        std::filesystem::remove(box, filesystem_error);
        error << "forge: downloaded dependency '" << dependency.name << "' checksum does not match\n";
        return false;
      }

      return true;
    }

    bool resolve_github_dependency(const std::filesystem::path& parent_directory,
                                   Dependency& dependency,
                                   const ProcessRunner& process_runner,
                                   std::ostream& output,
                                   std::ostream& error)
    {
      if (!is_github_repository(dependency.github)
          || !is_github_package_version(dependency.version)
          || (!dependency.package.empty() && !is_safe_dependency_name(dependency.package))
          || (!dependency.component.empty() && !is_safe_dependency_name(dependency.component))
          || (!dependency.variant.empty() && !is_safe_dependency_name(dependency.variant)))
      {
        error << "forge: dependency '" << dependency.name
              << "' has an invalid GitHub repository or version\n";
        return false;
      }

      const auto package = dependency.package.empty() ? dependency.name : dependency.package;
      const auto selected_target = dependency_target();
      const auto selected_os = dependency_target_os();
      const auto selected_arch = dependency_target_arch();
      const auto variant = dependency.variant.empty()
        ? std::string {}
        : "-" + dependency.variant;
      const auto compiled_asset = package
        + "-" + dependency.version
        + variant
        + "-" + selected_os
        + "-" + selected_arch
        + ".cbox";
      const auto header_only_asset = package + "-" + dependency.version + variant + "-ho.cbox";
      std::vector<std::string> candidate_assets { compiled_asset };

      if (selected_os == "linux")
      {
        candidate_assets.push_back(
          package
            + "-" + dependency.version
            + variant
            + "-" + selected_os
            + "-" + selected_arch
            + "-linux-modern.cbox"
        );
        candidate_assets.push_back(
          package
            + "-" + dependency.version
            + variant
            + "-" + selected_os
            + "-" + selected_arch
            + "-linux-legacy.cbox"
        );
      }

      candidate_assets.push_back(header_only_asset);
      std::vector<std::string> tag_versions;
      const auto build_metadata = dependency.version.find("+build.");

      if (build_metadata != std::string::npos)
      {
        const auto version = dependency.version.substr(0, build_metadata);
        const auto build = dependency.version.substr(
          build_metadata + std::string { "+build." }.size()
        );
        tag_versions.push_back(version + "." + build);
        tag_versions.push_back(dependency.version);
        tag_versions.push_back(version);
      }
      else
        tag_versions.push_back(dependency.version);

      const auto release_base =
        "https://github.com/" + dependency.github + "/releases/download/release-";
      const auto cache_directory =
        parent_directory / ".forge" / "cache" / "github"
          / std::filesystem::path { dependency.github }
          / dependency.version
          / selected_target;
      std::error_code filesystem_error;
      std::filesystem::create_directories(cache_directory, filesystem_error);

      if (filesystem_error)
      {
        error << "forge: could not create the GitHub dependency cache\n";
        return false;
      }

      output << "Resolving GitHub dependency " << dependency.name;

      if (!dependency.package.empty() || !dependency.component.empty())
      {
        output << " from package " << package;

        if (!dependency.component.empty())
          output << " component " << dependency.component;
      }

      if (!dependency.variant.empty())
        output << " variant " << dependency.variant;

      output << '\n' << std::flush;

      for (const auto& tag_version : tag_versions)
      {
        const auto release_url = release_base + tag_version + "/";

        for (const auto& asset : candidate_assets)
        {
          const auto checksum_path = cache_directory / (asset + ".sha256");
          std::ostringstream download_error;

          if (!download_file(
            parent_directory,
            release_url + asset + ".sha256",
            checksum_path,
            true,
            process_runner,
            download_error
          ))
          {
            continue;
          }

          std::ifstream checksum_file { checksum_path };
          std::string checksum;
          std::string filename;
          checksum_file >> checksum >> filename;

          if (!checksum_file || !is_sha256(checksum) || filename != asset)
          {
            error << "forge: dependency '" << dependency.name
                  << "' has an invalid GitHub release checksum file\n";
            return false;
          }

          dependency.url = release_url + asset;
          dependency.sha256 = std::move(checksum);
          dependency.resolved_target =
            asset == header_only_asset ? "any" : selected_target;
          return true;
        }
      }

      error << "forge: could not download checksum for dependency '" << dependency.name << "'\n";
      return false;
    }

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

    bool parse_lock_string(std::string_view value, std::string& result)
    {
      value = trim(value);

      if (value.size() < 2 || value.front() != '"' || value.back() != '"')
        return false;

      result = std::string { value.substr(1, value.size() - 2) };
      return result.find('"') == std::string::npos;
    }

    std::string lock_key(std::string_view name,
                         std::string_view variant,
                         std::string_view target)
    {
      return std::string { name }
        + '\n' + std::string { variant }
        + '\n' + std::string { target };
    }

    bool validate_imported_project(const std::filesystem::path& project_directory,
                                   const Recipe& recipe,
                                   std::ostream& output,
                                   std::ostream& error)
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
        return false;
      }

      if (profile->public_headers.empty()
          || (profile->static_libraries.empty()
              && profile->dynamic_libraries.empty()
              && profile->import_libraries.empty()))
      {
        error << "forge: imported_library profile requires headers and at least one library\n";
        return false;
      }

      if (profile->compiler.empty()
          || profile->compiler_version.empty()
          || profile->cpp_standard == 0
          || profile->configuration.empty()
          || profile->runtime.empty())
      {
        error << "forge: imported_library profile requires a complete toolchain identity\n";
        return false;
      }

      for (const auto& headers : profile->public_headers)
      {
        const auto path = project_directory / headers;

        if (!is_resolved_project_path(project_directory, headers)
            || std::filesystem::is_symlink(path)
            || (!std::filesystem::is_regular_file(path) && !std::filesystem::is_directory(path)))
        {
          error << "forge: imported header path '" << headers.generic_string()
                << "' must stay inside the project and contain regular files\n";
          return false;
        }
      }

      const std::array libraries {
        &profile->static_libraries,
        &profile->dynamic_libraries,
        &profile->import_libraries
      };

      for (const auto* group : libraries)
      {
        for (const auto& library : *group)
        {
          const auto path = project_directory / library;

          if (!is_resolved_project_path(project_directory, library)
              || std::filesystem::is_symlink(path)
              || !std::filesystem::is_regular_file(path))
          {
            error << "forge: imported library '" << library.generic_string()
                  << "' must be a project-relative file\n";
            return false;
          }
        }
      }

      output << "Validated imported library " << recipe.name << " for " << current_target() << '\n';
      return true;
    }

    bool load_lockfile(const std::filesystem::path& project_directory,
                       std::ostream& error)
    {
      if (dependency_session->lock.loaded)
        return true;

      dependency_session->lock.loaded = true;
      const auto path = project_directory / "forge.lock.toml";

      if (!std::filesystem::is_regular_file(path))
        return true;

      std::ifstream file { path };
      std::optional<LockedDependency> dependency;
      std::string line;
      std::size_t line_number = 0;
      int format = 0;

      const auto store_dependency =
        [&dependency, &error]() -> bool
        {
          if (!dependency)
            return true;

          if (dependency->name.empty()
              || dependency->github.empty()
              || (!dependency->package.empty() && !is_safe_dependency_name(dependency->package))
              || (!dependency->component.empty() && !is_safe_dependency_name(dependency->component))
              || (!dependency->variant.empty() && !is_safe_dependency_name(dependency->variant))
              || dependency->version.empty()
              || dependency->target.empty()
              || dependency->url.empty()
              || !is_sha256(dependency->sha256))
          {
            error << "forge: forge.lock.toml contains an incomplete dependency\n";
            return false;
          }

          if (dependency->package.empty())
            dependency->package = dependency->name;

          const auto key = lock_key(dependency->name, dependency->variant, dependency->target);

          if (!dependency_session->lock.entries.emplace(key, *dependency).second)
          {
            error << "forge: forge.lock.toml contains a duplicate dependency target\n";
            return false;
          }

          dependency.reset();
          return true;
        };

      while (std::getline(file, line))
      {
        ++line_number;
        const auto content = trim(line);

        if (content.empty() || content.front() == '#')
          continue;

        if (content == "[[dependency]]")
        {
          if (!store_dependency())
            return false;

          dependency.emplace();
          continue;
        }

        const auto equals = content.find('=');

        if (equals == std::string_view::npos)
        {
          error << "forge: invalid forge.lock.toml line " << line_number << '\n';
          return false;
        }

        const auto key = trim(content.substr(0, equals));
        const auto value = trim(content.substr(equals + 1));

        if (!dependency && key == "format" && (value == "1" || value == "2"))
        {
          format = value == "1" ? 1 : 2;
          continue;
        }

        std::string parsed;

        if (!dependency
            || !parse_lock_string(value, parsed)
            || (key != "name"
                && key != "github"
                && key != "package"
                && key != "component"
                && key != "variant"
                && key != "version"
                && key != "target"
                && key != "url"
                && key != "sha256"))
        {
          error << "forge: invalid forge.lock.toml line " << line_number << '\n';
          return false;
        }

        if (key == "name")
          dependency->name = std::move(parsed);
        else if (key == "github")
          dependency->github = std::move(parsed);
        else if (key == "package")
          dependency->package = std::move(parsed);
        else if (key == "component")
          dependency->component = std::move(parsed);
        else if (key == "variant")
          dependency->variant = std::move(parsed);
        else if (key == "version")
          dependency->version = std::move(parsed);
        else if (key == "target")
          dependency->target = std::move(parsed);
        else if (key == "url")
          dependency->url = std::move(parsed);
        else
          dependency->sha256 = std::move(parsed);
      }

      if (!store_dependency() || format == 0)
      {
        if (format == 0)
          error << "forge: forge.lock.toml has an unsupported or missing format\n";

        return false;
      }

      return true;
    }

    bool write_lockfile(std::ostream& error)
    {
      if (!dependency_session->lock.dirty)
        return true;

      const auto lock_path = dependency_session->root_project / "forge.lock.toml";
      const auto temporary_path = dependency_session->root_project / "forge.lock.toml.tmp";
      std::ofstream lock { temporary_path };

      if (!lock)
      {
        error << "forge: could not write forge.lock.toml\n";
        return false;
      }

      lock << "format = 2\n";

      for (const auto& entry : dependency_session->lock.entries)
      {
        const auto& dependency = entry.second;
        lock
          << "\n[[dependency]]\n"
          << "name = \"" << dependency.name << "\"\n"
          << "github = \"" << dependency.github << "\"\n"
          << "package = \"" << dependency.package << "\"\n";

        if (!dependency.component.empty())
          lock << "component = \"" << dependency.component << "\"\n";

        if (!dependency.variant.empty())
          lock << "variant = \"" << dependency.variant << "\"\n";

        lock
          << "version = \"" << dependency.version << "\"\n"
          << "target = \"" << dependency.target << "\"\n"
          << "url = \"" << dependency.url << "\"\n"
          << "sha256 = \"" << dependency.sha256 << "\"\n";
      }

      if (!lock)
      {
        error << "forge: could not write forge.lock.toml\n";
        return false;
      }

      lock.close();
      const auto backup_path = dependency_session->root_project / "forge.lock.toml.bak";
      std::error_code filesystem_error;
      std::filesystem::remove(backup_path, filesystem_error);
      filesystem_error.clear();

      if (std::filesystem::is_regular_file(lock_path))
      {
        std::filesystem::rename(lock_path, backup_path, filesystem_error);

        if (filesystem_error)
        {
          error << "forge: could not replace forge.lock.toml\n";
          return false;
        }
      }

      std::filesystem::rename(temporary_path, lock_path, filesystem_error);

      if (filesystem_error)
      {
        filesystem_error.clear();
        std::filesystem::rename(backup_path, lock_path, filesystem_error);
        error << "forge: could not replace forge.lock.toml\n";
        return false;
      }

      std::filesystem::remove(backup_path, filesystem_error);
      return true;
    }

    bool use_locked_github_dependency(Dependency& dependency,
                                      const ProcessRunner& process_runner,
                                      std::ostream& output,
                                      std::ostream& error)
    {
      const auto target = dependency_target();
      const auto update =
        dependency_session->options.update_dependencies
        && (!dependency_session->options.update_dependency
            || *dependency_session->options.update_dependency == dependency.name);

      if (update)
      {
        dependency_session->update_dependency_found = true;

        if (!resolve_github_dependency(
          dependency_session->root_project,
          dependency,
          process_runner,
          output,
          error
        ))
        {
          return false;
        }

        const auto resolved_target = dependency.resolved_target.empty()
          ? target
          : dependency.resolved_target;

        for (auto entry = dependency_session->lock.entries.begin();
             entry != dependency_session->lock.entries.end();)
        {
          if (entry->second.name == dependency.name
              && entry->second.variant == dependency.variant
              && (resolved_target == "any" || entry->second.target == "any"
                  || entry->second.target == resolved_target))
          {
            entry = dependency_session->lock.entries.erase(entry);
          }
          else
            ++entry;
        }

        dependency_session->lock.entries[
          lock_key(dependency.name, dependency.variant, resolved_target)
        ] =
          {
            dependency.name,
            dependency.github,
            dependency.package.empty() ? dependency.name : dependency.package,
            dependency.component,
            dependency.variant,
            dependency.version,
            resolved_target,
            dependency.url,
            dependency.sha256
          };
        dependency_session->lock.dirty = true;
        return true;
      }

      auto locked = dependency_session->lock.entries.find(
        lock_key(dependency.name, dependency.variant, target)
      );

      if (locked == dependency_session->lock.entries.end())
      {
        locked = dependency_session->lock.entries.find(
          lock_key(dependency.name, dependency.variant, "any")
        );
      }

      if (locked == dependency_session->lock.entries.end())
      {
        error << "forge: dependency '" << dependency.name << "' is not locked for "
              << target << "; run forge update " << dependency.name;

        if (dependency_session->options.profile)
          error << " --profile=" << *dependency_session->options.profile;
        else if (dependency_session->options.system_profile)
          error << " --sysprofile=" << *dependency_session->options.system_profile;

        error << '\n';
        return false;
      }

      if (locked->second.github != dependency.github
          || locked->second.package != (dependency.package.empty() ? dependency.name : dependency.package)
          || locked->second.component != dependency.component
          || locked->second.variant != dependency.variant
          || locked->second.version != dependency.version)
      {
        error << "forge: dependency '" << dependency.name
              << "' conflicts with forge.lock.toml; run forge update "
              << dependency.name;

        if (dependency_session->options.profile)
          error << " --profile=" << *dependency_session->options.profile;
        else if (dependency_session->options.system_profile)
          error << " --sysprofile=" << *dependency_session->options.system_profile;

        error << '\n';
        return false;
      }

      dependency.url = locked->second.url;
      dependency.sha256 = locked->second.sha256;
      output << "Using locked dependency " << dependency.name;

      if (!dependency.component.empty())
        output << " component " << dependency.component;

      if (!dependency.variant.empty())
        output << " variant " << dependency.variant;

      output << " for " << locked->second.target << '\n';
      return true;
    }

    bool write_header_validation_sources(const std::filesystem::path& directory,
                                         const Recipe& recipe,
                                         std::ostream& error)
    {
      std::error_code filesystem_error;
      std::filesystem::remove_all(directory, filesystem_error);
      filesystem_error.clear();
      std::filesystem::create_directories(directory, filesystem_error);

      if (filesystem_error)
      {
        error << "forge: could not create '" << directory.string() << "'\n";
        return false;
      }

      const auto& headers = recipe.header_validation_headers.empty()
        ? recipe.public_headers
        : recipe.header_validation_headers;

      for (std::size_t index = 0; index < headers.size(); ++index)
      {
        const auto include_path = public_header_include_path(
          headers[index],
          recipe.include_directories
        );

        if (!include_path)
        {
          error << "forge: public header '"
                << headers[index].generic_string()
                << "' is not below a public include root\n";
          return false;
        }

        const auto extension = recipe.cpp_standard == 0 && recipe.c_standard != 0
          ? ".c"
          : ".cpp";
        std::ofstream source { directory / ("header-" + std::to_string(index) + extension) };

        if (!source)
        {
          error << "forge: could not create header validation source\n";
          return false;
        }

        source << "#include <" << include_path->generic_string() << ">\n";
      }

      return true;
    }

    bool include_directory_publishes_headers(
      const std::vector<std::filesystem::path>& headers,
      const std::filesystem::path& include_directory)
    {
      return std::ranges::any_of(
        headers,
        [&include_directory](const std::filesystem::path& header)
        {
          const auto relative = header.lexically_relative(include_directory);
          return !relative.empty()
            && relative != "."
            && !relative.is_absolute()
            && relative.begin()->string() != "..";
        }
      );
    }

#ifdef __APPLE__
    bool install_missing_macos_brew_packages(const Recipe& recipe,
                                             const std::filesystem::path& project_directory,
                                             const ProcessRunner& process_runner,
                                             std::ostream& output,
                                             std::ostream& error)
    {
      if (recipe.macos_brew_packages.empty() || !::isatty(STDIN_FILENO))
        return true;

      std::vector<std::string> missing_packages;

      for (const auto& package : recipe.macos_brew_packages)
      {
        std::ostringstream ignored_output;

        if (process_runner({ "brew", "list", "--versions", package }, project_directory, ignored_output) != 0)
          missing_packages.push_back(package);
      }

      if (missing_packages.empty())
        return true;

      output << "forge: missing system librar";

      if (recipe.macos_libraries.size() == 1)
        output << "y " << recipe.macos_libraries.front();
      else if (!recipe.macos_libraries.empty())
      {
        output << "ies";

        for (const auto& library : recipe.macos_libraries)
          output << " " << library;
      }
      else
        output << "y provider package";

      output << "\nInstall provider with: brew install";

      for (const auto& package : missing_packages)
        output << ' ' << package;

      output << " ? [y/N] ";
      std::string response;

      if (!std::getline(std::cin, response)
          || (response != "y" && response != "Y" && response != "yes" && response != "YES"))
      {
        error << "forge: system provider installation declined\n";
        return false;
      }

      output << "Installing system provider...\n";
      std::vector<std::string> install_arguments { "brew", "install" };
      install_arguments.insert(install_arguments.end(), missing_packages.begin(), missing_packages.end());

      if (process_runner(install_arguments, project_directory, error) != 0)
      {
        error << "forge: failed to install system provider package\n";
        return false;
      }

      return true;
    }

    bool ensure_vcpkg_manifest_dependencies(const std::optional<std::filesystem::path>& manifest,
                                            const ProcessRunner& process_runner,
                                            std::ostream& output,
                                            std::ostream& error)
    {
      if (!manifest)
        return true;

#if defined(__aarch64__) || defined(__arm64__)
      constexpr auto triplet = "arm64-osx";
#else
      constexpr auto triplet = "x64-osx";
#endif
      const auto root = manifest->parent_path();
      const auto installed = root / "vcpkg_installed" / triplet;
      const auto vcpkg_root = root / ".forge" / "vcpkg";
      const auto vcpkg_root_argument = "--vcpkg-root=" + vcpkg_root.generic_string();
      const auto completion_marker = root / ".forge" / ("vcpkg-manifest-" + std::string { triplet } + ".sha256");

      if (!::isatty(STDIN_FILENO))
        return true;

      std::string manifest_checksum;

      if (!sha256_file(*manifest, manifest_checksum, error))
        return false;

      std::ifstream completion_file { completion_marker };
      std::string completed_checksum;

      if (std::getline(completion_file, completed_checksum)
          && completed_checksum == manifest_checksum
          && std::filesystem::is_directory(installed / "include"))
      {
        return true;
      }

      std::ostringstream ignored_output;

      if (!std::filesystem::is_regular_file(vcpkg_root / ".vcpkg-root"))
      {
        output << "forge: missing vcpkg ports tree\n"
               << "Install provider with: git clone https://github.com/microsoft/vcpkg .forge/vcpkg ? [y/N] ";
        std::string response;

        if (!std::getline(std::cin, response)
            || (response != "y" && response != "Y" && response != "yes" && response != "YES"))
        {
          error << "forge: vcpkg provider installation declined\n";
          return false;
        }

        output << "Installing vcpkg provider...\n";

        std::error_code filesystem_error;
        std::filesystem::create_directories(vcpkg_root.parent_path(), filesystem_error);

        if (filesystem_error)
        {
          error << "forge: could not create vcpkg provider directory\n";
          return false;
        }

        if (process_runner(
              { "git", "clone", "https://github.com/microsoft/vcpkg", vcpkg_root.generic_string() },
              root,
              error
            ) != 0)
        {
          error << "forge: failed to install vcpkg ports tree\n";
          return false;
        }
      }

      if (process_runner({ "vcpkg", "version", vcpkg_root_argument }, root, ignored_output) != 0)
      {
        output << "forge: missing vcpkg provider\n"
               << "Install provider with: brew install vcpkg ? [y/N] ";
        std::string response;

        if (!std::getline(std::cin, response)
            || (response != "y" && response != "Y" && response != "yes" && response != "YES"))
        {
          error << "forge: system provider installation declined\n";
          return false;
        }

        output << "Installing system provider...\n";

        if (process_runner({ "brew", "install", "vcpkg" }, root, error) != 0)
        {
          error << "forge: failed to install vcpkg\n";
          return false;
        }
      }

      if (process_runner({ "brew", "list", "--versions", "pkg-config" }, root, ignored_output) != 0)
      {
        output << "forge: missing vcpkg host tool pkg-config\n"
               << "Install provider with: brew install pkg-config ? [y/N] ";
        std::string response;

        if (!std::getline(std::cin, response)
            || (response != "y" && response != "Y" && response != "yes" && response != "YES"))
        {
          error << "forge: vcpkg host tool installation declined\n";
          return false;
        }

        output << "Installing vcpkg host tool...\n";

        if (process_runner({ "brew", "install", "pkg-config" }, root, error) != 0)
        {
          error << "forge: failed to install vcpkg host tool pkg-config\n";
          return false;
        }
      }

      output << "forge: missing vcpkg dependencies from " << manifest->string() << "\n"
             << "Install provider with: vcpkg install --triplet=" << triplet << " ? [y/N] ";
      std::string response;

      if (!std::getline(std::cin, response)
          || (response != "y" && response != "Y" && response != "yes" && response != "YES"))
      {
        error << "forge: system provider installation declined\n";
        return false;
      }

      output << "Installing vcpkg dependencies...\n";

      std::vector<std::string> install_arguments { "vcpkg", "install" };
      install_arguments.push_back(vcpkg_root_argument);
      install_arguments.push_back("--triplet=" + std::string { triplet });

      if (process_runner(install_arguments, root, error) != 0)
      {
        error << "forge: failed to install vcpkg dependencies\n";
        return false;
      }

      std::ofstream marker { completion_marker };

      if (!marker)
      {
        error << "forge: could not write vcpkg dependency completion marker\n";
        return false;
      }

      marker << manifest_checksum << '\n';

      return true;
    }
#endif

    std::optional<std::filesystem::path> find_vcpkg_manifest(
      const std::filesystem::path& project_directory
    )
    {
      std::error_code error;
      auto directory = std::filesystem::weakly_canonical(project_directory, error);

      if (error)
        return std::nullopt;

      while (true)
      {
        const auto manifest = directory / "vcpkg.json";

        if (std::filesystem::is_regular_file(manifest))
          return manifest;

        if (std::filesystem::exists(directory / ".git"))
          return std::nullopt;

        const auto parent = directory.parent_path();

        if (parent == directory)
          return std::nullopt;

        directory = parent;
      }
    }

    std::filesystem::path vcpkg_installation_directory(const std::filesystem::path& manifest)
    {
#if defined(__APPLE__) && (defined(__aarch64__) || defined(__arm64__))
      constexpr auto triplet = "arm64-osx";
#elif defined(__APPLE__)
      constexpr auto triplet = "x64-osx";
#elif defined(_WIN32)
      constexpr auto triplet = "x64-windows";
#elif defined(__aarch64__) || defined(__arm64__)
      constexpr auto triplet = "arm64-linux";
#else
      constexpr auto triplet = "x64-linux";
#endif
      return manifest.parent_path() / "vcpkg_installed" / triplet;
    }

    void write_system_links(std::ostream& file,
                            std::string_view target,
                            std::string_view visibility,
                            const std::vector<std::string>& macos_frameworks,
                            const std::vector<std::string>& macos_libraries,
                            const std::vector<std::string>& macos_brew_packages,
                            const std::vector<std::string>& linux_libraries,
                            const std::vector<std::string>& linux_apt_packages,
                            const std::vector<std::string>& windows_libraries)
    {
      const auto variable_component = [](std::string_view value)
      {
        std::string result;
        result.reserve(value.size());

        for (const unsigned char character : value)
          result += std::isalnum(character) ? static_cast<char>(character) : '_';

        return result;
      };
      const auto write_missing_system_library =
        [&file](std::string_view variable,
                std::string_view library,
                std::string_view package_manager,
                const std::vector<std::string>& packages)
        {
          file << "  if(NOT " << variable << ")\n"
               << "    message(FATAL_ERROR \"forge: missing system library '"
               << escape_cmake(library) << "'";

          if (!packages.empty())
          {
            file << "; install provider package"
                 << (packages.size() == 1 ? "" : "s") << " with: "
                 << package_manager << " install";

            for (const auto& package : packages)
              file << ' ' << escape_cmake(package);
          }

          file << "\")\n"
               << "  endif()\n";
        };

      if (!macos_frameworks.empty() || !macos_libraries.empty())
      {
        file << "if(APPLE)\n";

        for (const auto& framework : macos_frameworks)
        {
          const auto variable = "FORGE_" + std::string { target }
            + "_FRAMEWORK_" + variable_component(framework);
          file << "  find_library(" << variable << ' '
               << escape_cmake(framework) << ")\n";
          write_missing_system_library(
            variable,
            framework,
            "brew",
            macos_brew_packages
          );
          file
               << "  target_link_libraries(" << target << ' ' << visibility
               << " \"${" << variable << "}\")\n";
        }

        for (const auto& library : macos_libraries)
        {
          const auto variable = "FORGE_" + std::string { target }
            + "_MACOS_LIBRARY_" + variable_component(library);
          file << "  find_library(" << variable << ' '
               << escape_cmake(library) << ")\n";
          write_missing_system_library(
            variable,
            library,
            "brew",
            macos_brew_packages
          );
          file << "  target_link_libraries(" << target << ' ' << visibility
               << " \"${" << variable << "}\")\n";
        }

        file << "endif()\n";
      }

      if (!linux_libraries.empty())
      {
        file << "if(UNIX AND NOT APPLE)\n";

        for (const auto& library : linux_libraries)
        {
          const auto variable = "FORGE_" + std::string { target }
            + "_LINUX_LIBRARY_" + variable_component(library);
          file << "  find_library(" << variable << ' '
               << escape_cmake(library) << ")\n";
          write_missing_system_library(
            variable,
            library,
            "sudo apt",
            linux_apt_packages
          );
          file << "  target_link_libraries(" << target << ' ' << visibility
               << " \"${" << variable << "}\")\n";
        }

        file << "endif()\n";
      }

      if (!windows_libraries.empty())
      {
        file << "if(WIN32)\n";

        for (const auto& library : windows_libraries)
        {
          file << "  target_link_libraries(" << target << ' ' << visibility << ' '
               << escape_cmake(library) << ")\n";
        }

        file << "endif()\n";
      }
    }

    void write_system_include_directories(
      std::ostream& file,
      std::string_view target,
      std::string_view visibility,
      const std::vector<std::filesystem::path>& macos_include_directories,
      const std::vector<std::filesystem::path>& linux_include_directories,
      const std::vector<std::filesystem::path>& windows_include_directories)
    {
      const auto write_group =
        [&file, target, visibility](std::string_view condition,
                                    const std::vector<std::filesystem::path>& directories)
        {
          if (directories.empty())
            return;

          file << "if(" << condition << ")\n";

          for (const auto& directory : directories)
          {
            file << "  target_include_directories(" << target << " SYSTEM "
                 << visibility << " \"" << escape_cmake(directory.generic_string()) << "\")\n";
          }

          file << "endif()\n";
        };

      write_group("APPLE", macos_include_directories);
      write_group("UNIX AND NOT APPLE", linux_include_directories);
      write_group("WIN32", windows_include_directories);
    }

    void write_macos_brew_provider_paths(
      std::ostream& file,
      std::string_view target,
      std::string_view visibility,
      const std::vector<std::string>& packages)
    {
      if (packages.empty())
        return;

      file << "if(APPLE)\n";

      for (std::size_t index = 0; index < packages.size(); ++index)
      {
        const auto variable = "FORGE_" + std::string { target }
          + "_BREW_PREFIX_" + std::to_string(index);
        const auto result = variable + "_RESULT";
        file
          << "  execute_process(COMMAND brew --prefix \""
          << escape_cmake(packages[index])
          << "\" OUTPUT_VARIABLE " << variable
          << " RESULT_VARIABLE " << result
          << " OUTPUT_STRIP_TRAILING_WHITESPACE)\n"
          << "  if(" << result << " EQUAL 0)\n"
            << "    list(PREPEND CMAKE_PREFIX_PATH \"${" << variable << "}\")\n"
            << "    target_include_directories(" << target << " SYSTEM " << visibility
            << " \"${" << variable << "}/include\")\n"
            << "    target_link_directories(" << target << ' ' << visibility
            << " \"${" << variable << "}/lib\")\n";

        if (packages[index] == "sdl2-compat")
        {
          file
            << "    target_include_directories(" << target << " SYSTEM " << visibility
            << " \"${" << variable << "}/include/SDL2\")\n";
        }

        file << "  endif()\n";
      }

      file << "endif()\n";
    }

    void write_vcpkg_provider_paths(std::ostream& file,
                                    std::string_view target,
                                    std::string_view visibility,
                                    const std::optional<std::filesystem::path>& manifest)
    {
      if (!manifest)
        return;

      const auto installation = vcpkg_installation_directory(*manifest);

      file
        << "if(EXISTS \"" << escape_cmake((installation / "include").generic_string()) << "\")\n"
        << "  target_include_directories(" << target << " SYSTEM " << visibility << " \""
        << escape_cmake((installation / "include").generic_string()) << "\")\n"
        << "  target_link_directories(" << target << ' ' << visibility << " \""
        << escape_cmake((installation / "lib").generic_string()) << "\")\n"
        << "endif()\n";
    }

    void write_system_library_directories(
      std::ostream& file,
      std::string_view target,
      std::string_view visibility,
      const std::vector<std::filesystem::path>& macos_library_directories,
      const std::vector<std::filesystem::path>& linux_library_directories,
      const std::vector<std::filesystem::path>& windows_library_directories)
    {
      const auto write_group =
        [&file, target, visibility](std::string_view condition,
                                    const std::vector<std::filesystem::path>& directories)
        {
          if (directories.empty())
            return;

          file << "if(" << condition << ")\n";

          for (const auto& directory : directories)
          {
            file << "  target_link_directories(" << target << ' ' << visibility
                 << " \"" << escape_cmake(directory.generic_string()) << "\")\n";
          }

          file << "endif()\n";
        };

      write_group("APPLE", macos_library_directories);
      write_group("UNIX AND NOT APPLE", linux_library_directories);
      write_group("WIN32", windows_library_directories);
    }

    bool write_generated_cmake(const std::filesystem::path& project_directory,
                               const std::filesystem::path& path,
                               const Recipe& recipe,
                               const std::vector<ResolvedDependency>& dependencies,
                               std::ostream& error)
    {
      std::ofstream file { path };

      if (!file)
      {
        error << "forge: could not create '" << path.string() << "'\n";
        return false;
      }

      const auto uses_objective_cpp = [](const auto& sources)
      {
        return std::ranges::any_of(
          sources,
          [](const std::filesystem::path& source)
          {
            return source.extension() == ".mm";
          }
        );
      };
      const auto uses_c = [](const auto& sources)
      {
        return std::ranges::any_of(
          sources,
          [](const std::filesystem::path& source)
          {
            return source.extension() == ".c";
          }
        );
      };
      const auto uses_cpp = [](const auto& sources)
      {
        return std::ranges::any_of(
          sources,
          [](const std::filesystem::path& source)
          {
            const auto extension = source.extension();
            return extension == ".cpp" || extension == ".cc"
              || extension == ".cxx" || extension == ".mm";
          }
        );
      };
      auto has_objective_cpp_sources = uses_objective_cpp(recipe.sources);
      auto has_c_sources = uses_c(recipe.sources) || recipe.c_standard != 0;

      for (const auto& target : recipe.internal_targets)
      {
        has_objective_cpp_sources = has_objective_cpp_sources || uses_objective_cpp(target.sources);
        has_c_sources = has_c_sources || uses_c(target.sources) || target.c_standard != 0;
      }

      file
        << "cmake_minimum_required(VERSION 3.25)\n"
        << "project(forge_project LANGUAGES CXX"
        << (has_c_sources ? " C" : "")
        << (has_objective_cpp_sources ? " OBJCXX" : "")
        << ")\n\n"
        << "if(WIN32)\n"
        << "  add_compile_definitions(NOMINMAX)\n"
        << "endif()\n\n"
        << "include(CheckCXXSourceCompiles)\n"
        << "check_cxx_source_compiles(\"#include <string>\\n#ifndef _LIBCPP_VERSION\\n"
        << "#error\\n#endif\\nint main() {}\" FORGE_USES_LIBCPP)\n"
        << "check_cxx_source_compiles(\"#include <string>\\n"
        << "#if !defined(_GLIBCXX_USE_CXX11_ABI) || !_GLIBCXX_USE_CXX11_ABI\\n"
        << "#error\\n#endif\\nint main() {}\" FORGE_USES_GLIBCXX_CXX11_ABI)\n"
        << "set(FORGE_RUNTIME \"unknown\")\n"
        << "if(MSVC)\n"
        << "  set(FORGE_RUNTIME \"msvc-dynamic\")\n"
        << "elseif(FORGE_USES_LIBCPP)\n"
        << "  set(FORGE_RUNTIME \"libc++\")\n"
        << "elseif(FORGE_USES_GLIBCXX_CXX11_ABI)\n"
        << "  set(FORGE_RUNTIME \"libstdc++-cxx11\")\n"
        << "else()\n"
        << "  set(FORGE_RUNTIME \"libstdc++-legacy\")\n"
        << "endif()\n"
        << "file(WRITE \"${CMAKE_BINARY_DIR}/forge-toolchain.toml\" "
        << "\"compiler = \\\"${CMAKE_CXX_COMPILER_ID}\\\"\\n\" "
        << "\"compiler_version = \\\"${CMAKE_CXX_COMPILER_VERSION}\\\"\\n\" "
        << "\"cpp_std = " << recipe.cpp_standard << "\\n\" "
        << "\"configuration = \\\"${CMAKE_BUILD_TYPE}\\\"\\n\" "
        << "\"runtime = \\\"${FORGE_RUNTIME}\\\"\\n\")\n\n";

      if (recipe.python_extension)
        file << "find_package(Python3 REQUIRED COMPONENTS Development.Module)\n\n";

      std::map<std::string, std::string> internal_target_names;
      const auto vcpkg_manifest = find_vcpkg_manifest(project_directory);

      for (std::size_t index = 0; index < recipe.internal_targets.size(); ++index)
      {
        internal_target_names.emplace(
          recipe.internal_targets[index].name,
          "forge_internal_" + std::to_string(index)
        );
      }

      for (const auto& target : recipe.internal_targets)
      {
        const auto& target_name = internal_target_names.at(target.name);

        if (target.type == "header_only")
          file << "add_library(" << target_name << " INTERFACE)\n";
        else
        {
          file
            << "add_library(" << target_name << ' '
            << (target.type == "dynamic_library" ? "SHARED" : "STATIC") << "\n";

          for (const auto& source : target.sources)
          {
            file << "  \"${FORGE_PROJECT_ROOT}/" << escape_cmake(source.generic_string()) << "\"\n";
          }

          for (const auto& header : target.public_headers)
          {
            file << "  \"${FORGE_PROJECT_ROOT}/" << escape_cmake(header.generic_string()) << "\"\n";
          }

          file << ")\n";
        }

        const auto visibility = target.type == "header_only" ? "INTERFACE" : "PUBLIC";
        if (uses_cpp(target.sources))
        {
          file << "target_compile_features(" << target_name << ' ' << visibility
               << " cxx_std_" << target.cpp_standard << ")\n";
        }

        if (uses_c(target.sources)
            || (target.sources.empty() && target.c_standard != 0))
        {
          file << "target_compile_features(" << target_name << ' ' << visibility
               << " c_std_" << (target.c_standard == 0 ? 11 : target.c_standard) << ")\n";
        }

        file
          << "target_include_directories(" << target_name << ' ' << visibility
          << " \"${FORGE_PROJECT_ROOT}/include\")\n";

        for (const auto& include_directory : target.include_directories)
        {
          const auto include_visibility =
            target.type == "header_only"
              ? "INTERFACE"
              : include_directory_publishes_headers(target.public_headers, include_directory)
                ? "PUBLIC"
                : "PRIVATE";
          file
            << "target_include_directories(" << target_name << ' ' << include_visibility << ' '
            << "\"${FORGE_PROJECT_ROOT}/"
            << escape_cmake(include_directory.generic_string()) << "\")\n";
        }

        write_macos_brew_provider_paths(
          file,
          target_name,
          target.type == "header_only" ? "INTERFACE" : "PRIVATE",
          target.macos_brew_packages
        );
        write_vcpkg_provider_paths(
          file,
          target_name,
          target.type == "header_only" ? "INTERFACE" : "PRIVATE",
          vcpkg_manifest
        );

        write_system_include_directories(
          file,
          target_name,
          target.type == "header_only" ? "INTERFACE" : "PRIVATE",
          target.macos_system_include_directories,
          target.linux_system_include_directories,
          target.windows_system_include_directories
        );
        write_system_library_directories(
          file,
          target_name,
          target.type == "header_only" ? "INTERFACE" : "PRIVATE",
          target.macos_system_library_directories,
          target.linux_system_library_directories,
          target.windows_system_library_directories
        );

        for (const auto& definition : target.compile_definitions)
        {
          const auto definition_visibility =
            target.type == "header_only" ? "INTERFACE" : "PRIVATE";
          file
            << "target_compile_definitions(" << target_name << ' ' << definition_visibility << " \""
            << escape_cmake(definition) << "\")\n";
        }

        for (const auto& dependency : target.dependencies)
        {
          file << "target_link_libraries(" << target_name << ' ' << visibility
               << ' ' << internal_target_names.at(dependency) << ")\n";
        }

        write_system_links(
          file,
          target_name,
          visibility,
          target.macos_frameworks,
          target.macos_libraries,
          target.macos_brew_packages,
          target.linux_libraries,
          target.linux_apt_packages,
          target.windows_libraries
        );

#ifdef _WIN32
        if (target.type == "dynamic_library")
        {
          file
            << "set_target_properties(" << target_name
            << " PROPERTIES WINDOWS_EXPORT_ALL_SYMBOLS TRUE)\n";
        }
#endif

        file << '\n';
      }

      if (recipe.type == "static_library")
        file << "add_library(forge_project STATIC\n";
      else if (recipe.type == "dynamic_library")
        file << "add_library(forge_project SHARED\n";
      else if (recipe.type == "header_only")
      {
        file << "add_library(forge_project OBJECT\n";

        const auto& headers = recipe.header_validation_headers.empty()
          ? recipe.public_headers
          : recipe.header_validation_headers;

        for (std::size_t index = 0; index < headers.size(); ++index)
        {
          file << "  \"${CMAKE_CURRENT_SOURCE_DIR}/header-validation/header-"
               << index << (recipe.cpp_standard == 0 && recipe.c_standard != 0 ? ".c" : ".cpp")
               << "\"\n";
        }
      }
      else
        file << "add_executable(forge_project\n";

      for (const auto& source : recipe.sources)
      {
        file << "  \"${FORGE_PROJECT_ROOT}/" << escape_cmake(source.generic_string()) << "\"\n";
      }

      for (const auto& header : recipe.public_headers)
      {
        file << "  \"${FORGE_PROJECT_ROOT}/" << escape_cmake(header.generic_string()) << "\"\n";
      }

      file << ")\n";

      if (uses_cpp(recipe.sources) || (recipe.sources.empty() && recipe.cpp_standard != 0))
      {
        file << "target_compile_features(forge_project PUBLIC cxx_std_"
             << recipe.cpp_standard << ")\n";
      }

      if (uses_c(recipe.sources) || (recipe.sources.empty() && recipe.c_standard != 0))
      {
        file << "target_compile_features(forge_project PUBLIC c_std_"
             << (recipe.c_standard == 0 ? 11 : recipe.c_standard) << ")\n";
      }

      if (!recipe.public_headers.empty())
      {
        file
          << "target_include_directories(forge_project PUBLIC \"${FORGE_PROJECT_ROOT}/include\")\n";
      }

      for (const auto& include_directory : recipe.include_directories)
      {
        const auto include_visibility = include_directory_publishes_headers(
          recipe.public_headers,
          include_directory
        ) ? "PUBLIC" : "PRIVATE";
        file
          << "target_include_directories(forge_project " << include_visibility
          << " \"${FORGE_PROJECT_ROOT}/"
          << escape_cmake(include_directory.generic_string()) << "\")\n";
      }

      write_macos_brew_provider_paths(
        file,
        "forge_project",
        "PRIVATE",
        recipe.macos_brew_packages
      );
      write_vcpkg_provider_paths(file, "forge_project", "PRIVATE", vcpkg_manifest);

      write_system_include_directories(
        file,
        "forge_project",
        "PRIVATE",
        recipe.macos_system_include_directories,
        recipe.linux_system_include_directories,
        recipe.windows_system_include_directories
      );
      write_system_library_directories(
        file,
        "forge_project",
        "PRIVATE",
        recipe.macos_system_library_directories,
        recipe.linux_system_library_directories,
        recipe.windows_system_library_directories
      );

      for (const auto& definition : recipe.compile_definitions)
      {
        file
          << "target_compile_definitions(forge_project PRIVATE \""
          << escape_cmake(definition) << "\")\n";
      }

      if (recipe.python_extension)
        file << "target_link_libraries(forge_project PRIVATE Python3::Module)\n";

      for (const auto& dependency : recipe.selected_internal_dependencies)
      {
        file << "target_link_libraries(forge_project PRIVATE "
             << internal_target_names.at(dependency) << ")\n";
      }

      write_system_links(
        file,
        "forge_project",
        "PRIVATE",
        recipe.macos_frameworks,
        recipe.macos_libraries,
        recipe.macos_brew_packages,
        recipe.linux_libraries,
        recipe.linux_apt_packages,
        recipe.windows_libraries
      );

      for (std::size_t index = 0; index < dependencies.size(); ++index)
      {
        const auto& dependency = dependencies[index];

        for (const auto& include_directory : dependency.include_directories)
        {
          file
            << "target_include_directories(forge_project PRIVATE \""
            << escape_cmake(include_directory.string()) << "\")\n";
        }

        write_system_include_directories(
          file,
          "forge_project",
          "PRIVATE",
          dependency.macos_system_include_directories,
          dependency.linux_system_include_directories,
          dependency.windows_system_include_directories
        );
        write_system_library_directories(
          file,
          "forge_project",
          "PRIVATE",
          dependency.macos_system_library_directories,
          dependency.linux_system_library_directories,
          dependency.windows_system_library_directories
        );

        write_system_links(
          file,
          "forge_project",
          "PRIVATE",
          dependency.macos_frameworks,
          dependency.macos_libraries,
          dependency.macos_brew_packages,
          dependency.linux_libraries,
          dependency.linux_apt_packages,
          dependency.windows_libraries
        );

        for (std::size_t library_index = 0;
             library_index < dependency.libraries.size();
             ++library_index)
        {
          const auto& library = dependency.libraries[library_index];
          const auto target = "forge_dependency_" + std::to_string(index)
            + "_" + std::to_string(library_index);
          file
            << "add_library(" << target << ' '
            << (library.runtime ? "SHARED" : "STATIC") << " IMPORTED)\n"
            << "set_target_properties(" << target << " PROPERTIES ";

#ifdef _WIN32
          if (library.runtime)
          {
            file
              << "IMPORTED_IMPLIB \"" << escape_cmake(library.path.string()) << "\" "
              << "IMPORTED_LOCATION \"" << escape_cmake(library.runtime->string()) << "\")\n";
          }
          else
#endif
          {
            file
              << "IMPORTED_LOCATION \"" << escape_cmake(library.path.string()) << "\")\n";
          }

          file << "target_link_libraries(forge_project PRIVATE " << target << ")\n";
        }

#ifdef _WIN32
        if (recipe.type != "header_only")
        {
          for (const auto& runtime : dependency.runtimes)
          {
            file
              << "add_custom_command(TARGET forge_project POST_BUILD "
              << "COMMAND ${CMAKE_COMMAND} -E copy_if_different \""
              << escape_cmake(runtime.string())
              << "\" \"$<TARGET_FILE_DIR:forge_project>\")\n";
          }
        }
#endif
      }

      file
        << "set_target_properties(forge_project PROPERTIES OUTPUT_NAME \""
        << escape_cmake(recipe.name) << "\")\n";

#ifdef _WIN32
      if (recipe.type == "static_library")
      {
        file
          << "set_target_properties(forge_project PROPERTIES "
          << "PREFIX \"\" SUFFIX \".lib\")\n";
      }
      else if (recipe.type == "dynamic_library")
      {
        file
          << "set_target_properties(forge_project PROPERTIES "
          << "PREFIX \"\" SUFFIX \".dll\" "
          << "IMPORT_PREFIX \"\" IMPORT_SUFFIX \".lib\" "
          << "WINDOWS_EXPORT_ALL_SYMBOLS TRUE)\n";
      }
#endif

#ifdef __APPLE__
      file
        << "set_target_properties(forge_project PROPERTIES "
        << "BUILD_WITH_INSTALL_RPATH TRUE "
        << "INSTALL_RPATH \""
        << (recipe.type == "dynamic_library" ? "@loader_path" : "@loader_path;@loader_path/runtime")
        << "\")\n";

      if (recipe.type == "dynamic_library")
      {
        file
          << "set_target_properties(forge_project PROPERTIES "
          << "BUILD_WITH_INSTALL_NAME_DIR TRUE INSTALL_NAME_DIR \"@rpath\")\n";
      }
#elif defined(__linux__)
      file
        << "set_target_properties(forge_project PROPERTIES "
        << "BUILD_WITH_INSTALL_RPATH TRUE INSTALL_RPATH \""
        << (recipe.type == "dynamic_library" ? "$ORIGIN" : "$ORIGIN;$ORIGIN/runtime")
        << "\")\n";
#endif

      if (!file)
      {
        error << "forge: could not write '" << path.string() << "'\n";
        return false;
      }

      return true;
    }

    bool source_files_are_older(const std::filesystem::path& dependency_directory,
                                const Recipe& recipe,
                                const std::filesystem::file_time_type& box_time)
    {
      std::error_code filesystem_error;
      std::vector<std::filesystem::path> files { dependency_directory / "forge.recipe.toml" };

      const auto add_target_inputs = [&](const auto& target)
      {
        for (const auto& source : target.sources)
          files.push_back(dependency_directory / source);

        for (const auto& header : target.public_headers)
          files.push_back(dependency_directory / header);

        for (const auto& include_directory : target.include_directories)
        {
          std::error_code traversal_error;
          std::filesystem::recursive_directory_iterator iterator {
            dependency_directory / include_directory,
            std::filesystem::directory_options::skip_permission_denied,
            traversal_error
          };
          const std::filesystem::recursive_directory_iterator end;

          while (!traversal_error && iterator != end)
          {
            const auto& entry = *iterator;
            const auto name = entry.path().filename().string();

            if (entry.is_directory(traversal_error)
                && (name == ".git"
                    || name == ".forge"
                    || name == "build"
                    || name == "out"
                    || name.starts_with("cmake-build-")))
            {
              iterator.disable_recursion_pending();
            }
            else if (!traversal_error && entry.is_regular_file(traversal_error))
            {
              const auto extension = entry.path().extension().string();

              if (extension == ".h"
                  || extension == ".hpp"
                  || extension == ".hh"
                  || extension == ".hxx")
              {
                files.push_back(entry.path());
              }
            }

            iterator.increment(traversal_error);
          }
        }
      };

      add_target_inputs(recipe);

      // A selected target packages its named internal dependencies as nested
      // cboxes.  They are part of the package input too: otherwise a changed
      // vendored implementation could leave a stale parent cbox reusable.
      std::set<std::string> visited_internal_targets;
      const auto add_internal_inputs = [&](const auto& self, std::string_view name) -> void
      {
        if (!visited_internal_targets.insert(std::string { name }).second)
          return;

        const auto target = std::find_if(
          recipe.internal_targets.begin(),
          recipe.internal_targets.end(),
          [name](const RecipeTarget& candidate)
          {
            return candidate.name == name;
          }
        );

        if (target == recipe.internal_targets.end())
          return;

        add_target_inputs(*target);

        for (const auto& dependency : target->dependencies)
          self(self, dependency);
      };

      for (const auto& dependency : recipe.selected_internal_dependencies)
        add_internal_inputs(add_internal_inputs, dependency);

      for (const auto& runtime : recipe.runtime_files)
      {
        const auto path = dependency_directory / runtime.source;

        if (std::filesystem::is_directory(path))
        {
          for (const auto& entry : std::filesystem::recursive_directory_iterator { path })
          {
            if (entry.is_regular_file())
              files.push_back(entry.path());
          }
        }
        else
          files.push_back(path);
      }

      for (const auto& profile : recipe.imports)
      {
        const std::array imported_groups {
          &profile.public_headers,
          &profile.static_libraries,
          &profile.dynamic_libraries,
          &profile.import_libraries
        };

        for (const auto* group : imported_groups)
        {
          for (const auto& imported : *group)
          {
            const auto path = dependency_directory / imported;

            if (std::filesystem::is_directory(path))
            {
              for (const auto& entry : std::filesystem::recursive_directory_iterator { path })
              {
                if (entry.is_regular_file())
                  files.push_back(entry.path());
              }
            }
            else
              files.push_back(path);
          }
        }
      }

      for (const auto& file : files)
      {
        const auto modified = std::filesystem::last_write_time(file, filesystem_error);

        if (filesystem_error || modified > box_time)
          return false;
      }

      return true;
    }

    bool dependency_graph_matches(const Recipe& recipe,
                                  const BoxMetadata& metadata,
                                  std::ostream& error)
    {
      std::vector<std::reference_wrapper<const Dependency>> active_dependencies;

      for (const auto& dependency : recipe.dependencies)
      {
        if (dependency_matches_target(dependency))
          active_dependencies.emplace_back(dependency);
      }

      if (active_dependencies.size() + recipe.selected_internal_dependencies.size()
          != metadata.dependencies.size())
        return false;

      for (const auto& dependency : active_dependencies)
      {
        const auto child_path = dependency_session->names.find(dependency.get().name);

        if (child_path == dependency_session->names.end())
          return false;

        const auto child = dependency_session->nodes.find(child_path->second);

        if (child == dependency_session->nodes.end() || child->second.box.empty())
          return false;

        std::string checksum;

        if (!sha256_file(child->second.box, checksum, error))
          return false;

        const auto declared = std::find_if(
          metadata.dependencies.begin(),
          metadata.dependencies.end(),
          [&dependency](const BoxDependencyMetadata& candidate)
          {
            return candidate.name == dependency.get().name;
          }
        );

        if (declared == metadata.dependencies.end()
            || declared->version != package_version(child->second.recipe)
            || declared->type != child->second.recipe.type
            || declared->sha256 != checksum)
        {
          return false;
        }
      }

      for (const auto& dependency_name : recipe.selected_internal_dependencies)
      {
        const auto internal = std::find_if(
          recipe.internal_targets.begin(),
          recipe.internal_targets.end(),
          [&dependency_name](const RecipeTarget& candidate)
          {
            return candidate.name == dependency_name;
          }
        );
        const auto declared = std::find_if(
          metadata.dependencies.begin(),
          metadata.dependencies.end(),
          [&dependency_name](const BoxDependencyMetadata& candidate)
          {
            return candidate.name == dependency_name;
          }
        );

        if (internal == recipe.internal_targets.end()
            || declared == metadata.dependencies.end()
            || declared->version != package_version(recipe)
            || declared->type != internal->type)
        {
          return false;
        }
      }

      return true;
    }

    bool find_compatible_dependency_box(DependencyNode& node,
                                        const ProcessRunner& process_runner,
                                        bool report_reuse,
                                        std::ostream& output,
                                        std::ostream& error)
    {
      const auto boxes_directory = node.directory / ".forge" / "boxes";
      const auto prefix = node.recipe.name + "-" + node.recipe.version;
      std::error_code filesystem_error;

      if (!std::filesystem::is_directory(boxes_directory))
        return true;

      for (const auto& entry : std::filesystem::directory_iterator { boxes_directory, filesystem_error })
      {
        if (filesystem_error)
          break;

        const auto filename = entry.path().filename().string();
        const auto platform_independent_filename = prefix + ".cbox";

        if (!entry.is_regular_file()
            || entry.path().extension() != ".cbox"
            || !filename.starts_with(prefix)
            || (filename != platform_independent_filename
                && filename.size() > prefix.size()
                && filename[prefix.size()] != '-'
                && filename[prefix.size()] != '+'))
        {
          continue;
        }

        const auto modified = entry.last_write_time(filesystem_error);

        if (filesystem_error)
          break;

        BoxMetadata metadata;
        std::ostringstream metadata_error;

        if (source_files_are_older(node.directory, node.recipe, modified)
            && read_box_metadata(
              entry.path(),
              node.directory,
              process_runner,
              metadata,
              metadata_error
            )
            && metadata.name == node.recipe.name
            && metadata.version == node.recipe.version
            && metadata.build_number == node.recipe.build_number
            && metadata.type == node.recipe.type
            && ((metadata.type == "header_only" && !has_platform_specific_requirements(node.recipe))
                || (metadata.os == dependency_target_os()
                    && metadata.arch == dependency_target_arch()))
            && (metadata.type == "header_only"
                || (metadata.toolchain
                    && metadata.toolchain->configuration == node.configuration))
            && dependency_graph_matches(node.recipe, metadata, error))
        {
          node.box = entry.path();
          node.box_metadata = std::move(metadata);

          if (report_reuse)
            output << "Using cached dependency " << node.recipe.name << '\n';

          return true;
        }
      }

      if (filesystem_error)
      {
        error << "forge: could not inspect cached boxes for dependency '" << node.recipe.name << "'\n";
        return false;
      }

      return true;
    }

    bool checkout_git_dependency(const std::filesystem::path& parent_directory,
                                 const Dependency& dependency,
                                 const ProcessRunner& process_runner,
                                 std::filesystem::path& checkout,
                                 std::ostream& output,
                                 std::ostream& error)
    {
      checkout =
        dependency_session->root_project
        / ".forge"
        / "cache"
        / "git"
        / dependency.name
        / dependency.commit;
      std::ostringstream cache_error;
      const auto cached =
        std::filesystem::is_directory(checkout)
        && process_runner(
          { "git", "merge-base", "--is-ancestor", dependency.commit, "HEAD" },
          checkout,
          cache_error
        ) == 0
        && process_runner(
          { "git", "merge-base", "--is-ancestor", "HEAD", dependency.commit },
          checkout,
          cache_error
        ) == 0;

      if (cached)
      {
        output << "Using cached Git dependency " << dependency.name << '\n';
        return true;
      }

      std::error_code filesystem_error;
      std::filesystem::remove_all(checkout, filesystem_error);
      filesystem_error.clear();
      std::filesystem::create_directories(checkout, filesystem_error);

      if (filesystem_error)
      {
        error << "forge: could not create Git dependency cache\n";
        return false;
      }

      output << "Fetching Git dependency " << dependency.name
             << " at " << dependency.commit << '\n';
      std::error_code repository_error;
      const auto local_repository =
        std::filesystem::weakly_canonical(parent_directory / dependency.git, repository_error);
      const auto repository =
        !repository_error && std::filesystem::is_directory(local_repository)
          ? local_repository.string()
          : dependency.git;

      if (process_runner({ "git", "init", "--quiet" }, checkout, error) != 0
          || process_runner(
            { "git", "remote", "add", "origin", repository },
            checkout,
            error
          ) != 0
          || process_runner(
            { "git", "fetch", "--quiet", "--depth", "1", "origin", dependency.commit },
            checkout,
            error
          ) != 0
          || process_runner(
            { "git", "checkout", "--quiet", "--detach", dependency.commit },
            checkout,
            error
          ) != 0)
      {
        std::filesystem::remove_all(checkout, filesystem_error);
        error << "forge: could not checkout Git dependency '" << dependency.name
              << "' at commit '" << dependency.commit << "'\n";
        return false;
      }

      return true;
    }

    bool validate_box_dependency(const Dependency& dependency,
                                 const Dependency& resolved_dependency,
                                 const BoxMetadata& metadata,
                                 const BoxMetadata& container_metadata,
                                 std::ostream& error)
    {
      if (!resolved_dependency.github.empty())
      {
        const auto package = resolved_dependency.package.empty()
          ? resolved_dependency.name
          : resolved_dependency.package;

        if (container_metadata.name != package
            || package_version(container_metadata) != resolved_dependency.version)
        {
          error << "forge: dependency '" << dependency.name
                << "' requires package '" << package << "' version '"
                << resolved_dependency.version << "', but the GitHub box contains package '"
                << container_metadata.name << "' version '"
                << package_version(container_metadata) << "'\n";
          return false;
        }
      }

      if (metadata.name != dependency.name
          && (dependency.component.empty() || metadata.name != dependency.component))
      {
        error << "forge: dependency name '" << dependency.name
              << "' does not match box name '" << metadata.name
              << "' or the selected component\n";
        return false;
      }

      const auto contained_version = package_version(metadata);

      if (!dependency.version.empty()
          && contained_version != dependency.version
          && metadata.version != dependency.version)
      {
        error << "forge: dependency '" << dependency.name << "' requires version '"
              << dependency.version << "', but box contains version '" << contained_version << "'\n";
        return false;
      }

      if (!dependency.type.empty() && metadata.type != dependency.type)
      {
        error << "forge: dependency '" << dependency.name << "' declares type '"
              << dependency.type << "', but box contains type '" << metadata.type << "'\n";
        return false;
      }

      if (metadata.type != "header_only"
          && (metadata.os != dependency_target_os()
              || metadata.arch != dependency_target_arch()))
      {
        error << "forge: dependency '" << dependency.name
              << "' box targets " << metadata.os << '-' << metadata.arch
              << ", but this build targets "
              << dependency_target_os() << '-' << dependency_target_arch() << '\n';
        return false;
      }

      return true;
    }

    bool read_dependency_node(const std::filesystem::path& parent_directory,
                              const Dependency& dependency,
                              const ProcessRunner& process_runner,
                              DependencyNode*& node,
                              std::ostream& output,
                              std::ostream& error)
    {
      if (!is_safe_dependency_name(dependency.name))
      {
        error << "forge: dependency names must be safe path components\n";
        return false;
      }

      std::error_code filesystem_error;
      std::filesystem::path downloaded_box;
      std::filesystem::path git_checkout;
      auto resolved_dependency = dependency;
      std::optional<std::string> dependency_target;
      const auto has_local_github_fallback =
        !resolved_dependency.path.empty() && !resolved_dependency.github.empty();
      const auto updating_github_dependency =
        dependency_session->options.update_dependencies && !resolved_dependency.github.empty();

      if (has_local_github_fallback && !updating_github_dependency)
      {
        const auto local_path =
          std::filesystem::weakly_canonical(parent_directory / resolved_dependency.path, filesystem_error);

        if (!filesystem_error && std::filesystem::is_directory(local_path))
        {
          resolved_dependency.github.clear();
          resolved_dependency.package.clear();
          resolved_dependency.version.clear();
          resolved_dependency.component.clear();
          resolved_dependency.variant.clear();
        }

        filesystem_error.clear();
      }

      if (!resolved_dependency.github.empty()
          && !use_locked_github_dependency(
            resolved_dependency,
            process_runner,
            output,
            error
          ))
      {
        return false;
      }

      if (!resolved_dependency.url.empty()
          && !download_dependency_box(
            parent_directory,
            resolved_dependency,
            process_runner,
            downloaded_box,
            output,
            error
          ))
      {
        return false;
      }

      if (!resolved_dependency.git.empty())
      {
        if (!checkout_git_dependency(
          parent_directory,
          resolved_dependency,
          process_runner,
          git_checkout,
          output,
          error
        ))
        {
          return false;
        }
      }

      const auto is_box =
        !resolved_dependency.box.empty()
        || !resolved_dependency.url.empty()
        || !resolved_dependency.github.empty();
      const auto dependency_location =
        !git_checkout.empty()
          ? git_checkout
          : !downloaded_box.empty()
          ? downloaded_box
          : (resolved_dependency.box.empty() ? resolved_dependency.path : resolved_dependency.box);
      auto directory =
        std::filesystem::weakly_canonical(parent_directory / dependency_location, filesystem_error);

      if (filesystem_error
          || (!is_box && !std::filesystem::is_directory(directory))
          || (is_box && !std::filesystem::is_regular_file(directory)))
      {
        error << "forge: dependency '" << dependency.name << "' location does not exist\n";
        return false;
      }

      if (!is_box && dependency_session->active_projects.contains(directory))
      {
        error << "forge: dependency cycle detected at '" << dependency.name << "'\n";
        return false;
      }

      std::optional<BoxMetadata> box_metadata;

      if (is_box)
      {
        BoxMetadata metadata;
        BoxMetadata container_metadata;
        std::filesystem::path component_box;
        const auto component = dependency.component.empty()
          ? std::optional<std::string> { dependency.name }
          : std::optional<std::string> { dependency.component };

        if (!resolve_box_component(
          directory,
          parent_directory,
          component,
          process_runner,
          component_box,
          metadata,
          error,
          &container_metadata
        )
            || !validate_box_dependency(
              dependency,
              resolved_dependency,
              metadata,
              container_metadata,
              error
            ))
        {
          return false;
        }

        directory = component_box;
        box_metadata = std::move(metadata);
      }

      const auto existing_name = dependency_session->names.find(dependency.name);

      if (existing_name != dependency_session->names.end() && existing_name->second != directory)
      {
        const auto existing = dependency_session->nodes.find(existing_name->second);
        const auto existing_version = existing == dependency_session->nodes.end()
          ? std::string {}
          : package_version(existing->second.recipe);
        std::string existing_checksum;
        std::string requested_checksum;

        if (is_box
            && existing != dependency_session->nodes.end()
            && !existing->second.box.empty())
        {
          if (!sha256_file(existing->second.box, existing_checksum, error)
              || !sha256_file(directory, requested_checksum, error))
          {
            return false;
          }

          if (existing_checksum == requested_checksum)
          {
            node = &existing->second;
            return true;
          }
        }

        auto requested_version = dependency.version;

        if (requested_version.empty() && box_metadata)
          requested_version = package_version(*box_metadata);
        else if (requested_version.empty())
        {
          Recipe requested_recipe;

          if (read_recipe(directory / "forge.recipe.toml", requested_recipe, error))
            requested_version = package_version(requested_recipe);
          else
            return false;
        }

        if (!existing_version.empty() && !requested_version.empty())
        {
          if (existing_version != requested_version)
          {
            error << "forge: dependency conflict for '" << dependency.name
                  << "': exact versions '" << existing_version << "' and '"
                  << requested_version << "' cannot both be installed\n";
            return false;
          }

          error << "forge: dependency conflict for '" << dependency.name
                << "': exact version '" << existing_version
                << "' resolves to different packages";

          if (!existing_checksum.empty() && !requested_checksum.empty())
          {
            error << " (sha256 '" << existing_checksum << "' and '"
                  << requested_checksum << "')";
          }

          error << '\n';
          return false;
        }

        error << "forge: dependency name '" << dependency.name
              << "' refers to conflicting packages\n";
        return false;
      }

      auto existing_node = dependency_session->nodes.find(directory);

      if (existing_node == dependency_session->nodes.end())
      {
        Recipe dependency_recipe;

        if (is_box)
        {
          const auto& metadata = *box_metadata;

          dependency_recipe.name = dependency.name;
          dependency_recipe.version = metadata.version;
          dependency_recipe.type = metadata.type;
          dependency_recipe.build_number = metadata.build_number;

          for (const auto& child : metadata.dependencies)
          {
            dependency_recipe.dependencies.push_back(
              {
                child.name,
                {},
                child.path,
                {},
                child.sha256,
                {},
                {},
                child.version,
                {},
                {},
                child.type,
                {},
                {},
                {},
                {},
                {}
              }
            );
          }

        }
        else if (!read_recipe(directory / "forge.recipe.toml", dependency_recipe, error))
          return false;

        std::optional<std::string> selected_dependency_profile;
        std::optional<std::string> selected_dependency_system_profile;
        auto dependency_configuration = dependency_session->options.configuration;

        if (!is_box && dependency_recipe.type.empty() && !dependency_recipe.targets.empty())
        {
          const auto preferred = std::find_if(
            dependency_recipe.targets.begin(),
            dependency_recipe.targets.end(),
            [&dependency](const RecipeTarget& target)
            {
              return target.name == dependency.name
                && (target.type == "static_library"
                    || target.type == "dynamic_library"
                    || target.type == "imported_library"
                    || target.type == "header_only");
            }
          );
          if (preferred == dependency_recipe.targets.end())
          {
            error << "forge: dependency '" << dependency.name
                  << "' has no library target named '" << dependency.name << "'\n";
            return false;
          }

          if (!select_recipe_target(dependency_recipe, preferred->name, error))
            return false;

          dependency_target = preferred->name;
        }

        if (!is_box)
        {
          EffectiveBuildSelection effective_selection;

          if (!resolve_effective_build_selection(
            dependency_recipe,
            {
              .profile = dependency_session->options.profile,
              .system_profile = dependency_session->options.system_profile,
              .style = dependency_session->options.style,
              .platform = dependency_session->options.platform.value_or(current_target()),
              .selector_configuration = dependency_session->options.config,
              .build_configuration = dependency_session->options.configuration,
              .profile_resolution = dependency_session->profile_is_legacy
                ? ProfileResolution::inherited_legacy
                : ProfileResolution::selectors_only,
              .require_profile = false,
              .select_target = false
            },
            effective_selection,
            error
          ))
          {
            return false;
          }

          selected_dependency_profile = effective_selection.legacy_profile;

          if (!selected_dependency_profile && !effective_selection.selectors.profile.empty())
            selected_dependency_profile = effective_selection.selectors.profile;

          if (dependency_session->options.system_profile
              && (dependency_recipe.system_dependency_profiles.contains(
                    *dependency_session->options.system_profile
                  )
                  || dependency_recipe.system_build_profiles.contains(
                    *dependency_session->options.system_profile
                  )))
          {
            selected_dependency_system_profile = dependency_session->options.system_profile;
          }
          dependency_configuration = std::move(effective_selection.configuration);
        }

        if (selected_dependency_profile == workflow_release_profile)
          dependency_configuration = "Release";

        if (dependency_recipe.name != dependency.name)
        {
          error << "forge: dependency name '" << dependency.name << "' does not match recipe name '"
                << dependency_recipe.name << "'\n";
          return false;
        }

        if (dependency_recipe.type != "static_library"
            && dependency_recipe.type != "dynamic_library"
            && dependency_recipe.type != "imported_library"
            && dependency_recipe.type != "header_only")
        {
          error << "forge: dependency '" << dependency.name
                << "' must be a static_library, dynamic_library, imported_library, "
                << "or header_only project\n";
          return false;
        }

        dependency_session->names.emplace(dependency.name, directory);
        existing_node = dependency_session->nodes.emplace(
          directory,
          DependencyNode
            {
              directory,
              std::move(dependency_recipe),
              dependency_target,
              selected_dependency_profile,
              selected_dependency_system_profile,
              dependency_configuration,
              is_box ? directory : std::filesystem::path {},
              std::move(box_metadata)
            }
        ).first;
      }
      else if (existing_node->second.recipe.name != dependency.name)
      {
        error << "forge: dependency name '" << dependency.name << "' does not match recipe name '"
              << existing_node->second.recipe.name << "'\n";
        return false;
      }
      else if (!dependency.version.empty()
               && package_version(existing_node->second.recipe) != dependency.version
               && existing_node->second.recipe.version != dependency.version)
      {
        error << "forge: dependency '" << dependency.name << "' requires conflicting versions '"
              << package_version(existing_node->second.recipe) << "' and '"
              << dependency.version << "'\n";
        return false;
      }

      node = &existing_node->second;
      return true;
    }

    bool ensure_dependency_box(DependencyNode& node,
                               const ProcessRunner& process_runner,
                               std::ostream& output,
                               std::ostream& error)
    {
      if (!node.box.empty())
        return true;

      if (!find_compatible_dependency_box(node, process_runner, true, output, error))
        return false;

      if (!node.box.empty())
        return true;

      output << "Resolving dependency " << node.recipe.name << '\n';

      auto box_options = dependency_session->options;
      box_options.target = node.target;
      box_options.profile = node.profile;
      box_options.system_profile = node.system_profile;

      if (create_box(
        node.directory,
        node.target,
        box_options,
        process_runner,
        output,
        error
      ) != 0)
      {
        return false;
      }

      return find_compatible_dependency_box(node, process_runner, false, output, error)
        && !node.box.empty();
    }

    bool collect_dependency(const std::filesystem::path& parent_directory,
                            const Dependency& dependency,
                            const ProcessRunner& process_runner,
                            std::set<std::filesystem::path>& collected,
                            std::set<std::filesystem::path>& active,
                            std::vector<DependencyNode*>& ordered,
                            std::ostream& output,
                            std::ostream& error)
    {
      if (dependency_session->options.dependencies_only
          && dependency_session->options.update_dependency
          && *dependency_session->options.update_dependency != dependency.name
          && !dependency.github.empty())
      {
        return true;
      }

      DependencyNode* node = nullptr;

      if (!read_dependency_node(parent_directory, dependency, process_runner, node, output, error))
        return false;

      if (active.contains(node->directory))
      {
        error << "forge: dependency cycle detected at '" << dependency.name << "'\n";
        return false;
      }

      if (!collected.insert(node->directory).second)
        return true;

      active.insert(node->directory);

      for (const auto& child : node->recipe.dependencies)
      {
        if (!dependency_matches_target(child))
          continue;

        if (!collect_dependency(
          node->directory,
          child,
          process_runner,
          collected,
          active,
          ordered,
          output,
          error
        ))
        {
          active.erase(node->directory);
          return false;
        }
      }

      active.erase(node->directory);

      if (!ensure_dependency_box(*node, process_runner, output, error))
        return false;

      ordered.push_back(node);
      return true;
    }

    bool install_dependency(const std::filesystem::path& dependencies_directory,
                            const std::filesystem::path& dependency_boxes_directory,
                            const DependencyNode& node,
                            const ProcessRunner& process_runner,
                            ResolvedDependency& resolved,
                            std::ostream& output,
                            std::ostream& error)
    {
      std::error_code filesystem_error;
      std::filesystem::create_directories(dependencies_directory, filesystem_error);

      if (filesystem_error)
      {
        error << "forge: could not create dependency directory\n";
        return false;
      }

      const auto extracted = dependencies_directory / node.box.stem();
      const auto destination = dependencies_directory / node.recipe.name;
      std::filesystem::remove_all(extracted, filesystem_error);
      filesystem_error.clear();
      std::filesystem::remove_all(destination, filesystem_error);
      filesystem_error.clear();

      if (extract_box(node.box, dependencies_directory, process_runner, output, error) != 0)
        return false;

      std::filesystem::rename(extracted, destination, filesystem_error);

#ifdef _WIN32
      constexpr auto retry_timeout = std::chrono::seconds { 3 };
      auto retry_delay = std::chrono::milliseconds { 25 };
      const auto retry_deadline = std::chrono::steady_clock::now() + retry_timeout;

      while (filesystem_error && std::chrono::steady_clock::now() < retry_deadline)
      {
        std::this_thread::sleep_for(retry_delay);
        filesystem_error.clear();
        std::filesystem::rename(extracted, destination, filesystem_error);
        retry_delay = std::min(retry_delay * 2, std::chrono::milliseconds { 400 });
      }
#endif

      if (filesystem_error)
      {
        error << "forge: could not install dependency '" << node.recipe.name << "'\n"
              << "forge: could not rename '" << extracted.string() << "' to '"
              << destination.string() << "': " << filesystem_error.message() << '\n';
        return false;
      }

      std::filesystem::create_directories(dependency_boxes_directory, filesystem_error);

      if (filesystem_error)
      {
        error << "forge: could not create resolved dependency box directory\n";
        return false;
      }

      std::filesystem::copy_file(
        node.box,
        dependency_boxes_directory / (node.recipe.name + ".cbox"),
        std::filesystem::copy_options::overwrite_existing,
        filesystem_error
      );

      if (filesystem_error)
      {
        error << "forge: could not retain dependency box '" << node.recipe.name << "'\n";
        return false;
      }

      resolved =
        {
          node.recipe.name,
          node.recipe.type,
          destination,
          node.box_metadata ? node.box_metadata->toolchain : std::nullopt,
          false,
          {},
          node.box_metadata ? node.box_metadata->macos_system_include_directories
                            : std::vector<std::filesystem::path> {},
          node.box_metadata ? node.box_metadata->linux_system_include_directories
                            : std::vector<std::filesystem::path> {},
          node.box_metadata ? node.box_metadata->windows_system_include_directories
                            : std::vector<std::filesystem::path> {},
          node.box_metadata ? node.box_metadata->macos_system_library_directories
                            : std::vector<std::filesystem::path> {},
          node.box_metadata ? node.box_metadata->linux_system_library_directories
                            : std::vector<std::filesystem::path> {},
          node.box_metadata ? node.box_metadata->windows_system_library_directories
                            : std::vector<std::filesystem::path> {},
          node.box_metadata ? node.box_metadata->macos_frameworks : std::vector<std::string> {},
          node.box_metadata ? node.box_metadata->macos_libraries : std::vector<std::string> {},
          node.box_metadata ? node.box_metadata->macos_brew_packages : std::vector<std::string> {},
          node.box_metadata ? node.box_metadata->linux_libraries : std::vector<std::string> {},
          node.box_metadata ? node.box_metadata->linux_apt_packages : std::vector<std::string> {},
          node.box_metadata ? node.box_metadata->windows_libraries : std::vector<std::string> {},
          {},
          {},
          {}
        };

      add_dependency_include_directories(resolved, node.box_metadata);

      if (node.box_metadata)
      {
        for (const auto& artifact : node.box_metadata->artifacts)
        {
          if (artifact.kind == "runtime_asset")
          {
            auto destination_path = artifact.path;
            destination_path = destination_path.lexically_relative("runtime-assets");
            resolved.runtime_assets.push_back({ destination / artifact.path, destination_path });
          }
        }
      }

      if (node.recipe.type == "static_library")
      {
        if (node.box_metadata)
        {
          for (const auto& artifact : node.box_metadata->artifacts)
          {
            if (artifact.kind == "static_library")
            {
              resolved.libraries.push_back({ destination / artifact.path, std::nullopt });
              resolved.has_static_library = true;
            }
          }
        }
        else
        {
#ifdef _WIN32
          resolved.libraries.push_back(
            { destination / "lib" / (node.recipe.name + ".lib"), std::nullopt }
          );
#else
          resolved.libraries.push_back(
            { destination / "lib" / ("lib" + node.recipe.name + ".a"), std::nullopt }
          );
#endif
          resolved.has_static_library = true;
        }
      }
      else if (node.recipe.type == "dynamic_library")
      {
        if (node.box_metadata)
        {
          for (const auto& artifact : node.box_metadata->artifacts)
          {
            if (artifact.kind == "dynamic_library")
              resolved.runtimes.push_back(destination / artifact.path);
            else if (artifact.kind == "import_library")
            {
              resolved.libraries.push_back({ destination / artifact.path, std::nullopt });
            }
          }

#ifndef _WIN32
          for (const auto& runtime : resolved.runtimes)
          {
            resolved.libraries.push_back({ runtime, runtime });
          }
#endif
        }
        else
        {
#ifdef _WIN32
          resolved.libraries.push_back(
            { destination / "lib" / import_library_filename(node.recipe.name), std::nullopt }
          );
#else
          resolved.libraries.push_back(
            {
              destination / "runtime" / dynamic_library_filename(node.recipe.name),
              destination / "runtime" / dynamic_library_filename(node.recipe.name)
            }
          );
#endif
          resolved.runtimes.push_back(
            destination / "runtime" / dynamic_library_filename(node.recipe.name)
          );
        }

#ifdef _WIN32
        if (!resolved.libraries.empty() && !resolved.runtimes.empty())
          resolved.libraries.front().runtime = resolved.runtimes.front();
#endif
      }
      else if (node.recipe.type == "imported_library" && node.box_metadata)
      {
        for (const auto& artifact : node.box_metadata->artifacts)
        {
          const auto path = destination / artifact.path;

          if (artifact.kind == "static_library" || artifact.kind == "import_library")
          {
            resolved.libraries.push_back({ path, std::nullopt });

            if (artifact.kind == "static_library")
              resolved.has_static_library = true;
          }
          else if (artifact.kind == "dynamic_library")
          {
            resolved.runtimes.push_back(path);
#ifndef _WIN32
            resolved.libraries.push_back({ path, path });
#endif
          }
        }

#ifdef _WIN32
        for (auto& library : resolved.libraries)
        {
          const auto runtime = std::find_if(
            resolved.runtimes.begin(),
            resolved.runtimes.end(),
            [&library](const std::filesystem::path& candidate)
            {
              return candidate.stem() == library.path.stem();
            }
          );

          if (runtime != resolved.runtimes.end())
            library.runtime = *runtime;
        }
#endif
      }

      if (node.box_metadata)
      {
        // A component library is stored as an embedded cbox so a package can
        // be moved as one file.  A static archive still needs its component
        // archives on a consuming link line, however.  Extract and flatten
        // that link closure into this resolved dependency.
        const auto embedded_directory = destination / ".forge-embedded";
        std::set<std::filesystem::path> extracted_embedded_boxes;
        const auto add_embedded_dependencies =
          [&](const auto& self,
              const BoxMetadata& parent,
              const std::filesystem::path& parent_root) -> bool
        {
          for (const auto& dependency : parent.dependencies)
          {
            const auto child_box = parent_root / "dependencies" / dependency.path.filename();
            const auto child_root = embedded_directory / child_box.stem();
            BoxMetadata child_metadata;

            if (!read_box_metadata(
                  child_box,
                  parent_root,
                  process_runner,
                  child_metadata,
                  error
                )
                || (extracted_embedded_boxes.insert(child_root).second
                    && extract_box(child_box, embedded_directory, process_runner, output, error) != 0))
            {
              return false;
            }

            ResolvedDependency child_resolved;
            child_resolved.root = child_root;
            add_dependency_include_directories(
              child_resolved,
              std::optional<BoxMetadata> { child_metadata }
            );

            for (const auto& include_directory : child_resolved.include_directories)
              add_unique_path(resolved.include_directories, include_directory);

            for (const auto& artifact : child_metadata.artifacts)
            {
              const auto artifact_path = child_root / artifact.path;

              if (artifact.kind == "static_library" || artifact.kind == "import_library")
              {
                resolved.libraries.push_back({ artifact_path, std::nullopt });

                if (artifact.kind == "static_library")
                  resolved.has_static_library = true;
              }
              else if (artifact.kind == "dynamic_library")
              {
                resolved.runtimes.push_back(artifact_path);
#ifndef _WIN32
                resolved.libraries.push_back({ artifact_path, artifact_path });
#endif
              }
              else if (artifact.kind == "runtime_asset")
              {
                auto destination_path = artifact.path.lexically_relative("runtime-assets");
                resolved.runtime_assets.push_back({ artifact_path, destination_path });
              }
            }

            if (!self(self, child_metadata, child_root))
              return false;
          }

          return true;
        };

        if (!add_embedded_dependencies(add_embedded_dependencies, *node.box_metadata, destination))
          return false;
      }

      if ((node.recipe.type == "static_library"
           || node.recipe.type == "dynamic_library"
           || node.recipe.type == "imported_library")
          && resolved.libraries.empty())
      {
        error << "forge: dependency '" << node.recipe.name << "' box has no linkable library\n";
        return false;
      }

      if (node.recipe.type == "dynamic_library" && resolved.runtimes.empty())
      {
        error << "forge: dependency '" << node.recipe.name << "' box has no runtime library\n";
        return false;
      }

      return true;
    }

    bool validate_dependency_toolchains(const std::filesystem::path& build_directory,
                                        const std::vector<ResolvedDependency>& dependencies,
                                        std::ostream& error)
    {
      const auto has_compiled_dependency = std::any_of(
        dependencies.begin(),
        dependencies.end(),
        [](const ResolvedDependency& dependency)
        {
          return !dependency.libraries.empty();
        }
      );

      if (!has_compiled_dependency)
        return true;

      ToolchainIdentity project;
      std::ifstream file { build_directory / "forge-toolchain.toml" };
      std::string line;

      while (std::getline(file, line))
      {
        const auto content = trim(line);
        const auto equals = content.find('=');

        if (equals == std::string_view::npos)
          continue;

        const auto key = trim(content.substr(0, equals));
        std::string value;

        if (!parse_lock_string(trim(content.substr(equals + 1)), value))
        {
          if (key == "cpp_std")
          {
            const auto number = trim(content.substr(equals + 1));
            const auto parsed = std::from_chars(
              number.data(),
              number.data() + number.size(),
              project.cpp_standard
            );

            if (parsed.ec == std::errc {} && parsed.ptr == number.data() + number.size())
              continue;
          }

          error << "forge: could not read configured toolchain identity\n";
          return false;
        }

        if (key == "compiler")
          project.compiler = std::move(value);
        else if (key == "compiler_version")
          project.compiler_version = std::move(value);
        else if (key == "configuration")
          project.configuration = std::move(value);
        else if (key == "runtime")
          project.runtime = std::move(value);
      }

      if (project.compiler.empty()
          || project.compiler_version.empty()
          || project.cpp_standard == 0
          || project.configuration.empty()
          || project.runtime.empty())
      {
        error << "forge: configured build did not report a complete toolchain identity\n";
        return false;
      }

      for (const auto& dependency : dependencies)
      {
        if (dependency.libraries.empty())
          continue;

        if (dependency.type == "imported_library" && !dependency.has_static_library)
          continue;

        if (!dependency.toolchain)
        {
          error << "forge: compiled dependency '" << dependency.name
                << "' has no toolchain identity\n";
          return false;
        }

        const auto& candidate = *dependency.toolchain;

        if (!is_binary_compatible_toolchain(candidate, project))
        {
          error << "forge: dependency '" << dependency.name
                << "' toolchain is incompatible with the configured build\n";
          return false;
        }
      }

      return true;
    }

    bool stage_runtime_dependencies(const std::filesystem::path& runtime_directory,
                                    const std::vector<ResolvedDependency>& dependencies,
                                    std::ostream& error)
    {
      std::error_code filesystem_error;
      std::filesystem::remove_all(runtime_directory, filesystem_error);
      filesystem_error.clear();

      for (const auto& dependency : dependencies)
      {
        for (const auto& runtime : dependency.runtimes)
        {
          std::filesystem::create_directories(runtime_directory, filesystem_error);

          if (filesystem_error)
          {
            error << "forge: could not create runtime dependency directory\n";
            return false;
          }

          std::filesystem::copy_file(
            runtime,
            runtime_directory / runtime.filename(),
            std::filesystem::copy_options::overwrite_existing,
            filesystem_error
          );

          if (filesystem_error)
          {
            error << "forge: could not stage runtime dependency '" << dependency.name << "'\n";
            return false;
          }
        }
      }

      return true;
    }

    void collect_dependency_runtime_assets(const std::vector<ResolvedDependency>& dependencies,
                                           std::vector<RuntimeAsset>& runtime_assets)
    {
      for (const auto& dependency : dependencies)
      {
        runtime_assets.insert(
          runtime_assets.end(),
          dependency.runtime_assets.begin(),
          dependency.runtime_assets.end()
        );
      }
    }

    bool stage_internal_runtime_dependencies(
      const std::filesystem::path& build_directory,
      const std::vector<RecipeTarget>& targets,
      std::ostream& error)
    {
      std::error_code filesystem_error;

      for (std::size_t index = 0; index < targets.size(); ++index)
      {
        if (targets[index].type != "dynamic_library")
          continue;

        const auto runtime =
          build_directory / dynamic_library_filename("forge_internal_" + std::to_string(index));
        const auto runtime_directory = build_directory / "runtime";
        std::filesystem::create_directories(runtime_directory, filesystem_error);

        if (filesystem_error)
        {
          error << "forge: could not create runtime dependency directory\n";
          return false;
        }

        std::filesystem::copy_file(
          runtime,
          runtime_directory / runtime.filename(),
          std::filesystem::copy_options::overwrite_existing,
          filesystem_error
        );

        if (filesystem_error)
        {
          error << "forge: could not stage internal runtime dependency '"
                << targets[index].name << "'\n";
          return false;
        }
      }

      return true;
    }

    bool resolve_dependencies(const std::filesystem::path& project_directory,
                              const Recipe& recipe,
                              const ProcessRunner& process_runner,
                              std::vector<ResolvedDependency>& resolved,
                              std::ostream& output,
                              std::ostream& error)
    {
      const auto dependencies_directory = project_directory / ".forge" / "deps";
      const auto dependency_boxes_directory = project_directory / ".forge" / "dependency-boxes";
      std::set<std::string> direct_names;
      std::set<std::filesystem::path> collected;
      std::set<std::filesystem::path> active;
      std::vector<DependencyNode*> ordered;
      std::error_code filesystem_error;
      std::filesystem::remove_all(dependency_boxes_directory, filesystem_error);

      for (const auto& dependency : recipe.dependencies)
      {
        if (!dependency_matches_target(dependency))
          continue;

        if (!direct_names.insert(dependency.name).second)
        {
          error << "forge: duplicate dependency name '" << dependency.name << "'\n";
          return false;
        }

        if (!collect_dependency(
          project_directory,
          dependency,
          process_runner,
          collected,
          active,
          ordered,
          output,
          error
        ))
        {
          return false;
        }
      }

      std::reverse(ordered.begin(), ordered.end());

      for (const auto* node : ordered)
      {
        ResolvedDependency resolved_dependency;

        if (!install_dependency(
          dependencies_directory,
          dependency_boxes_directory,
          *node,
          process_runner,
          resolved_dependency,
          output,
          error
        ))
        {
          return false;
        }

        resolved.push_back(std::move(resolved_dependency));
      }

      return true;
    }

  } // namespace

  int build_project(const std::filesystem::path& project_directory,
                    std::ostream& output,
                    std::ostream& error)
  {
    return build_project(project_directory, BuildOptions {}, run_process, output, error);
  }

  int build_project(const std::filesystem::path& project_directory,
                    const BuildOptions& options,
                    std::ostream& output,
                    std::ostream& error)
  {
    return build_project(project_directory, options, run_process, output, error);
  }

  int build_project(const std::filesystem::path& project_directory,
                    const ProcessRunner& process_runner,
                    std::ostream& output,
                    std::ostream& error)
  {
    return build_project(project_directory, BuildOptions {}, process_runner, output, error);
  }

  int build_project(const std::filesystem::path& project_directory,
                    const BuildOptions& options,
                    const ProcessRunner& process_runner,
                    std::ostream& output,
                    std::ostream& error)
  {
    DependencyResolverScope session_scope;
    std::error_code canonical_error;
    const auto canonical_project =
      std::filesystem::weakly_canonical(project_directory, canonical_error);

    if (canonical_error)
    {
      error << "forge: could not resolve project directory\n";
      return 2;
    }

    const auto is_root_project = dependency_session->root_project.empty();

    if (is_root_project)
    {
      dependency_session->root_project = canonical_project;
      dependency_session->options = options;

      if (options.update_target && !is_supported_dependency_target(*options.update_target))
      {
        error << "forge: unsupported update target '" << *options.update_target << "'\n";
        return 2;
      }

      if (options.platform && !is_supported_dependency_target(*options.platform))
      {
        error << "forge: unsupported build platform '" << *options.platform << "'\n";
        return 2;
      }

      if (options.platform && *options.platform != current_target())
      {
        error << "forge: build platform '" << *options.platform
              << "' does not match the current host '" << current_target() << "'\n";
        return 2;
      }

      if (!load_lockfile(canonical_project, error))
        return 2;
    }

    ActiveProjectScope active_project { canonical_project };

    if (!active_project.inserted())
    {
      error << "forge: dependency cycle detected\n";
      return 2;
    }

    Recipe recipe;

    if (!read_recipe(project_directory / "forge.recipe.toml", recipe, error))
      return 2;

    auto requested_target = options.target
      ? options.target
      : is_root_project
        ? dependency_session->options.target
        : std::optional<std::string> {};

    if (is_root_project
        && dependency_session->options.dependencies_only
        && !requested_target
        && !recipe.targets.empty())
    {
      requested_target = recipe.targets.front().name;
    }

    const auto selectable_dependencies = selector_dependency_names(recipe);
    EffectiveBuildSelection effective_selection;

    if (!resolve_effective_build_selection(
      recipe,
      {
        .target = requested_target,
        .profile = dependency_session->options.profile,
        .system_profile = dependency_session->options.system_profile,
        .style = dependency_session->options.style,
        .platform = dependency_session->options.platform.value_or(current_target()),
        .selector_configuration = dependency_session->options.config,
        .build_configuration = dependency_session->options.configuration,
        .profile_resolution = is_root_project
          ? ProfileResolution::automatic
          : ProfileResolution::inherited_legacy,
        .require_profile = is_root_project
      },
      effective_selection,
      error
    ))
    {
      return 2;
    }

    const auto& legacy_profile = effective_selection.legacy_profile;
    const auto& selection = effective_selection.selectors;
    const auto& configuration = effective_selection.configuration;

    if (is_root_project)
    {
      dependency_session->profile_is_legacy = legacy_profile.has_value();

      if (!legacy_profile)
      {
        dependency_session->options.style = selection.style;
        dependency_session->options.platform = selection.platform;
        dependency_session->options.config = selection.configuration;
        dependency_session->options.profile = selection.profile.empty()
          ? std::optional<std::string> {}
          : std::optional<std::string> { selection.profile };
      }
    }

    if (!legacy_profile
        && !validate_selected_dependency_includes(
          project_directory,
          recipe,
          selection,
          selectable_dependencies,
          error
        ))
    {
      return 2;
    }

    replace_system_provider_requirements(recipe);

    if (is_root_project)
    {
      recipe.compile_definitions.insert(
        recipe.compile_definitions.end(),
        options.compile_definitions.begin(),
        options.compile_definitions.end()
      );
    }

#ifdef __APPLE__
    if (!install_missing_macos_brew_packages(
          recipe,
          project_directory,
          process_runner,
          output,
          error
        ))
    {
      return 2;
    }

    if (!ensure_vcpkg_manifest_dependencies(
          find_vcpkg_manifest(project_directory),
          process_runner,
          output,
          error
        ))
    {
      return 2;
    }
#endif

    for (const auto& target : recipe.internal_targets)
    {
      if (target.type != "static_library"
          && target.type != "dynamic_library"
          && target.type != "header_only")
      {
        error << "forge: unsupported internal dependency target type '" << target.type << "'\n";
        return 2;
      }

      if (target.sources.empty() && target.type != "header_only")
      {
        error << "forge: internal target '" << target.name << "' contains no source files\n";
        return 2;
      }

      if (target.public_headers.empty() && target.type != "dynamic_library")
      {
        error << "forge: internal library target '" << target.name << "' requires public headers\n";
        return 2;
      }

      if (target.type == "header_only" && !target.sources.empty())
      {
        error << "forge: internal header-only target '" << target.name
              << "' cannot declare source files\n";
        return 2;
      }

      for (const auto& source : target.sources)
      {
        if (!is_resolved_project_path(project_directory, source)
            || !std::filesystem::is_regular_file(project_directory / source))
        {
          error << "forge: internal target source '" << source.generic_string()
                << "' does not exist or leaves the project\n";
          return 2;
        }
      }

      for (const auto& header : target.public_headers)
      {
        if (!is_resolved_project_path(project_directory, header)
            || !public_header_include_path(header, target.include_directories)
            || !std::filesystem::is_regular_file(project_directory / header))
        {
          error << "forge: internal target public header '" << header.generic_string()
                << "' must be a file below include/ or a declared include directory\n";
          return 2;
        }
      }

      for (const auto& include_directory : target.include_directories)
      {
        if (!is_resolved_project_path(project_directory, include_directory)
            || !std::filesystem::is_directory(project_directory / include_directory))
        {
          error << "forge: internal target include directory '"
                << include_directory.generic_string()
                << "' does not exist or leaves the project\n";
          return 2;
        }
      }
    }

    if (recipe.type != "executable"
        && recipe.type != "static_library"
        && recipe.type != "dynamic_library"
        && recipe.type != "imported_library"
        && recipe.type != "header_only")
    {
      error << "forge: unsupported project type '" << recipe.type << "'\n";
      return 2;
    }

    // Imported libraries do not have sources to compile, but they still own a
    // dependency graph.  Resolve that graph below so it is recorded in the
    // package and can be reproduced by consumers.
    if (recipe.type == "imported_library"
        && !validate_imported_project(project_directory, recipe, output, error))
    {
      return 2;
    }

    if (recipe.sources.empty()
        && recipe.type != "header_only"
        && recipe.type != "imported_library")
    {
      error << "forge: recipe contains no source files\n";
      return 2;
    }

    for (const auto& source : recipe.sources)
    {
      if (!is_resolved_project_path(project_directory, source))
      {
        error << "forge: source paths must stay inside the project\n";
        return 2;
      }

      if (!std::filesystem::is_regular_file(project_directory / source))
      {
        error << "forge: source file '" << source.generic_string() << "' does not exist\n";
        return 2;
      }
    }

    if ((recipe.type == "static_library" || recipe.type == "header_only")
        && recipe.public_headers.empty())
    {
      error << "forge: library projects require public headers\n";
      return 2;
    }

    if (recipe.type == "header_only" && !recipe.sources.empty())
    {
      error << "forge: header-only projects cannot declare source files\n";
      return 2;
    }

    for (const auto& header : recipe.public_headers)
    {
      if (!is_resolved_project_path(project_directory, header)
          || !public_header_include_path(header, recipe.include_directories))
      {
        error << "forge: public header paths must stay under include/ or a declared include directory\n";
        return 2;
      }

      if (!std::filesystem::is_regular_file(project_directory / header))
      {
        error << "forge: public header '" << header.generic_string() << "' does not exist\n";
        return 2;
      }
    }

    for (const auto& header : recipe.header_validation_headers)
    {
      if (!is_resolved_project_path(project_directory, header)
          || !public_header_include_path(header, recipe.include_directories)
          || !std::filesystem::is_regular_file(project_directory / header))
      {
        error << "forge: header validation header '" << header.generic_string()
              << "' must be a public project header\n";
        return 2;
      }
    }

    for (const auto& include_directory : recipe.include_directories)
    {
      if (!is_resolved_project_path(project_directory, include_directory)
          || !std::filesystem::is_directory(project_directory / include_directory))
      {
        error << "forge: include directory '" << include_directory.generic_string()
              << "' does not exist or leaves the project\n";
        return 2;
      }
    }

    if (recipe.type == "imported_library" && !recipe.runtime_files.empty())
    {
      error << "forge: imported_library projects cannot declare runtime assets\n";
      return 2;
    }

    std::vector<RuntimeAsset> runtime_assets;

    if (!collect_runtime_assets(project_directory, recipe.runtime_files, runtime_assets, error))
      return 2;

    const auto forge_directory = project_directory / ".forge";
    auto generated_directory = forge_directory / "generated";
    auto build_directory = forge_directory / "build";

    if (recipe.selected_target)
    {
      generated_directory /= *recipe.selected_target;
      build_directory /= *recipe.selected_target;
    }

    const auto runtime_asset_manifest = build_directory / ".forge" / "runtime-assets.txt";
    std::vector<ResolvedDependency> dependencies;

    if (!clean_runtime_assets(build_directory, runtime_asset_manifest, error))
      return 2;

    if (!resolve_dependencies(
      project_directory,
      recipe,
      process_runner,
      dependencies,
      output,
      error
    ))
    {
      return 2;
    }

    if (is_root_project
        && dependency_session->options.update_dependency
        && !dependency_session->update_dependency_found)
    {
      error << "forge: GitHub dependency '" << *dependency_session->options.update_dependency
            << "' was not found\n";
      return 2;
    }

    if (is_root_project && dependency_session->options.dependencies_only)
    {
      if (!write_lockfile(error))
        return 2;

      output << "Updated locked dependencies for " << dependency_target() << '\n';
      return 0;
    }

    // Import profiles are prebuilt artifacts.  Their dependencies have now
    // been resolved and retained under .forge/dependency-boxes for box
    // creation, but there is no CMake project to configure or build.
    if (recipe.type == "imported_library")
      return 0;

    if (!stage_runtime_dependencies(build_directory / "runtime", dependencies, error))
      return 2;

    collect_dependency_runtime_assets(dependencies, runtime_assets);

    for (const auto& target : recipe.internal_targets)
    {
      std::vector<RuntimeAsset> target_runtime_assets;

      if (!collect_runtime_assets(
        project_directory,
        target.runtime_files,
        target_runtime_assets,
        error
      ))
      {
        return 2;
      }

      runtime_assets.insert(
        runtime_assets.end(),
        target_runtime_assets.begin(),
        target_runtime_assets.end()
      );
    }

    std::error_code filesystem_error;
    std::filesystem::create_directories(generated_directory, filesystem_error);

    if (filesystem_error)
    {
      error << "forge: could not create '" << generated_directory.string() << "'\n";
      return 2;
    }

    if ((recipe.type == "header_only"
         && !write_header_validation_sources(
           generated_directory / "header-validation",
           recipe,
           error
         ))
        || !write_generated_cmake(
          project_directory,
          generated_directory / "CMakeLists.txt",
          recipe,
          dependencies,
          error
        ))
    {
      return 2;
    }

    output << "Configuring " << recipe.name << '\n' << std::flush;

    const std::vector<std::string> configure_arguments {
      "cmake",
      "-S",
      generated_directory.string(),
      "-B",
      build_directory.string(),
      "-G",
      "Ninja",
      "-DCMAKE_BUILD_TYPE=" + configuration,
      "-DFORGE_PROJECT_ROOT=" + project_directory.generic_string()
    };

    if (process_runner(configure_arguments, project_directory, error) != 0)
    {
      error << "forge: CMake configuration failed\n";
      return 2;
    }

    if (!validate_dependency_toolchains(build_directory, dependencies, error))
      return 2;

    output << "Building " << recipe.name << '\n' << std::flush;

    const std::vector<std::string> build_arguments {
      "cmake",
      "--build",
      build_directory.string(),
      "--config",
      configuration
    };

    if (process_runner(build_arguments, project_directory, error) != 0)
    {
      error << "forge: build failed\n";
      return 2;
    }

    if (!stage_internal_runtime_dependencies(build_directory, recipe.internal_targets, error))
      return 2;

    if (!runtime_assets.empty()
        && !stage_runtime_assets(
          runtime_assets,
          build_directory,
          runtime_asset_manifest,
          error
        ))
    {
      return 2;
    }

    if (recipe.type == "header_only")
    {
      const auto header_validation_count = recipe.header_validation_headers.empty()
        ? recipe.public_headers.size()
        : recipe.header_validation_headers.size();
      output << "Validated " << header_validation_count << " public header";

      if (header_validation_count != 1)
        output << 's';

      output << '\n';

      if (is_root_project && !write_lockfile(error))
        return 2;

      return 0;
    }

    auto artifact = build_directory / recipe.name;

    if (recipe.type == "static_library")
    {
#ifdef _WIN32
      artifact += ".lib";
#else
      artifact = build_directory / ("lib" + recipe.name + ".a");
#endif
    }
    else if (recipe.type == "dynamic_library")
      artifact = build_directory / dynamic_library_filename(recipe.name);
#ifdef _WIN32
    else if (recipe.type == "executable")
      artifact += ".exe";
#endif

    output << "Built " << artifact.string() << '\n';

    auto run_selection = selection;

    if (run_selection.profile.empty() && legacy_profile)
      run_selection.profile = *legacy_profile;
    else if (run_selection.profile.empty() && dependency_session->options.system_profile)
      run_selection.profile = *dependency_session->options.system_profile;

    if (is_root_project
        && recipe.type == "executable"
        && std::filesystem::is_regular_file(artifact)
        && !cache_executable_run_variant(
          project_directory,
          build_directory,
          artifact,
          recipe,
          run_selection,
          configuration,
          runtime_assets,
          error
        ))
    {
      return 2;
    }

    if (is_root_project && !write_lockfile(error))
      return 2;

    return 0;
  }

} // namespace forge
