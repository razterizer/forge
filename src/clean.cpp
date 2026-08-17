#include "clean.h"
#include "run_cache.h"

#include <fstream>
#include <system_error>
#include <vector>

namespace forge
{

  int clean_project(const std::filesystem::path& project_directory,
                    std::ostream& output,
                    std::ostream& error)
  {
    std::error_code filesystem_error;

    if (!std::filesystem::is_regular_file(
      project_directory / "forge.recipe.toml",
      filesystem_error
    ))
    {
      error << "forge: forge.recipe.toml was not found in the current directory\n";
      return 2;
    }

    const auto forge_directory = project_directory / ".forge";
    const auto removed = std::filesystem::remove_all(forge_directory, filesystem_error);

    if (filesystem_error)
    {
      error
        << "forge: could not clean '" << forge_directory.string()
        << "': " << filesystem_error.message() << '\n';
      return 2;
    }

    if (removed == 0)
      output << "Already clean\n";
    else
      output << "Cleaned " << forge_directory.string() << '\n';

    return 0;
  }

  int clean_project(const std::filesystem::path& project_directory,
                    const CleanOptions& options,
                    std::ostream& output,
                    std::ostream& error)
  {
    std::error_code filesystem_error;

    if (!std::filesystem::is_regular_file(project_directory / "forge.recipe.toml", filesystem_error))
    {
      error << "forge: forge.recipe.toml was not found in the current directory\n";
      return 2;
    }

    const auto run_root = project_directory / ".forge" / "run";
    std::vector<std::filesystem::path> variants;
    const RunCacheSelectors selectors {
      options.target,
      options.configuration,
      options.style,
      options.profile
    };

    for (const auto& entry : std::filesystem::recursive_directory_iterator { run_root, filesystem_error })
    {
      if (filesystem_error)
        break;

      if (entry.path().filename() != "forge-run.toml")
        continue;

      if (matches_run_cache_metadata(read_run_cache_metadata(entry.path()), selectors))
        variants.push_back(entry.path().parent_path());
    }

    if (filesystem_error && std::filesystem::exists(run_root))
    {
      error << "forge: could not inspect cached run variants: "
            << filesystem_error.message() << '\n';
      return 2;
    }

    std::uintmax_t removed = 0;

    for (const auto& variant : variants)
      removed += std::filesystem::remove_all(variant, filesystem_error);

    std::ifstream current_file { run_root / "current.txt" };
    std::string current_relative;
    std::getline(current_file, current_relative);
    current_file.close();

    if (current_relative.empty()
        || !std::filesystem::is_directory(run_root / current_relative, filesystem_error))
      std::filesystem::remove(run_root / "current.txt", filesystem_error);

    if (filesystem_error)
    {
      error << "forge: could not clean cached run variant: "
            << filesystem_error.message() << '\n';
      return 2;
    }

    if (removed == 0)
      output << "No matching cached run variants\n";
    else
      output << "Cleaned " << variants.size() << " cached run variant"
             << (variants.size() == 1 ? "" : "s") << '\n';

    return 0;
  }

} // namespace forge
