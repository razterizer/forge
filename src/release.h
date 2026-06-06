#pragma once

#include "process.h"

#include <filesystem>
#include <iosfwd>
#include <optional>
#include <string>

namespace forge
{

  struct GitReleaseOptions
  {
    std::optional<std::string> tag_format;
    bool force_tag = false;
  };

  int release_project(const std::filesystem::path& project_directory,
                      std::ostream& output,
                      std::ostream& error);

  int release_project(const std::filesystem::path& project_directory,
                      const ProcessRunner& process_runner,
                      std::ostream& output,
                      std::ostream& error);

  int release_git(const std::filesystem::path& project_directory,
                  const GitReleaseOptions& options,
                  std::ostream& output,
                  std::ostream& error);

  int release_git(const std::filesystem::path& project_directory,
                  const GitReleaseOptions& options,
                  const ProcessRunner& process_runner,
                  std::ostream& output,
                  std::ostream& error);

} // namespace forge
