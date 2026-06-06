#include "clean.h"

#include <system_error>

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
    {
      output << "Already clean\n";
    }
    else
    {
      output << "Cleaned " << forge_directory.string() << '\n';
    }

    return 0;
  }

} // namespace forge
