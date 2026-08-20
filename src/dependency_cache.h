#pragma once

#include "recipe.h"

#include <filesystem>

namespace forge
{

  bool dependency_sources_are_older(const std::filesystem::path& dependency_directory,
                                    const Recipe& recipe,
                                    const std::filesystem::file_time_type& box_time);

} // namespace forge
