#pragma once

#include <filesystem>
#include <iosfwd>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace forge
{

  inline constexpr std::string_view recipe_schema_url =
    "https://raw.githubusercontent.com/razterizer/forge/main/schemas/forge.recipe.schema.json";
  inline constexpr std::string_view workflow_release_profile = "workflow-release";

  struct Dependency
  {
    std::string name;
    std::filesystem::path path;
    std::filesystem::path box;
    std::string url;
    std::string sha256;
    std::string github;
    std::string package;
    std::string version;
    std::string git;
    std::string commit;
    std::string type;
    std::string component;
    std::string variant;
    std::string resolved_target;
    std::vector<std::string> targets;
  };

  struct ImportProfile
  {
    std::string target;
    std::string compiler;
    std::string compiler_version;
    std::string configuration;
    std::string runtime;
    int cpp_standard = 0;
    std::vector<std::filesystem::path> public_headers;
    std::vector<std::filesystem::path> static_libraries;
    std::vector<std::filesystem::path> dynamic_libraries;
    std::vector<std::filesystem::path> import_libraries;
  };

  struct RuntimeFile
  {
    std::filesystem::path source;
    std::filesystem::path destination;
  };

  struct RecipeTarget
  {
    std::string name;
    std::string type;
    int cpp_standard = 0;
    std::vector<std::filesystem::path> sources;
    std::vector<std::filesystem::path> public_headers;
    std::vector<std::filesystem::path> include_directories;
    std::vector<std::filesystem::path> macos_system_include_directories;
    std::vector<std::filesystem::path> linux_system_include_directories;
    std::vector<std::filesystem::path> windows_system_include_directories;
    std::vector<std::filesystem::path> macos_system_library_directories;
    std::vector<std::filesystem::path> linux_system_library_directories;
    std::vector<std::filesystem::path> windows_system_library_directories;
    std::vector<std::string> compile_definitions;
    std::vector<RuntimeFile> runtime_files;
    std::vector<std::string> dependencies;
    std::vector<std::string> macos_frameworks;
    std::vector<std::string> macos_libraries;
    std::vector<std::string> linux_libraries;
    std::vector<std::string> windows_libraries;
    bool test = false;
  };

  struct BuildProfile
  {
    std::string configuration;
    int cpp_standard = 0;
    std::vector<std::filesystem::path> include_directories;
    std::vector<std::filesystem::path> macos_system_include_directories;
    std::vector<std::filesystem::path> linux_system_include_directories;
    std::vector<std::filesystem::path> windows_system_include_directories;
    std::vector<std::filesystem::path> macos_system_library_directories;
    std::vector<std::filesystem::path> linux_system_library_directories;
    std::vector<std::filesystem::path> windows_system_library_directories;
    std::vector<std::string> compile_definitions;
  };

  struct ReleaseVariant
  {
    std::string profile;
    std::string suffix;
  };

  struct ReleaseReadme
  {
    std::filesystem::path linux_path;
    std::filesystem::path macos_path;
    std::filesystem::path windows_path;
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
    std::vector<std::filesystem::path> include_directories;
    std::vector<std::filesystem::path> macos_system_include_directories;
    std::vector<std::filesystem::path> linux_system_include_directories;
    std::vector<std::filesystem::path> windows_system_include_directories;
    std::vector<std::filesystem::path> macos_system_library_directories;
    std::vector<std::filesystem::path> linux_system_library_directories;
    std::vector<std::filesystem::path> windows_system_library_directories;
    std::vector<std::string> compile_definitions;
    std::vector<std::string> macos_frameworks;
    std::vector<std::string> macos_libraries;
    std::vector<std::string> linux_libraries;
    std::vector<std::string> windows_libraries;
    std::vector<ImportProfile> imports;
    std::vector<Dependency> dependencies;
    std::map<std::string, std::vector<Dependency>> dependency_profiles;
    std::map<std::string, BuildProfile> build_profiles;
    std::vector<RuntimeFile> runtime_files;
    std::vector<std::filesystem::path> release_files;
    std::optional<std::string> release_bundle_name;
    std::vector<ReleaseVariant> release_variants;
    std::vector<ReleaseVariant> box_variants;
    ReleaseReadme release_readme;
    std::optional<std::string> release_notes_build_number_format;
    std::filesystem::path version_header_path;
    std::string version_header_prefix;
    std::vector<RecipeTarget> targets;
    std::vector<RecipeTarget> internal_targets;
    std::vector<std::string> selected_internal_dependencies;
    std::optional<std::string> selected_target;
  };

  bool read_recipe(const std::filesystem::path& path,
                   Recipe& recipe,
                   std::ostream& error);

  bool select_recipe_target(Recipe& recipe,
                            const std::optional<std::string>& target,
                            std::ostream& error);

  bool select_dependency_profile(Recipe& recipe,
                                 const std::optional<std::string>& profile,
                                 bool required,
                                 std::ostream& error);

  bool select_build_profile(Recipe& recipe,
                            const std::optional<std::string>& profile,
                            bool required,
                            std::string& configuration,
                            std::ostream& error);

  bool is_valid_compile_definition(std::string_view definition);

} // namespace forge
