#pragma once

#include <filesystem>
#include <iosfwd>
#include <string>
#include <vector>

namespace forge
{

  struct ProjectScan
  {
    std::vector<std::string> sources;
    std::vector<std::string> public_headers;
    std::vector<std::string> headers;
    std::vector<std::string> entry_points;
  };

  bool is_ignored_project_scan_directory(const std::filesystem::path& path);
  bool is_cpp_source(const std::filesystem::path& path);
  bool is_cpp_header(const std::filesystem::path& path);
  bool contains_main_function(const std::filesystem::path& path);

  bool scan_project(const std::filesystem::path& project_directory,
                    ProjectScan& scan,
                    std::ostream& error);

} // namespace forge
