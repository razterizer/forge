#pragma once

#include "process.h"

#include <filesystem>
#include <iosfwd>
#include <span>
#include <string_view>

namespace forge
{

  int run_project(const std::filesystem::path& project_directory,
                  std::span<const std::string_view> arguments,
                  std::ostream& output,
                  std::ostream& error);

  int run_project(const std::filesystem::path& project_directory,
                  std::span<const std::string_view> arguments,
                  const ProcessRunner& process_runner,
                  std::ostream& output,
                  std::ostream& error);

} // namespace forge

