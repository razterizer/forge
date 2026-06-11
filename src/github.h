#pragma once

#include <filesystem>
#include <iosfwd>
#include <string_view>

namespace forge
{

  int add_github_workflow_feature(const std::filesystem::path& project_directory,
                                  std::string_view feature,
                                  const std::filesystem::path& workflow_file,
                                  bool apply,
                                  std::ostream& output,
                                  std::ostream& error);

  bool generate_github_release_support(const std::filesystem::path& project_directory,
                                       std::ostream& error);

} // namespace forge
