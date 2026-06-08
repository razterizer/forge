#pragma once

#include "process.h"

#include <filesystem>
#include <iosfwd>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace forge
{

  int test_project(const std::filesystem::path& project_directory,
                   const std::optional<std::string>& target,
                   std::span<const std::string_view> arguments,
                   std::ostream& output,
                   std::ostream& error);

  int test_project(const std::filesystem::path& project_directory,
                   const std::optional<std::string>& target,
                   const std::optional<std::string>& profile,
                   std::span<const std::string_view> arguments,
                   std::ostream& output,
                   std::ostream& error);

  int test_project(const std::filesystem::path& project_directory,
                   const std::optional<std::string>& target,
                   std::span<const std::string_view> arguments,
                   const ProcessRunner& process_runner,
                   std::ostream& output,
                   std::ostream& error);

  int test_project(const std::filesystem::path& project_directory,
                   const std::optional<std::string>& target,
                   const std::optional<std::string>& profile,
                   std::span<const std::string_view> arguments,
                   const ProcessRunner& process_runner,
                   std::ostream& output,
                   std::ostream& error);

} // namespace forge
