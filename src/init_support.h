#pragma once

#include "recipe.h"

#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace forge
{

  struct VisualStudioProject
  {
    std::filesystem::path path;
    std::string format = "Visual Studio";
    std::string name;
    std::string version;
    std::string type;
    int cpp_standard = 20;
    std::vector<std::string> sources;
    std::vector<std::string> headers;
    std::vector<std::string> include_directories;
    std::vector<std::string> definitions;
    std::vector<std::string> macos_frameworks;
    std::vector<std::string> macos_libraries;
    std::vector<std::string> linux_libraries;
    std::vector<std::string> windows_libraries;
    std::vector<std::filesystem::path> references;
    std::map<std::string, BuildProfile> profiles;
    std::vector<std::string> unresolved_properties;
  };


  void sort_unique(BuildProfile& profile);

  std::optional<std::string> project_relative_path(
    const std::filesystem::path& project_directory,
    const std::filesystem::path& path);

} // namespace forge
