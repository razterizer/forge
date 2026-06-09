#pragma once

#include <filesystem>
#include <iosfwd>

namespace forge
{

  int adopt_project(const std::filesystem::path& project_directory,
                    std::ostream& output,
                    std::ostream& error);

  int init_project(const std::filesystem::path& project_directory,
                   std::ostream& output,
                   std::ostream& error);

} // namespace forge
