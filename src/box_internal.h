#pragma once

#include "box.h"

#include <functional>

namespace forge
{

  using BoxProjectBuilder = std::function<int(
    const std::filesystem::path&,
    const BuildOptions&,
    const ProcessRunner&,
    std::ostream&,
    std::ostream&)>;

  int create_box_with_builder(const std::filesystem::path& project_directory,
                              const std::optional<std::string>& target,
                              const BuildOptions& options,
                              const ProcessRunner& process_runner,
                              const BoxProjectBuilder& project_builder,
                              std::ostream& output,
                              std::ostream& error);

} // namespace forge
