#pragma once

#include <filesystem>
#include <iosfwd>
#include <span>
#include <string_view>

namespace forge::cli
{

  inline constexpr std::string_view version = "0.14.3+build.33";

  int run(std::span<const std::string_view> arguments,
          std::ostream& output,
          std::ostream& error);

  int run(std::span<const std::string_view> arguments,
          const std::filesystem::path& working_directory,
          std::ostream& output,
          std::ostream& error);

} // namespace forge::cli
