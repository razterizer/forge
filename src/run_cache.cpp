#include "run_cache.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <string_view>
#include <system_error>

namespace forge
{
  namespace
  {
    std::string run_cache_component(std::string_view value)
    {
      if (value.empty())
        return "default";

      constexpr char hex[] = "0123456789abcdef";
      std::string encoded;

      for (const auto byte : value)
      {
        const auto character = static_cast<unsigned char>(byte);

        if (std::isalnum(character) || character == '-' || character == '_')
          encoded += static_cast<char>(character);
        else
        {
          encoded += '%';
          encoded += hex[character >> 4];
          encoded += hex[character & 0xf];
        }
      }

      return encoded;
    }

    bool copy_run_file(const std::filesystem::path& source,
                       const std::filesystem::path& destination,
                       std::ostream& error)
    {
      std::error_code filesystem_error;
      std::filesystem::create_directories(destination.parent_path(), filesystem_error);

      if (!filesystem_error)
      {
        std::filesystem::copy_file(
          source,
          destination,
          std::filesystem::copy_options::overwrite_existing,
          filesystem_error
        );
      }

      if (!filesystem_error)
        return true;

      error << "forge: could not cache runtime file '" << source.string()
            << "': " << filesystem_error.message() << '\n';
      return false;
    }

    bool matches_value(const std::optional<std::string>& actual,
                       const std::optional<std::string>& expected)
    {
      return !expected || actual == expected;
    }

    bool matches_configuration(const std::optional<std::string>& actual,
                               const std::optional<std::string>& expected)
    {
      if (matches_value(actual, expected))
        return true;

      if (!actual || !expected)
        return false;

      auto left = *actual;
      auto right = *expected;
      const auto lower = [](std::string& value)
      {
        std::ranges::transform(value, value.begin(), [](unsigned char byte)
        {
          return static_cast<char>(std::tolower(byte));
        });
      };
      lower(left);
      lower(right);
      return left == right;
    }
  }

  RunCacheMetadata read_run_cache_metadata(const std::filesystem::path& path)
  {
    std::ifstream file { path };
    RunCacheMetadata metadata;
    std::string line;

    while (std::getline(file, line))
    {
      const auto equals = line.find(" = \"");

      if (equals == std::string::npos || line.size() <= equals + 4 || line.back() != '"')
        continue;

      const auto value = line.substr(equals + 4, line.size() - equals - 5);
      const auto key = std::string_view { line }.substr(0, equals);

      if (key == "target")
        metadata.target = value;
      else if (key == "configuration")
        metadata.configuration = value;
      else if (key == "style")
        metadata.style = value;
      else if (key == "profile")
        metadata.profile = value;
    }

    return metadata;
  }

  bool matches_run_cache_metadata(const RunCacheMetadata& metadata,
                                  const RunCacheSelectors& selectors)
  {
    return matches_configuration(metadata.configuration, selectors.configuration)
      && matches_value(metadata.target, selectors.target)
      && matches_value(metadata.style, selectors.style)
      && matches_value(metadata.profile, selectors.profile);
  }

  std::optional<std::filesystem::path> current_cached_run_directory(
    const std::filesystem::path& project_directory)
  {
    const auto run_root = project_directory / ".forge" / "run";
    std::ifstream current { run_root / "current.txt" };
    std::string relative;

    if (!std::getline(current, relative) || relative.empty())
      return std::nullopt;

    const auto path = std::filesystem::path { relative };

    if (path.is_absolute()
        || std::ranges::any_of(path, [](const auto& component)
        {
          return component == "..";
        }))
    {
      return std::nullopt;
    }

    std::error_code filesystem_error;
    const auto directory = run_root / path;
    return std::filesystem::is_directory(directory, filesystem_error)
      ? std::optional<std::filesystem::path> { directory }
      : std::nullopt;
  }

