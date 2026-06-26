#include "target_support.h"

#include "box.h"
#include "recipe.h"

#include <algorithm>

namespace forge
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

  std::string current_target()
  {
    return target_os() + "-" + target_arch();
  }

  std::string target_os_from_target(std::string_view target)
  {
    const auto separator = target.find('-');
    return separator == std::string_view::npos
      ? std::string { target }
      : std::string { target.substr(0, separator) };
  }

  std::string target_arch_from_target(std::string_view target)
  {
    const auto separator = target.find('-');
    return separator == std::string_view::npos
      ? std::string { target }
      : std::string { target.substr(separator + 1) };
  }

  bool is_supported_dependency_target(std::string_view target)
  {
    return target == "linux-x86_64"
      || target == "linux-arm64"
      || target == "macos-x86_64"
      || target == "macos-arm64"
      || target == "windows-x86"
      || target == "windows-x86_64"
      || target == "windows-arm64";
  }

  bool dependency_matches_target(const Dependency& dependency, std::string_view target)
  {
    return dependency.targets.empty()
      || std::find(
        dependency.targets.begin(),
        dependency.targets.end(),
        target
      ) != dependency.targets.end();
  }

  bool dependency_matches_current_target(const Dependency& dependency)
  {
    return dependency_matches_target(dependency, current_target());
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

  std::string package_version(const Recipe& recipe)
  {
    auto version = recipe.version;

    if (recipe.build_number)
      version += "+build." + std::to_string(*recipe.build_number);

    return version;
  }

  std::string package_version(const BoxMetadata& metadata)
  {
    auto version = metadata.version;

    if (metadata.build_number)
      version += "+build." + std::to_string(*metadata.build_number);

    return version;
  }

} // namespace forge
