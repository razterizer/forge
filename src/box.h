#pragma once

#include "fprocess.h"

#include <filesystem>
#include <iosfwd>
#include <optional>
#include <string>
#include <vector>

namespace forge
{

  struct BoxArtifactMetadata
  {
    std::filesystem::path path;
    std::string kind;
  };

  struct BoxDependencyMetadata
  {
    std::string name;
    std::string version;
    std::string type;
    std::filesystem::path path;
    std::string sha256;
  };

  struct BoxComponentMetadata
  {
    std::string name;
    std::string type;
    std::filesystem::path path;
    std::string sha256;
  };

  struct ToolchainIdentity
  {
    std::string compiler;
    std::string compiler_version;
    int cpp_standard = 0;
    std::string configuration;
    std::string runtime;
  };

  struct BoxMetadata
  {
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
    std::vector<BoxArtifactMetadata> artifacts;
    std::vector<BoxDependencyMetadata> dependencies;
    std::vector<BoxComponentMetadata> components;
  };

  int create_box(const std::filesystem::path& project_directory,
                 std::ostream& output,
                 std::ostream& error);

  int create_box(const std::filesystem::path& project_directory,
                 const std::optional<std::string>& target,
                 std::ostream& output,
                 std::ostream& error);

  int create_box(const std::filesystem::path& project_directory,
                 const ProcessRunner& process_runner,
                 std::ostream& output,
                 std::ostream& error);

  int create_box(const std::filesystem::path& project_directory,
                 const std::optional<std::string>& target,
                 const ProcessRunner& process_runner,
                 std::ostream& output,
                 std::ostream& error);

  int create_box(const std::filesystem::path& project_directory,
                 const std::optional<std::string>& target,
                 const std::optional<std::string>& profile,
                 const ProcessRunner& process_runner,
                 std::ostream& output,
                 std::ostream& error);

  int list_boxes(const std::filesystem::path& project_directory,
                 bool show_platforms,
                 std::ostream& output,
                 std::ostream& error);

  int inspect_box(const std::filesystem::path& box_path,
                  const std::filesystem::path& working_directory,
                  std::ostream& output,
                  std::ostream& error);

  int inspect_box(const std::filesystem::path& box_path,
                  const std::filesystem::path& working_directory,
                  const ProcessRunner& process_runner,
                  std::ostream& output,
                  std::ostream& error);

  int verify_box(const std::filesystem::path& box_path,
                 const std::filesystem::path& working_directory,
                 std::ostream& output,
                 std::ostream& error);

  int verify_box(const std::filesystem::path& box_path,
                 const std::filesystem::path& working_directory,
                 const ProcessRunner& process_runner,
                 std::ostream& output,
                 std::ostream& error);

  int publish_box(const std::filesystem::path& box_path,
                  const std::filesystem::path& project_directory,
                  std::ostream& output,
                  std::ostream& error);

  int publish_box(const std::filesystem::path& box_path,
                  const std::filesystem::path& project_directory,
                  const ProcessRunner& process_runner,
                  std::ostream& output,
                  std::ostream& error);

  int extract_box(const std::filesystem::path& box_path,
                  const std::filesystem::path& working_directory,
                  std::ostream& output,
                  std::ostream& error);

  int extract_box(const std::filesystem::path& box_path,
                  const std::filesystem::path& working_directory,
                  const ProcessRunner& process_runner,
                  std::ostream& output,
                  std::ostream& error);

  bool read_box_metadata(const std::filesystem::path& box_path,
                         const std::filesystem::path& working_directory,
                         const ProcessRunner& process_runner,
                         BoxMetadata& metadata,
                         std::ostream& error);

  bool resolve_box_component(const std::filesystem::path& box_path,
                             const std::filesystem::path& working_directory,
                             const std::optional<std::string>& component,
                             const ProcessRunner& process_runner,
                             std::filesystem::path& component_box,
                             BoxMetadata& metadata,
                             std::ostream& error,
                             BoxMetadata* container_metadata = nullptr);

} // namespace forge
