#pragma once

#include <filesystem>
#include <iosfwd>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace forge
{

  inline constexpr std::string_view recipe_schema_url =
    "https://raw.githubusercontent.com/razterizer/forge/main/schemas/forge.recipe.schema.json";

  struct Dependency
  {
    std::string name;
    std::filesystem::path path;
  };

  struct Recipe
  {
    std::string name;
    std::string version;
    std::string type;
    int cpp_standard = 0;
    std::optional<int> build_number;
    std::vector<std::filesystem::path> sources;
    std::vector<std::filesystem::path> public_headers;
    std::vector<Dependency> dependencies;
  };

  bool read_recipe(const std::filesystem::path& path,
                   Recipe& recipe,
                   std::ostream& error);

} // namespace forge
