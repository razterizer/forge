#pragma once

#include "fprocess.h"

#include <filesystem>
#include <iosfwd>
#include <optional>
#include <string>

namespace forge
{

  struct AdoptOptions
  {
    bool github = false;
    std::optional<std::string> library_type;
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
