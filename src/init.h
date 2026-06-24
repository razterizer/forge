#pragma once

#include "fprocess.h"

#include <filesystem>
#include <iosfwd>
#include <optional>
#include <string>

namespace forge
{

  enum class DependencyStyle
  {
    local,
    git
  };

  struct AdoptOptions
  {
    DependencyStyle dependency_style = DependencyStyle::local;
    std::optional<std::string> library_type;
    std::optional<std::string> initial_version;
    std::optional<std::filesystem::path> version_header_path;
  };

  int adopt_project(const std::filesystem::path& project_directory,
                    std::ostream& output,
                    std::ostream& error);

  int adopt_project(const std::filesystem::path& project_directory,
                    const AdoptOptions& options,
                    const ProcessRunner& process_runner,
                    std::ostream& output,
                    std::ostream& error);

  int init_project(const std::filesystem::path& project_directory,
                   std::ostream& output,
                   std::ostream& error);

} // namespace forge
