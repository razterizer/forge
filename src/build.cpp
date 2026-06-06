#include "build.h"

#include "box.h"
#include "process.h"
#include "recipe.h"
#include "sha256.h"

#include <algorithm>
#include <fstream>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <system_error>
#include <vector>

namespace forge
{
  namespace
  {

    struct ResolvedDependency
    {
      std::string name;
      std::filesystem::path root;
      std::optional<std::filesystem::path> library;
      std::optional<std::filesystem::path> runtime;
    };

    struct DependencyNode
    {
      std::filesystem::path directory;
      Recipe recipe;
      std::filesystem::path box;
      std::optional<BoxMetadata> box_metadata;
    };

    struct DependencySession
    {
      std::map<std::filesystem::path, DependencyNode> nodes;
      std::map<std::string, std::filesystem::path> names;
      std::set<std::filesystem::path> active_projects;
    };

    thread_local DependencySession* dependency_session = nullptr;

    class DependencySessionScope
    {
    public:
      DependencySessionScope()
      {
        if (dependency_session == nullptr)
        {
          dependency_session = &owned_session_;
          owns_session_ = true;
        }
      }

      ~DependencySessionScope()
      {
        if (owns_session_)
        {
          dependency_session = nullptr;
        }
      }

    private:
      DependencySession owned_session_;
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
        {
          dependency_session->active_projects.erase(project_);
        }
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
        {
          escaped += '\\';
        }

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

    std::filesystem::path shared_library_filename(std::string_view name)
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
#if defined(__x86_64__) || defined(_M_X64)
      return "x86_64";
#elif defined(__aarch64__) || defined(_M_ARM64)
      return "arm64";
#else
      return "unknown";
#endif
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
        {
          return false;
        }

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
      {
        return true;
      }

      const auto script = destination.parent_path() / "download.cmake";
      std::ofstream file { script };

      if (!file)
      {
        error << "forge: could not create the dependency download script\n";
        return false;
      }

      file
        << "file(DOWNLOAD \"${URL}\" \"${DESTINATION}.tmp\" STATUS status TLS_VERIFY ON)\n"
        << "list(GET status 0 code)\n"
        << "if(NOT code EQUAL 0)\n"
        << "  file(REMOVE \"${DESTINATION}.tmp\")\n"
        << "  message(FATAL_ERROR \"download failed: ${status}\")\n"
        << "endif()\n"
        << "file(REMOVE \"${DESTINATION}\")\n"
        << "file(RENAME \"${DESTINATION}.tmp\" \"${DESTINATION}\")\n";
      file.close();

      return process_runner(
        {
          "cmake",
          "-DURL=" + std::string { url },
          "-DDESTINATION=" + destination.generic_string(),
          "-P",
          script.string()
        },
        parent_directory,
        error
      ) == 0;
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
          || !is_github_package_version(dependency.version))
      {
        error << "forge: dependency '" << dependency.name
              << "' has an invalid GitHub repository or version\n";
        return false;
      }

      auto tag_version = dependency.version;
      const auto build_metadata = tag_version.find("+build.");

      if (build_metadata != std::string::npos)
      {
        tag_version.resize(build_metadata);
      }

      const auto asset = dependency.name
        + "-" + dependency.version
        + "-" + target_os()
        + "-" + target_arch()
        + ".cbox";
      const auto release_url =
        "https://github.com/" + dependency.github + "/releases/download/release-" + tag_version + "/";
      const auto checksum_url = release_url + asset + ".sha256";
      const auto cache_directory =
        parent_directory / ".forge" / "cache" / "github"
          / std::filesystem::path { dependency.github }
          / dependency.version
          / (target_os() + "-" + target_arch());
      const auto checksum_path = cache_directory / (asset + ".sha256");
      std::error_code filesystem_error;
      std::filesystem::create_directories(cache_directory, filesystem_error);

      if (filesystem_error)
      {
        error << "forge: could not create the GitHub dependency cache\n";
        return false;
      }

      output << "Resolving GitHub dependency " << dependency.name << '\n' << std::flush;

