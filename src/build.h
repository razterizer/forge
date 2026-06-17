#pragma once

#include <filesystem>
#include <iosfwd>
#include <optional>
#include <string>
#include <vector>

#include "fprocess.h"

namespace forge
{

  struct BuildOptions
  {
    std::optional<std::string> update_dependency;
    std::optional<std::string> update_target;
    std::optional<std::string> target;
    std::optional<std::string> profile;
    std::vector<std::string> compile_definitions;
    std::string configuration = "Debug";
    bool dependencies_only = false;
    bool update_dependencies = false;
  };

  int build_project(const std::filesystem::path& project_directory,
                    std::ostream& output,
                    std::ostream& error);

  int build_project(const std::filesystem::path& project_directory,
                    const BuildOptions& options,
                    std::ostream& output,
                    std::ostream& error);

  int build_project(const std::filesystem::path& project_directory,
                    const ProcessRunner& process_runner,
                    std::ostream& output,
                    std::ostream& error);

  int build_project(const std::filesystem::path& project_directory,
                    const BuildOptions& options,
                    const ProcessRunner& process_runner,
                    std::ostream& output,
                    std::ostream& error);

} // namespace forge
