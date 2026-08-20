#include "dependency_cache.h"

#include <algorithm>
#include <array>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace forge
{

  bool dependency_sources_are_older(const std::filesystem::path& dependency_directory,
                                    const Recipe& recipe,
                                    const std::filesystem::file_time_type& box_time)
  {
    std::error_code filesystem_error;
    std::vector<std::filesystem::path> files { dependency_directory / "forge.recipe.toml" };

    const auto add_target_inputs = [&](const auto& target)
    {
      for (const auto& source : target.sources)
        files.push_back(dependency_directory / source);

      for (const auto& header : target.public_headers)
        files.push_back(dependency_directory / header);

      for (const auto& include_directory : target.include_directories)
      {
        std::error_code traversal_error;
        std::filesystem::recursive_directory_iterator iterator {
          dependency_directory / include_directory,
          std::filesystem::directory_options::skip_permission_denied,
          traversal_error
        };
        const std::filesystem::recursive_directory_iterator end;

        while (!traversal_error && iterator != end)
        {
          const auto& entry = *iterator;
          const auto name = entry.path().filename().string();

          if (entry.is_directory(traversal_error)
              && (name == ".git"
                  || name == ".forge"
                  || name == "build"
                  || name == "out"
                  || name.starts_with("cmake-build-")))
          {
            iterator.disable_recursion_pending();
          }
          else if (!traversal_error && entry.is_regular_file(traversal_error))
          {
            const auto extension = entry.path().extension().string();

            if (extension == ".h"
                || extension == ".hpp"
                || extension == ".hh"
                || extension == ".hxx"
                || extension == ".inc"
                || extension == ".inl"
                || extension == ".ipp"
                || extension == ".tpp")
            {
              files.push_back(entry.path());
            }
          }

          iterator.increment(traversal_error);
        }
      }
    };

    add_target_inputs(recipe);
    std::set<std::string> visited_internal_targets;
    const auto add_internal_inputs = [&](const auto& self, std::string_view name) -> void
    {
      if (!visited_internal_targets.insert(std::string { name }).second)
        return;

      const auto target = std::find_if(
        recipe.internal_targets.begin(),
        recipe.internal_targets.end(),
        [name](const RecipeTarget& candidate)
        {
          return candidate.name == name;
        }
      );

      if (target == recipe.internal_targets.end())
        return;

      add_target_inputs(*target);

      for (const auto& dependency : target->dependencies)
        self(self, dependency);
    };

    for (const auto& dependency : recipe.selected_internal_dependencies)
      add_internal_inputs(add_internal_inputs, dependency);

    for (const auto& runtime : recipe.runtime_files)
    {
      const auto path = dependency_directory / runtime.source;

      if (std::filesystem::is_directory(path))
      {
        for (const auto& entry : std::filesystem::recursive_directory_iterator { path })
        {
          if (entry.is_regular_file())
            files.push_back(entry.path());
        }
      }
      else
        files.push_back(path);
    }

    for (const auto& profile : recipe.imports)
    {
      const std::array imported_groups {
        &profile.public_headers,
        &profile.static_libraries,
        &profile.dynamic_libraries,
        &profile.import_libraries
      };

      for (const auto* group : imported_groups)
      {
        for (const auto& imported : *group)
        {
          const auto path = dependency_directory / imported;

          if (std::filesystem::is_directory(path))
          {
            for (const auto& entry : std::filesystem::recursive_directory_iterator { path })
            {
              if (entry.is_regular_file())
                files.push_back(entry.path());
            }
          }
          else
            files.push_back(path);
        }
      }
    }

    for (const auto& file : files)
    {
      const auto modified = std::filesystem::last_write_time(file, filesystem_error);

      if (filesystem_error || modified > box_time)
        return false;
    }

    return true;
  }

} // namespace forge
