#pragma once

#include <filesystem>
#include <iosfwd>
#include <string_view>

namespace forge
{

  int new_project(
    const std::filesystem::path& parent_directory,
    std::string_view project_name,
    std::ostream& output,
    std::ostream& error
  );

} // namespace forge