      if (!download_file(
        parent_directory,
        checksum_url,
        checksum_path,
        true,
        process_runner,
        error
      ))
      {
        error << "forge: could not download checksum for dependency '" << dependency.name << "'\n";
        return false;
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
      return true;
    }

    bool write_github_lock_header(const std::filesystem::path& project_directory,
                                  const Recipe& recipe,
                                  std::ostream& error)
    {
      const auto has_github_dependency = std::any_of(
        recipe.dependencies.begin(),
        recipe.dependencies.end(),
        [](const Dependency& dependency)
        {
          return !dependency.github.empty();
        }
      );

      if (!has_github_dependency)
      {
        return true;
      }

      std::ofstream lock { project_directory / "forge.lock.toml" };

      if (!lock)
      {
        error << "forge: could not write forge.lock.toml\n";
        return false;
      }

      lock << "format = 1\n";

      if (!lock)
      {
        error << "forge: could not write forge.lock.toml\n";
        return false;
      }

      return true;
    }

    bool append_github_lock(const std::filesystem::path& project_directory,
                            const Dependency& dependency,
                            std::ostream& error)
    {
      std::ofstream lock { project_directory / "forge.lock.toml", std::ios::app };

      if (!lock)
      {
        error << "forge: could not write forge.lock.toml\n";
        return false;
      }

      lock
        << "\n[[dependency]]\n"
        << "name = \"" << dependency.name << "\"\n"
        << "github = \"" << dependency.github << "\"\n"
        << "version = \"" << dependency.version << "\"\n"
        << "url = \"" << dependency.url << "\"\n"
        << "sha256 = \"" << dependency.sha256 << "\"\n";

      if (!lock)
      {
        error << "forge: could not write forge.lock.toml\n";
        return false;
      }

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

      for (std::size_t index = 0; index < recipe.public_headers.size(); ++index)
      {
        auto include_path = recipe.public_headers[index];
        include_path = include_path.lexically_relative("include");
        std::ofstream source { directory / ("header-" + std::to_string(index) + ".cpp") };

        if (!source)
        {
          error << "forge: could not create header validation source\n";
          return false;
        }

        source << "#include <" << include_path.generic_string() << ">\n";
      }

      return true;
    }

