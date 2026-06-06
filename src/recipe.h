#pragma once

#include <filesystem>
#include <iosfwd>
#include <optional>
#include <string>
#include <vector>

namespace forge
{

  struct Recipe
  {
    std::string name;
    std::string version;
    std::string type;
    int cpp_standard = 0;
    std::optional<int> build_number;
    std::vector<std::filesystem::path> sources;
    std::vector<std::filesystem::path> public_headers;
  };

  bool read_recipe(const std::filesystem::path& path,
                   Recipe& recipe,
                   std::ostream& error);

} // namespace forge
