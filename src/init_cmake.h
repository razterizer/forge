#pragma once

#include "init_support.h"

#include <filesystem>
#include <iosfwd>
#include <optional>
#include <vector>

namespace forge
{

  std::optional<VisualStudioProject> read_cmake_project(
    const std::filesystem::path& path,
    std::ostream& error);

  std::vector<std::filesystem::path> read_cmake_subdirectories(
    const std::filesystem::path& cmake_path);

  bool cmake_defines_target(const std::filesystem::path& cmake_path);

} // namespace forge
