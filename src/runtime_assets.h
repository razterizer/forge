#pragma once

#include <filesystem>
#include <iosfwd>
#include <vector>

namespace forge
{

  struct RuntimeAsset
  {
    std::filesystem::path source;
    std::filesystem::path path;
  };

  bool collect_runtime_assets(const std::filesystem::path& project_directory,
                              const std::vector<std::filesystem::path>& declared,
                              std::vector<RuntimeAsset>& assets,
                              std::ostream& error);

  bool clean_runtime_assets(const std::filesystem::path& destination,
                            const std::filesystem::path& manifest,
                            std::ostream& error);

  bool stage_runtime_assets(const std::vector<RuntimeAsset>& assets,
                            const std::filesystem::path& destination,
                            const std::filesystem::path& manifest,
                            std::ostream& error);

} // namespace forge
