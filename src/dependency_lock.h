#pragma once

#include <filesystem>
#include <iosfwd>
#include <map>
#include <string>
#include <string_view>

namespace forge
{

  struct LockedDependency
  {
    std::string name;
    std::string github;
    std::string package;
    std::string component;
    std::string variant;
    std::string version;
    std::string target;
    std::string url;
    std::string sha256;
  };

  class DependencyLock
  {
  public:
    std::map<std::string, LockedDependency> entries;
    bool dirty = false;

    bool load(const std::filesystem::path& project_directory, std::ostream& error);
    bool write(const std::filesystem::path& project_directory, std::ostream& error);

    static std::string key(std::string_view name,
                           std::string_view variant,
                           std::string_view target);

  private:
    bool loaded_ = false;
  };

} // namespace forge
