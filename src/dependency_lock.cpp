#include "dependency_lock.h"

#include <fstream>
#include <optional>
#include <string_view>
#include <system_error>

namespace forge
{
  namespace
  {

    std::string_view trim(std::string_view value)
    {
      const auto first = value.find_first_not_of(" \t\r\n");

      if (first == std::string_view::npos)
        return {};

      const auto last = value.find_last_not_of(" \t\r\n");
      return value.substr(first, last - first + 1);
    }

    bool parse_string(std::string_view value, std::string& result)
    {
      value = trim(value);

      if (value.size() < 2 || value.front() != '"' || value.back() != '"')
        return false;

      result = std::string { value.substr(1, value.size() - 2) };
      return result.find('"') == std::string::npos;
    }

    bool is_safe_name(std::string_view name)
    {
      return !name.empty()
        && name != "."
        && name != ".."
        && name.find('/') == std::string_view::npos
        && name.find('\\') == std::string_view::npos;
    }

    bool is_sha256(std::string_view value)
    {
      return value.size() == 64
        && value.find_first_not_of("0123456789abcdef") == std::string_view::npos;
    }

  } // namespace

  std::string DependencyLock::key(std::string_view name,
                                  std::string_view variant,
                                  std::string_view target)
  {
    return std::string { name }
      + '\n' + std::string { variant }
      + '\n' + std::string { target };
  }

  bool DependencyLock::load(const std::filesystem::path& project_directory,
                            std::ostream& error)
  {
    if (loaded_)
      return true;

    loaded_ = true;
    const auto path = project_directory / "forge.lock.toml";

    if (!std::filesystem::is_regular_file(path))
      return true;

    std::ifstream file { path };
    std::optional<LockedDependency> dependency;
    std::string line;
    std::size_t line_number = 0;
    int format = 0;

    const auto store_dependency =
      [this, &dependency, &error]() -> bool
      {
        if (!dependency)
          return true;

        if (dependency->name.empty()
            || dependency->github.empty()
            || (!dependency->package.empty() && !is_safe_name(dependency->package))
            || (!dependency->component.empty() && !is_safe_name(dependency->component))
            || (!dependency->variant.empty() && !is_safe_name(dependency->variant))
            || dependency->version.empty()
            || dependency->target.empty()
            || dependency->url.empty()
            || !is_sha256(dependency->sha256))
        {
          error << "forge: forge.lock.toml contains an incomplete dependency\n";
          return false;
        }

        if (dependency->package.empty())
          dependency->package = dependency->name;

        if (!entries.emplace(key(dependency->name, dependency->variant, dependency->target), *dependency).second)
        {
          error << "forge: forge.lock.toml contains a duplicate dependency target\n";
          return false;
        }

        dependency.reset();
        return true;
      };

    while (std::getline(file, line))
    {
      ++line_number;
      const auto content = trim(line);

      if (content.empty() || content.front() == '#')
        continue;

      if (content == "[[dependency]]")
      {
        if (!store_dependency())
          return false;

        dependency.emplace();
        continue;
      }

      const auto equals = content.find('=');

      if (equals == std::string_view::npos)
      {
        error << "forge: invalid forge.lock.toml line " << line_number << '\n';
        return false;
      }

      const auto field = trim(content.substr(0, equals));
      const auto value = trim(content.substr(equals + 1));

      if (!dependency && field == "format" && (value == "1" || value == "2"))
      {
        format = value == "1" ? 1 : 2;
        continue;
      }

      std::string parsed;

      if (!dependency
          || !parse_string(value, parsed)
          || (field != "name"
              && field != "github"
              && field != "package"
              && field != "component"
              && field != "variant"
              && field != "version"
              && field != "target"
              && field != "url"
              && field != "sha256"))
      {
        error << "forge: invalid forge.lock.toml line " << line_number << '\n';
        return false;
      }

      if (field == "name")
        dependency->name = std::move(parsed);
      else if (field == "github")
        dependency->github = std::move(parsed);
      else if (field == "package")
        dependency->package = std::move(parsed);
      else if (field == "component")
        dependency->component = std::move(parsed);
      else if (field == "variant")
        dependency->variant = std::move(parsed);
      else if (field == "version")
        dependency->version = std::move(parsed);
      else if (field == "target")
        dependency->target = std::move(parsed);
      else if (field == "url")
        dependency->url = std::move(parsed);
      else
        dependency->sha256 = std::move(parsed);
    }

    if (!store_dependency() || format == 0)
    {
      if (format == 0)
        error << "forge: forge.lock.toml has an unsupported or missing format\n";

      return false;
    }

    return true;
  }

  bool DependencyLock::write(const std::filesystem::path& project_directory,
                             std::ostream& error)
  {
    if (!dirty)
      return true;

    const auto lock_path = project_directory / "forge.lock.toml";
    const auto temporary_path = project_directory / "forge.lock.toml.tmp";
    std::ofstream lock { temporary_path };

    if (!lock)
    {
      error << "forge: could not write forge.lock.toml\n";
      return false;
    }

    lock << "format = 2\n";

    for (const auto& entry : entries)
    {
      const auto& dependency = entry.second;
      lock
        << "\n[[dependency]]\n"
        << "name = \"" << dependency.name << "\"\n"
        << "github = \"" << dependency.github << "\"\n"
        << "package = \"" << dependency.package << "\"\n";

      if (!dependency.component.empty())
        lock << "component = \"" << dependency.component << "\"\n";

      if (!dependency.variant.empty())
        lock << "variant = \"" << dependency.variant << "\"\n";

      lock
        << "version = \"" << dependency.version << "\"\n"
        << "target = \"" << dependency.target << "\"\n"
        << "url = \"" << dependency.url << "\"\n"
        << "sha256 = \"" << dependency.sha256 << "\"\n";
    }

    if (!lock)
    {
      error << "forge: could not write forge.lock.toml\n";
      return false;
    }

    lock.close();
    const auto backup_path = project_directory / "forge.lock.toml.bak";
    std::error_code filesystem_error;
    std::filesystem::remove(backup_path, filesystem_error);
    filesystem_error.clear();

    if (std::filesystem::is_regular_file(lock_path))
    {
      std::filesystem::rename(lock_path, backup_path, filesystem_error);

      if (filesystem_error)
      {
        error << "forge: could not replace forge.lock.toml\n";
        return false;
      }
    }

    std::filesystem::rename(temporary_path, lock_path, filesystem_error);

    if (filesystem_error)
    {
      filesystem_error.clear();
      std::filesystem::rename(backup_path, lock_path, filesystem_error);
      error << "forge: could not replace forge.lock.toml\n";
      return false;
    }

    std::filesystem::remove(backup_path, filesystem_error);
    return true;
  }

} // namespace forge
