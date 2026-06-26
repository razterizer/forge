#pragma once

#include <filesystem>
#include <iosfwd>
#include <string_view>

namespace forge
{

  bool is_safe_path_component(std::string_view value);
  bool is_safe_project_path(const std::filesystem::path& path);
  bool copy_file(const std::filesystem::path& source,
                 const std::filesystem::path& destination,
                 std::ostream& error);
  bool write_file(const std::filesystem::path& path,
                  std::string_view contents,
                  std::ostream& error,
                  bool create_parent_directories = false);
  bool prepare_empty_directory(const std::filesystem::path& path,
                               std::ostream& error);

} // namespace forge
