#include "runtime_assets.h"

#include <algorithm>
#include <fstream>
#include <set>
#include <string>
#include <system_error>

namespace forge
{
  namespace
  {

    bool is_safe_project_path(const std::filesystem::path& path)
    {
      if (path.empty() || path.is_absolute() || path.has_root_path())
      {
        return false;
      }

      for (const auto& component : path)
      {
        if (component == "." || component == ".." || component.empty())
        {
          return false;
        }
      }

      const auto first = path.begin()->string();
      return first != ".forge" && first != ".git";
    }

    bool add_asset(const std::filesystem::path& source,
                   const std::filesystem::path& path,
                   std::set<std::filesystem::path>& paths,
                   std::vector<RuntimeAsset>& assets,
                   std::ostream& error)
    {
      if (!paths.insert(path).second)
      {
        error << "forge: runtime assets collide at '" << path.generic_string() << "'\n";
        return false;
      }

      assets.push_back({ source, path });
      return true;
    }

  } // namespace

  bool collect_runtime_assets(const std::filesystem::path& project_directory,
                              const std::vector<std::filesystem::path>& declared,
                              std::vector<RuntimeAsset>& assets,
                              std::ostream& error)
  {
    std::set<std::filesystem::path> paths;
    std::error_code filesystem_error;
    assets.clear();

    for (const auto& path : declared)
    {
      if (!is_safe_project_path(path))
      {
        error << "forge: runtime asset paths must stay inside the project\n";
        return false;
      }

      const auto source = project_directory / path;

      if (std::filesystem::is_symlink(source, filesystem_error))
      {
        error << "forge: runtime assets cannot contain symbolic links\n";
        return false;
      }

      if (std::filesystem::is_regular_file(source, filesystem_error))
      {
        if (!add_asset(source, path, paths, assets, error))
        {
          return false;
        }

        continue;
      }

      if (!std::filesystem::is_directory(source, filesystem_error))
      {
        error << "forge: runtime asset '" << path.generic_string() << "' does not exist\n";
        return false;
      }

      for (const auto& entry : std::filesystem::recursive_directory_iterator {
        source,
        filesystem_error
      })
      {
        if (filesystem_error || entry.is_symlink())
        {
          error << "forge: runtime assets cannot contain symbolic links\n";
          return false;
        }

        if (entry.is_directory())
        {
          continue;
        }

        if (!entry.is_regular_file())
        {
          error << "forge: runtime asset directories may contain only regular files\n";
          return false;
        }

        const auto relative = entry.path().lexically_relative(project_directory);

        if (!add_asset(entry.path(), relative, paths, assets, error))
        {
          return false;
        }
      }

      if (filesystem_error)
      {
        error << "forge: could not inspect runtime assets\n";
        return false;
      }
    }

    std::sort(
      assets.begin(),
      assets.end(),
      [](const RuntimeAsset& left, const RuntimeAsset& right)
      {
        return left.path.generic_string() < right.path.generic_string();
      }
    );
    return true;
  }

  bool clean_runtime_assets(const std::filesystem::path& destination,
                            const std::filesystem::path& manifest,
                            std::ostream& error)
  {
    std::ifstream previous_manifest { manifest };
    std::string line;
    std::error_code filesystem_error;

    while (std::getline(previous_manifest, line))
    {
      if (!line.empty())
      {
        std::filesystem::remove(destination / line, filesystem_error);

        if (filesystem_error)
        {
          error << "forge: could not remove stale runtime asset '" << line << "'\n";
          return false;
        }
      }
    }

    previous_manifest.close();
    std::filesystem::remove(manifest, filesystem_error);

    if (filesystem_error)
    {
      error << "forge: could not remove stale runtime asset manifest\n";
      return false;
    }

    return true;
  }

  bool stage_runtime_assets(const std::vector<RuntimeAsset>& assets,
                            const std::filesystem::path& destination,
                            const std::filesystem::path& manifest,
                            std::ostream& error)
  {
    std::error_code filesystem_error;

    for (const auto& asset : assets)
    {
      const auto target = destination / asset.path;

      if (std::filesystem::exists(target, filesystem_error))
      {
        error << "forge: runtime asset collides with build output at '"
              << asset.path.generic_string() << "'\n";
        return false;
      }

      std::filesystem::create_directories(target.parent_path(), filesystem_error);

      if (filesystem_error)
      {
        error << "forge: could not create runtime asset directory\n";
        return false;
      }

      std::filesystem::copy_file(asset.source, target, filesystem_error);

      if (filesystem_error)
      {
        error << "forge: could not stage runtime asset '" << asset.path.generic_string() << "'\n";
        return false;
      }
    }

    std::filesystem::create_directories(manifest.parent_path(), filesystem_error);

    if (filesystem_error)
    {
      error << "forge: could not create runtime asset manifest directory\n";
      return false;
    }

    std::ofstream output { manifest };

    if (!output)
    {
      error << "forge: could not write runtime asset manifest\n";
      return false;
    }

    for (const auto& asset : assets)
    {
      output << asset.path.generic_string() << '\n';
    }

    return true;
  }

} // namespace forge
