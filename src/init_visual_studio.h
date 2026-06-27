#pragma once

#include "init_support.h"

#include <filesystem>
#include <iosfwd>
#include <optional>
#include <vector>

namespace forge
{

  std::optional<VisualStudioProject> read_visual_studio_project(
    const std::filesystem::path& path,
    std::ostream& error);

  bool is_cmake_generated_visual_studio_project(const std::filesystem::path& path);

  std::vector<std::filesystem::path> read_solution_projects(
    const std::filesystem::path& solution_path,
    std::ostream& error);

} // namespace forge
