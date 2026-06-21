#pragma once

#include <filesystem>
#include <iosfwd>
#include <span>
#include <string_view>

namespace forge::cli
{

  inline constexpr std::string_view version = "0.8.4+build.12";

  int run(std::span<const std::string_view> arguments,
          std::ostream& output,
          std::ostream& error);

  int run(std::span<const std::string_view> arguments,
          const std::filesystem::path& working_directory,
          std::ostream& output,
          std::ostream& error);

} // namespace forge::cli
