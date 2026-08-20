#pragma once

#include "box.h"
#include "dependency_lock.h"
#include "recipe.h"

#include <filesystem>
#include <map>
#include <optional>
#include <set>
#include <string>

namespace forge
{

  struct DependencyNode
  {
    std::filesystem::path directory;
    Recipe recipe;
    std::optional<std::string> target;
    std::optional<std::string> profile;
    std::optional<std::string> system_profile;
    std::string configuration;
    std::filesystem::path box;
    std::optional<BoxMetadata> box_metadata;
  };

  struct DependencyResolver
  {
    std::map<std::filesystem::path, DependencyNode> nodes;
    std::map<std::string, std::filesystem::path> names;
    std::set<std::filesystem::path> active_projects;
    DependencyLock lock;
    std::filesystem::path root_project;
    BuildOptions options;
    bool profile_is_legacy = false;
    bool update_dependency_found = false;
  };

} // namespace forge
