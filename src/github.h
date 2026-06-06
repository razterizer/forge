#pragma once

#include <filesystem>
#include <iosfwd>
#include <string_view>

namespace forge
{

  bool generate_github_release_support(const std::filesystem::path& project_directory,
                                       std::string_view project_name,
                                       std::ostream& error);

} // namespace forge
