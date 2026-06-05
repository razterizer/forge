#pragma once

#include <filesystem>
#include <iosfwd>

#include "process.h"

namespace forge
{

  int build_project(const std::filesystem::path& project_directory,
                    std::ostream& output,
                    std::ostream& error);

  int build_project(const std::filesystem::path& project_directory,
                    const ProcessRunner& process_runner,
                    std::ostream& output,
                    std::ostream& error);

} // namespace forge
