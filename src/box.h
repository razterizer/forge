#pragma once

#include "process.h"

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

  struct BoxMetadata
  {
    std::string name;
    std::string version;
    std::optional<int> build_number;
    std::string type;
    std::string os;
    std::string arch;
    std::vector<BoxArtifactMetadata> artifacts;
  };

  int create_box(const std::filesystem::path& project_directory,
                 std::ostream& output,
                 std::ostream& error);

  int create_box(const std::filesystem::path& project_directory,
                 const ProcessRunner& process_runner,
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

} // namespace forge
