#pragma once

#include <filesystem>
#include <iosfwd>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "runtime_assets.h"

namespace forge
{

  struct RunCacheMetadata
  {
    std::optional<std::string> target;
    std::optional<std::string> configuration;
    std::optional<std::string> style;
    std::optional<std::string> profile;
  };

  struct RunCacheSelectors
  {
    std::optional<std::string> target;
    std::optional<std::string> configuration;
    std::optional<std::string> style;
    std::optional<std::string> profile;
  };

  RunCacheMetadata read_run_cache_metadata(const std::filesystem::path& path);

  bool matches_run_cache_metadata(const RunCacheMetadata& metadata,
                                  const RunCacheSelectors& selectors);

  std::optional<std::filesystem::path> current_cached_run_directory(
    const std::filesystem::path& project_directory
  );

  bool cache_executable_run_variant(const std::filesystem::path& project_directory,
                                    const std::filesystem::path& build_directory,
                                    const std::filesystem::path& artifact,
                                    const Recipe& recipe,
                                    const RecipeSelection& selection,
                                    std::string_view configuration,
                                    const std::vector<RuntimeAsset>& runtime_assets,
                                    std::ostream& error);

  bool select_cached_run_variant(const std::filesystem::path& project_directory,
                                 const std::optional<std::string>& target,
                                 const std::optional<std::string>& configuration,
                                 const std::optional<std::string>& style,
                                 const std::optional<std::string>& profile,
                                 std::ostream& error);

} // namespace forge
