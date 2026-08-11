#include "init_support.h"

#include <algorithm>

namespace forge
{

  std::optional<std::string> project_relative_path(
    const std::filesystem::path& project_directory,
    const std::filesystem::path& path)
  {
    auto normalized_path = path.generic_string();
    std::replace(normalized_path.begin(), normalized_path.end(), '\\', '/');

    if (normalized_path.empty() || normalized_path.find("$(") != std::string::npos)
      return std::nullopt;

    const auto normalized =
      (std::filesystem::path { normalized_path }.is_absolute()
        ? std::filesystem::path { normalized_path }
        : project_directory / normalized_path).lexically_normal();
    const auto relative = normalized.lexically_relative(project_directory);

    if (relative.empty() || relative.is_absolute() || *relative.begin() == "..")
      return std::nullopt;

    return relative.generic_string();
  }

  void sort_unique(BuildProfile& profile)
  {
    std::ranges::sort(profile.include_directories);
    std::ranges::sort(profile.compile_definitions);
    profile.include_directories.erase(
      std::unique(profile.include_directories.begin(), profile.include_directories.end()),
      profile.include_directories.end()
    );
    profile.compile_definitions.erase(
      std::unique(profile.compile_definitions.begin(), profile.compile_definitions.end()),
      profile.compile_definitions.end()
    );
  }


} // namespace forge
