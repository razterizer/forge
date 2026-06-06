#pragma once

#include "process.h"

#include <filesystem>
#include <iosfwd>
#include <optional>
#include <string>

namespace forge
{

  struct GitHubReleaseOptions
  {
    std::optional<std::string> tag_format;
  };

  int release_project(const std::filesystem::path& project_directory,
                      std::ostream& output,
                      std::ostream& error);

  int release_project(const std::filesystem::path& project_directory,
                      const ProcessRunner& process_runner,
                      std::ostream& output,
                      std::ostream& error);

  int release_github(const std::filesystem::path& project_directory,
                     const GitHubReleaseOptions& options,
                     std::ostream& output,
                     std::ostream& error);

  int release_github(const std::filesystem::path& project_directory,
                     const GitHubReleaseOptions& options,
                     const ProcessRunner& process_runner,
                     std::ostream& output,
                     std::ostream& error);

} // namespace forge
