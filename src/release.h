#pragma once

#include "process.h"

#include <filesystem>
#include <iosfwd>

namespace forge
{

  int release_project(const std::filesystem::path& project_directory,
                      std::ostream& output,
                      std::ostream& error);

  int release_project(const std::filesystem::path& project_directory,
                      const ProcessRunner& process_runner,
                      std::ostream& output,
                      std::ostream& error);

} // namespace forge

