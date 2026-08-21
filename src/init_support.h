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
    // The CMake target selected from this project.  This differs from name
    // for projects that expose a target such as libdeflate_static.
    std::string cmake_target;
    std::string version;
    std::string type;
    bool python_extension = false;
    std::string python_extension_name;
    std::filesystem::path python_extension_output_directory;
    bool python_extension_with_soabi = false;
    bool has_cmake_subprojects = false;
    std::vector<std::filesystem::path> cmake_subproject_directories;
    std::vector<std::string> cmake_link_dependencies;
    int cpp_standard = 20;
    int c_standard = 0;
    std::vector<std::string> sources;
    std::vector<std::string> headers;
    std::vector<std::string> include_directories;
    std::vector<std::string> public_include_directories;
    std::vector<std::string> definitions;
    std::vector<std::string> public_definitions;
    std::vector<std::string> macos_frameworks;
    std::vector<std::string> macos_libraries;
    std::vector<std::string> macos_brew_packages;
    std::vector<std::string> linux_libraries;
    std::vector<std::string> linux_apt_packages;
    std::vector<std::string> windows_libraries;
    std::vector<RuntimeFile> runtime_files;
    std::vector<std::filesystem::path> references;
    std::map<std::string, BuildProfile> profiles;
    std::vector<std::string> unresolved_properties;
  };


  void sort_unique(BuildProfile& profile);

  std::optional<std::string> project_relative_path(
    const std::filesystem::path& project_directory,
    const std::filesystem::path& path);

} // namespace forge