    bool write_generated_cmake(const std::filesystem::path& path,
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

      file
        << "cmake_minimum_required(VERSION 3.25)\n"
        << "project(forge_project LANGUAGES CXX)\n\n";

      if (recipe.type == "static_library")
      {
        file << "add_library(forge_project STATIC\n";
      }
      else if (recipe.type == "shared_library")
      {
        file << "add_library(forge_project SHARED\n";
      }
      else if (recipe.type == "header_only")
      {
        file << "add_library(forge_project OBJECT\n";

        for (std::size_t index = 0; index < recipe.public_headers.size(); ++index)
        {
          file << "  \"${CMAKE_CURRENT_SOURCE_DIR}/header-validation/header-"
               << index << ".cpp\"\n";
        }
      }
      else
      {
        file << "add_executable(forge_project\n";
      }

      for (const auto& source : recipe.sources)
      {
        file << "  \"${FORGE_PROJECT_ROOT}/" << escape_cmake(source.generic_string()) << "\"\n";
      }

      for (const auto& header : recipe.public_headers)
      {
        file << "  \"${FORGE_PROJECT_ROOT}/" << escape_cmake(header.generic_string()) << "\"\n";
      }

      file
        << ")\n"
        << "target_compile_features(forge_project PUBLIC cxx_std_" << recipe.cpp_standard << ")\n";

      if (!recipe.public_headers.empty())
      {
        file
          << "target_include_directories(forge_project PUBLIC \"${FORGE_PROJECT_ROOT}/include\")\n";
      }

      for (std::size_t index = 0; index < dependencies.size(); ++index)
      {
        const auto& dependency = dependencies[index];
        file
          << "target_include_directories(forge_project PRIVATE \""
          << escape_cmake((dependency.root / "include").string()) << "\")\n";

        if (dependency.library)
        {
          file
            << "add_library(forge_dependency_" << index << ' '
            << (dependency.runtime ? "SHARED" : "STATIC") << " IMPORTED)\n"
            << "set_target_properties(forge_dependency_" << index
            << " PROPERTIES IMPORTED_LOCATION \""
            << escape_cmake(dependency.library->string()) << "\")\n"
            << "target_link_libraries(forge_project PRIVATE forge_dependency_" << index << ")\n";
        }
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
#endif

#ifdef __APPLE__
      file
        << "set_target_properties(forge_project PROPERTIES "
        << "BUILD_WITH_INSTALL_RPATH TRUE "
        << "INSTALL_RPATH \""
        << (recipe.type == "shared_library" ? "@loader_path" : "@loader_path/runtime")
        << "\")\n";

      if (recipe.type == "shared_library")
      {
        file
          << "set_target_properties(forge_project PROPERTIES "
          << "BUILD_WITH_INSTALL_NAME_DIR TRUE INSTALL_NAME_DIR \"@rpath\")\n";
      }
#elif defined(__linux__)
      file
        << "set_target_properties(forge_project PROPERTIES "
        << "BUILD_WITH_INSTALL_RPATH TRUE INSTALL_RPATH \""
        << (recipe.type == "shared_library" ? "$ORIGIN" : "$ORIGIN/runtime")
        << "\")\n";
#endif

      if (!file)
      {
        error << "forge: could not write '" << path.string() << "'\n";
        return false;
      }

      return true;
    }

    std::filesystem::path find_dependency_box(const std::filesystem::path& dependency_directory,
                                              const Recipe& recipe,
                                              std::ostream& error)
    {
      const auto boxes_directory = dependency_directory / ".forge" / "boxes";
      const auto prefix = recipe.name + "-" + recipe.version;
      std::filesystem::path result;
      std::filesystem::file_time_type result_time;
      std::error_code filesystem_error;

      for (const auto& entry : std::filesystem::directory_iterator { boxes_directory, filesystem_error })
      {
        if (filesystem_error)
        {
          break;
        }

        const auto filename = entry.path().filename().string();

        if (!entry.is_regular_file()
            || entry.path().extension() != ".cbox"
            || !filename.starts_with(prefix)
            || (filename.size() > prefix.size()
                && filename[prefix.size()] != '-'
                && filename[prefix.size()] != '+'))
        {
          continue;
        }

        const auto modified = entry.last_write_time(filesystem_error);

        if (filesystem_error)
        {
          break;
        }

        if (result.empty() || modified > result_time)
        {
          result = entry.path();
          result_time = modified;
        }
      }

      if (filesystem_error || result.empty())
      {
        error << "forge: could not locate box for dependency '" << recipe.name << "'\n";
      }

      return result;
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
      auto resolved_dependency = dependency;

      if (!resolved_dependency.github.empty()
          && !resolve_github_dependency(
            parent_directory,
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

      if (!resolved_dependency.github.empty()
          && !append_github_lock(parent_directory, resolved_dependency, error))
      {
        return false;
      }

      const auto is_box =
        !resolved_dependency.box.empty()
        || !resolved_dependency.url.empty()
        || !resolved_dependency.github.empty();
      const auto dependency_location =
        !downloaded_box.empty()
          ? downloaded_box
          : (resolved_dependency.box.empty() ? resolved_dependency.path : resolved_dependency.box);
      const auto directory =
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

      const auto existing_name = dependency_session->names.find(dependency.name);

      if (existing_name != dependency_session->names.end() && existing_name->second != directory)
      {
        error << "forge: dependency name '" << dependency.name
              << "' refers to multiple project paths\n";
        return false;
      }

      auto existing_node = dependency_session->nodes.find(directory);

      if (existing_node == dependency_session->nodes.end())
      {
        Recipe dependency_recipe;
        std::optional<BoxMetadata> box_metadata;

        if (is_box)
        {
          BoxMetadata metadata;

          if (!read_box_metadata(directory, parent_directory, process_runner, metadata, error))
          {
            return false;
          }

          if (metadata.name != dependency.name)
          {
            error << "forge: dependency name '" << dependency.name
                  << "' does not match box name '" << metadata.name << "'\n";
            return false;
          }

          if (metadata.os != target_os() || metadata.arch != target_arch())
          {
            error << "forge: dependency '" << dependency.name
                  << "' box targets " << metadata.os << '-' << metadata.arch
                  << ", but this build targets " << target_os() << '-' << target_arch() << '\n';
            return false;
          }

          dependency_recipe.name = metadata.name;
          dependency_recipe.version = metadata.version;
          dependency_recipe.type = metadata.type;
          dependency_recipe.build_number = metadata.build_number;
          box_metadata = std::move(metadata);
        }
        else if (!read_recipe(directory / "forge.recipe.toml", dependency_recipe, error))
        {
          return false;
        }

        if (dependency_recipe.name != dependency.name)
        {
          error << "forge: dependency name '" << dependency.name << "' does not match recipe name '"
                << dependency_recipe.name << "'\n";
          return false;
        }

        if (dependency_recipe.type != "static_library"
            && dependency_recipe.type != "shared_library"
            && dependency_recipe.type != "header_only")
        {
          error << "forge: dependency '" << dependency.name
                << "' must be a static_library, shared_library, or header_only project\n";
          return false;
        }

        dependency_session->names.emplace(dependency.name, directory);
        existing_node = dependency_session->nodes.emplace(
          directory,
          DependencyNode
            {
              directory,
              std::move(dependency_recipe),
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

      node = &existing_node->second;
      return true;
    }

    bool ensure_dependency_box(DependencyNode& node,
                               const ProcessRunner& process_runner,
                               std::ostream& output,
                               std::ostream& error)
    {
      if (!node.box.empty())
      {
        return true;
      }

      output << "Resolving dependency " << node.recipe.name << '\n';

      if (create_box(node.directory, process_runner, output, error) != 0)
      {
        return false;
      }

      node.box = find_dependency_box(node.directory, node.recipe, error);
      return !node.box.empty();
    }

    bool collect_dependency(const std::filesystem::path& parent_directory,
                            const Dependency& dependency,
                            const ProcessRunner& process_runner,
                            std::set<std::filesystem::path>& collected,
                            std::vector<DependencyNode*>& ordered,
                            std::ostream& output,
                            std::ostream& error)
    {
      DependencyNode* node = nullptr;

      if (!read_dependency_node(parent_directory, dependency, process_runner, node, output, error)
          || !ensure_dependency_box(*node, process_runner, output, error))
      {
        return false;
      }

      if (!collected.insert(node->directory).second)
      {
        return true;
      }

      for (const auto& child : node->recipe.dependencies)
      {
        if (!collect_dependency(
          node->directory,
          child,
          process_runner,
          collected,
          ordered,
          output,
          error
        ))
        {
          return false;
        }
      }

      ordered.push_back(node);
      return true;
    }

    bool install_dependency(const std::filesystem::path& dependencies_directory,
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
      {
        return false;
      }

      std::filesystem::rename(extracted, destination, filesystem_error);

      if (filesystem_error)
      {
        error << "forge: could not install dependency '" << node.recipe.name << "'\n";
        return false;
      }

      resolved =
        {
          node.recipe.name,
          destination,
          std::nullopt,
          std::nullopt
        };

      if (node.recipe.type == "static_library")
      {
        if (node.box_metadata)
        {
          for (const auto& artifact : node.box_metadata->artifacts)
          {
            if (artifact.kind == "static_library")
            {
              resolved.library = destination / artifact.path;
            }
          }
        }
        else
        {
#ifdef _WIN32
          resolved.library = destination / "lib" / (node.recipe.name + ".lib");
#else
          resolved.library = destination / "lib" / ("lib" + node.recipe.name + ".a");
#endif
        }
      }
      else if (node.recipe.type == "shared_library")
      {
        if (node.box_metadata)
        {
          for (const auto& artifact : node.box_metadata->artifacts)
          {
            if (artifact.kind == "shared_library")
            {
              resolved.library = destination / artifact.path;
            }
          }
        }
        else
        {
          resolved.library = destination / "runtime" / shared_library_filename(node.recipe.name);
        }

        resolved.runtime = resolved.library;
      }

      if ((node.recipe.type == "static_library" || node.recipe.type == "shared_library")
          && !resolved.library)
      {
        error << "forge: dependency '" << node.recipe.name << "' box has no linkable library\n";
        return false;
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
        if (!dependency.runtime)
        {
          continue;
        }

        std::filesystem::create_directories(runtime_directory, filesystem_error);

        if (filesystem_error)
        {
          error << "forge: could not create runtime dependency directory\n";
          return false;
        }

        std::filesystem::copy_file(
          *dependency.runtime,
          runtime_directory / dependency.runtime->filename(),
          std::filesystem::copy_options::overwrite_existing,
          filesystem_error
        );

        if (filesystem_error)
        {
          error << "forge: could not stage runtime dependency '" << dependency.name << "'\n";
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
      std::set<std::string> direct_names;
      std::set<std::filesystem::path> collected;
      std::vector<DependencyNode*> ordered;

      if (!write_github_lock_header(project_directory, recipe, error))
      {
        return false;
      }

      for (const auto& dependency : recipe.dependencies)
      {
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
    return build_project(project_directory, run_process, output, error);
  }

  int build_project(const std::filesystem::path& project_directory,
                    const ProcessRunner& process_runner,
                    std::ostream& output,
                    std::ostream& error)
  {
    DependencySessionScope session_scope;
    std::error_code canonical_error;
    const auto canonical_project =
      std::filesystem::weakly_canonical(project_directory, canonical_error);

    if (canonical_error)
    {
      error << "forge: could not resolve project directory\n";
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
    {
      return 2;
    }

    if (recipe.type != "executable"
        && recipe.type != "static_library"
        && recipe.type != "shared_library"
        && recipe.type != "header_only")
    {
      error << "forge: unsupported project type '" << recipe.type << "'\n";
      return 2;
    }

#ifdef _WIN32
    if (recipe.type == "shared_library")
    {
      error << "forge: shared_library projects are not supported on Windows yet\n";
      return 2;
    }
#endif

    if (recipe.sources.empty() && recipe.type != "header_only")
    {
      error << "forge: recipe contains no source files\n";
      return 2;
    }

    for (const auto& source : recipe.sources)
    {
      if (source.is_absolute() || source.string().starts_with(".."))
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

    if ((recipe.type == "static_library"
         || recipe.type == "shared_library"
         || recipe.type == "header_only")
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
      if (header.is_absolute()
          || header.string().starts_with("..")
          || header.begin() == header.end()
          || header.begin()->string() != "include")
      {
        error << "forge: public header paths must stay under include/\n";
        return 2;
      }

      if (!std::filesystem::is_regular_file(project_directory / header))
      {
        error << "forge: public header '" << header.generic_string() << "' does not exist\n";
        return 2;
      }
    }

    const auto forge_directory = project_directory / ".forge";
    const auto generated_directory = forge_directory / "generated";
    const auto build_directory = forge_directory / "build";
    std::vector<ResolvedDependency> dependencies;

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

    if (!stage_runtime_dependencies(build_directory / "runtime", dependencies, error))
    {
      return 2;
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
      "-DCMAKE_BUILD_TYPE=Debug",
      "-DFORGE_PROJECT_ROOT=" + project_directory.generic_string()
    };

    if (process_runner(configure_arguments, project_directory, error) != 0)
    {
      error << "forge: CMake configuration failed\n";
      return 2;
    }

    output << "Building " << recipe.name << '\n' << std::flush;

    const std::vector<std::string> build_arguments {
      "cmake",
      "--build",
      build_directory.string()
    };

    if (process_runner(build_arguments, project_directory, error) != 0)
    {
      error << "forge: build failed\n";
      return 2;
    }

    if (recipe.type == "header_only")
    {
      output << "Validated " << recipe.public_headers.size() << " public header";

      if (recipe.public_headers.size() != 1)
      {
        output << 's';
      }

      output << '\n';
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
    else if (recipe.type == "shared_library")
    {
      artifact = build_directory / shared_library_filename(recipe.name);
    }

    output << "Built " << artifact.string() << '\n';
    return 0;
  }

} // namespace forge
