#pragma once

#include <filesystem>
#include <iosfwd>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "recipe.h"

namespace forge
{

  struct ProjectScan
  {
    std::vector<std::string> sources;
    std::vector<std::string> public_headers;
    std::vector<std::string> headers;
    std::vector<std::string> entry_points;
  };

  struct IncludedHeader
  {
    std::string path;
    bool quoted = false;
  };

  bool is_ignored_project_scan_directory(const std::filesystem::path& path);
  bool is_cpp_source(const std::filesystem::path& path);
  bool is_cpp_header(const std::filesystem::path& path);
  bool contains_main_function(const std::filesystem::path& path);

  bool scan_project(const std::filesystem::path& project_directory,
                    ProjectScan& scan,
                    std::ostream& error);

  std::vector<IncludedHeader> included_headers(const std::filesystem::path& path);

  std::optional<std::string> resolve_local_header(const std::string& including_file,
                                                  const std::string& include,
                                                  const std::vector<std::string>& headers);

  std::set<std::string> reachable_local_headers(const std::filesystem::path& project_directory,
                                                const std::string& source,
                                                const std::vector<std::string>& headers);

  std::vector<std::string> infer_include_directories(
    const std::filesystem::path& project_directory,
    const std::vector<std::string>& sources,
    const std::vector<std::string>& headers);

  std::vector<RuntimeFile> infer_runtime_files(
    const std::filesystem::path& project_directory,
    const std::vector<std::string>& target_sources,
    const std::vector<std::string>& headers);

} // namespace forge
