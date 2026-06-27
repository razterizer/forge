#pragma once

#include "init_support.h"

#include <filesystem>
#include <iosfwd>
#include <optional>

namespace forge
{

  std::optional<VisualStudioProject> read_xcode_project(
    const std::filesystem::path& path,
    std::ostream& error);

} // namespace forge
