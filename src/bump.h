#pragma once

#include <filesystem>
#include <iosfwd>
#include <string_view>

namespace forge
{

  int bump_project(const std::filesystem::path& project_directory,
                   std::string_view component,
                   std::ostream& output,
                   std::ostream& error);

} // namespace forge
