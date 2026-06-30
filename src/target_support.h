#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace forge
{

  struct BoxMetadata;
  struct Dependency;
  struct Recipe;

  std::string target_os();
  std::string target_arch();
  std::string current_target();
  std::string target_os_from_target(std::string_view target);
  std::string target_arch_from_target(std::string_view target);
  std::vector<std::string> supported_dependency_targets();
  std::vector<std::string> release_dependency_targets();
  bool is_supported_dependency_target(std::string_view target);
  bool dependency_matches_target(const Dependency& dependency, std::string_view target);
  bool dependency_matches_current_target(const Dependency& dependency);
  bool has_platform_specific_requirements(const Recipe& recipe);
  std::string package_version(const Recipe& recipe);
  std::string package_version(const BoxMetadata& metadata);

} // namespace forge
