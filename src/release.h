#pragma once

#include "fprocess.h"

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

  struct PrepareReleaseOptions
  {
    std::optional<std::string> target;
    std::optional<std::string> profile;
    std::optional<std::string> executable_suffix;
    bool skip_unsupported = false;
    bool merge_executable_release = false;
  };

  int release_project(const std::filesystem::path& project_directory,
                      std::ostream& output,
                      std::ostream& error);

  int release_project(const std::filesystem::path& project_directory,
                      const std::optional<std::string>& target,
                      std::ostream& output,
                      std::ostream& error);

  int release_project(const std::filesystem::path& project_directory,
                      const ProcessRunner& process_runner,
                      std::ostream& output,
                      std::ostream& error);

  int release_project(const std::filesystem::path& project_directory,
                      const std::optional<std::string>& target,
                      const ProcessRunner& process_runner,
                      std::ostream& output,
                      std::ostream& error);

  int release_project(const std::filesystem::path& project_directory,
                      const std::optional<std::string>& target,
                      const std::optional<std::string>& profile,
                      const ProcessRunner& process_runner,
                      std::ostream& output,
                      std::ostream& error);

  int prepare_release(const std::filesystem::path& project_directory,
                      std::ostream& output,
                      std::ostream& error);

  int prepare_release(const std::filesystem::path& project_directory,
                      const std::optional<std::string>& target,
                      std::ostream& output,
                      std::ostream& error);

  int prepare_release(const std::filesystem::path& project_directory,
                      const ProcessRunner& process_runner,
                      std::ostream& output,
                      std::ostream& error);

  int prepare_release(const std::filesystem::path& project_directory,
                      const std::optional<std::string>& target,
                      const ProcessRunner& process_runner,
                      std::ostream& output,
                      std::ostream& error);

  int prepare_release(const std::filesystem::path& project_directory,
                      const PrepareReleaseOptions& options,
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
