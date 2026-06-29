#pragma once

#include "box.h"
#include "recipe.h"

#include <filesystem>
#include <iosfwd>
#include <optional>
#include <string>
#include <vector>

namespace forge
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
    std::vector<BoxArtifact> artifacts;
    std::vector<BoxDependency> dependencies;
    std::vector<BoxComponent> components;
  };

  bool write_box_manifest(const std::filesystem::path& path,
                          const Recipe& recipe,
                          const std::optional<ToolchainIdentity>& toolchain,
                          const std::vector<BoxArtifact>& artifacts,
                          const std::vector<BoxDependency>& dependencies,
                          std::ostream& error);

  bool read_box_manifest(const std::filesystem::path& path,
                         BoxManifest& manifest,
                         std::string& content,
                         std::ostream& error);

} // namespace forge
