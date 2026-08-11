#pragma once

#include <filesystem>
#include <iosfwd>
#include <optional>
#include <string>
#include <string_view>

namespace forge
{

  struct NewOptions
  {
    std::optional<std::string> initial_version;
    std::optional<std::filesystem::path> version_header_path;
  };

  int new_project(const std::filesystem::path& parent_directory,
                  std::string_view project_name,
                  std::ostream& output,
                  std::ostream& error);

  int new_project(const std::filesystem::path& parent_directory,
                  std::string_view project_name,
                  const NewOptions& options,
                  std::ostream& output,
                  std::ostream& error);

} // namespace forge
