#include "file_support.h"

#include <fstream>
#include <ostream>
#include <system_error>

namespace forge
{

  bool is_safe_path_component(std::string_view value)
  {
    return
      !value.empty()
      && value != "."
      && value != ".."
      && value.find('/') == std::string_view::npos
      && value.find('\\') == std::string_view::npos;
  }

  bool is_safe_project_path(const std::filesystem::path& path)
  {
    if (path.empty() || path.is_absolute() || path.has_root_path())
      return false;

    for (const auto& component : path)
    {
      if (component == "..")
        return false;
    }

    return true;
  }

  bool copy_file(const std::filesystem::path& source,
                 const std::filesystem::path& destination,
                 std::ostream& error)
  {
    std::error_code filesystem_error;
    std::filesystem::copy_file(
      source,
      destination,
      std::filesystem::copy_options::overwrite_existing,
      filesystem_error
    );

    if (filesystem_error)
    {
      error << "forge: could not copy '" << source.string() << "'\n";
      return false;
    }

    return true;
  }

  bool write_file(const std::filesystem::path& path,
                  std::string_view contents,
                  std::ostream& error,
                  bool create_parent_directories)
  {
    if (create_parent_directories)
    {
      std::error_code filesystem_error;
      std::filesystem::create_directories(path.parent_path(), filesystem_error);

      if (filesystem_error)
      {
        error << "forge: could not create directory for '" << path.string() << "'\n";
        return false;
      }
    }

    std::ofstream file { path };

    if (!file)
    {
      error << "forge: could not create '" << path.string() << "'\n";
      return false;
    }

    file << contents;

    if (!file)
    {
      error << "forge: could not write '" << path.string() << "'\n";
      return false;
    }

    return true;
  }

  bool prepare_empty_directory(const std::filesystem::path& path,
                               std::ostream& error)
  {
    std::error_code filesystem_error;
    std::filesystem::remove_all(path, filesystem_error);
    filesystem_error.clear();
    std::filesystem::create_directories(path, filesystem_error);

    if (filesystem_error)
    {
      error << "forge: could not create '" << path.string() << "'\n";
      return false;
    }

    return true;
  }

} // namespace forge
