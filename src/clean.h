#pragma once

#include <filesystem>
#include <iosfwd>
#include <optional>
#include <string>

namespace forge
{

  struct CleanOptions
  {
    std::optional<std::string> target;
    std::optional<std::string> configuration;
    std::optional<std::string> style;
    std::optional<std::string> profile;
  };

  int clean_project(const std::filesystem::path& project_directory,
                    std::ostream& output,
                    std::ostream& error);

  int clean_project(const std::filesystem::path& project_directory,
                    const CleanOptions& options,
                    std::ostream& output,
                    std::ostream& error);

} // namespace forge