  bool cache_executable_run_variant(const std::filesystem::path& project_directory,
                                    const std::filesystem::path& build_directory,
                                    const std::filesystem::path& artifact,
                                    const Recipe& recipe,
                                    const RecipeSelection& selection,
                                    std::string_view configuration,
                                    const std::vector<RuntimeAsset>& runtime_assets,
                                    std::ostream& error)
  {
    auto relative = std::filesystem::path { run_cache_component(recipe.name) }
      / run_cache_component(configuration)
      / run_cache_component(selection.style)
      / run_cache_component(selection.profile);
    const auto run_root = project_directory / ".forge" / "run";
    const auto destination = run_root / relative;
    std::error_code filesystem_error;
    std::filesystem::remove_all(destination, filesystem_error);

    if (filesystem_error)
    {
      error << "forge: could not replace cached run variant '" << destination.string()
            << "': " << filesystem_error.message() << '\n';
      return false;
    }

    if (!copy_run_file(artifact, destination / artifact.filename(), error))
      return false;

    if (std::filesystem::is_regular_file(build_directory / "forge-toolchain.toml")
        && !copy_run_file(
          build_directory / "forge-toolchain.toml",
          destination / "forge-toolchain.toml",
          error
        ))
    {
      return false;
    }

    for (const auto& asset : runtime_assets)
    {
      if (!copy_run_file(build_directory / asset.path, destination / asset.path, error))
        return false;
    }

    const auto runtime_directory = build_directory / "runtime";

    if (std::filesystem::is_directory(runtime_directory, filesystem_error))
    {
      std::filesystem::copy(
        runtime_directory,
        destination / "runtime",
        std::filesystem::copy_options::recursive
          | std::filesystem::copy_options::overwrite_existing,
        filesystem_error
      );

      if (filesystem_error)
      {
        error << "forge: could not cache runtime dependencies: "
              << filesystem_error.message() << '\n';
        return false;
      }

#ifdef _WIN32
      for (const auto& entry : std::filesystem::directory_iterator { runtime_directory })
      {
        if (entry.is_regular_file()
            && !copy_run_file(entry.path(), destination / entry.path().filename(), error))
        {
          return false;
        }
      }
#endif
    }

    std::filesystem::create_directories(destination, filesystem_error);
    std::ofstream metadata { destination / "forge-run.toml" };
    metadata << "target = \"" << recipe.name << "\"\n"
             << "configuration = \"" << configuration << "\"\n"
             << "style = \"" << selection.style << "\"\n"
             << "profile = \"" << selection.profile << "\"\n";

    if (!metadata)
    {
      error << "forge: could not write cached run metadata\n";
      return false;
    }

    std::filesystem::create_directories(run_root, filesystem_error);
    std::ofstream current { run_root / "current.txt" };
    current << relative.generic_string() << '\n';

    if (!current)
    {
      error << "forge: could not select cached run variant\n";
      return false;
    }

    return true;
  }

  bool select_cached_run_variant(const std::filesystem::path& project_directory,
                                 const std::optional<std::string>& target,
                                 const std::optional<std::string>& configuration,
                                 const std::optional<std::string>& style,
                                 const std::optional<std::string>& profile,
                                 std::ostream& error)
  {
    const auto run_root = project_directory / ".forge" / "run";
    std::optional<std::filesystem::path> selected;
    std::filesystem::file_time_type selected_time;
    std::error_code filesystem_error;
    const RunCacheSelectors selectors { target, configuration, style, profile };

    for (const auto& entry : std::filesystem::recursive_directory_iterator { run_root, filesystem_error })
    {
      if (filesystem_error)
        break;

      if (entry.path().filename() != "forge-run.toml"
          || !matches_run_cache_metadata(read_run_cache_metadata(entry.path()), selectors))
      {
        continue;
      }

      const auto modified = std::filesystem::last_write_time(entry.path(), filesystem_error);

      if (!filesystem_error && (!selected || modified > selected_time))
      {
        selected = entry.path().parent_path();
        selected_time = modified;
      }
    }

    if (!selected)
    {
      error << "forge: no cached run variant matches the requested selectors\n";
      return false;
    }

    const auto relative = selected->lexically_relative(run_root);
    std::ofstream current { run_root / "current.txt" };
    current << relative.generic_string() << '\n';

    if (!current)
    {
      error << "forge: could not select cached run variant\n";
      return false;
    }

    return true;
  }

} // namespace forge
