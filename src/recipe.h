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
    // Named system-provider capabilities supplied by this dependency. When
    // selected, these replace the matching package-manager requirements.
    std::vector<std::string> provides;
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
    int c_standard = 0;
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
    std::vector<std::string> macos_brew_packages;
    std::vector<std::string> linux_libraries;
    std::vector<std::string> linux_apt_packages;
    std::vector<std::string> windows_libraries;
    bool test = false;
  };

  struct BuildProfile
  {
    std::string configuration;
    int cpp_standard = 0;
    int c_standard = 0;
    std::vector<std::filesystem::path> include_directories;
    std::vector<std::filesystem::path> macos_system_include_directories;
    std::vector<std::filesystem::path> linux_system_include_directories;
    std::vector<std::filesystem::path> windows_system_include_directories;
    std::vector<std::filesystem::path> macos_system_library_directories;
    std::vector<std::filesystem::path> linux_system_library_directories;
    std::vector<std::filesystem::path> windows_system_library_directories;
    std::vector<std::string> compile_definitions;
  };

  using RecipeSelectors = std::map<std::string, std::string>;

  struct BuildRule
  {
    RecipeSelectors selectors;
    BuildProfile build;
  };

  struct DependencyRule
  {
    RecipeSelectors selectors;
    std::vector<Dependency> dependencies;
  };

  struct RecipeSelection
  {
    std::string style;
    std::string platform;
    std::string configuration;
    std::string profile;
  };

  enum class ProfileResolution
  {
    automatic,
    inherited_legacy,
    selectors_only
  };

  // The fully resolved inputs to a build.  Keeping this together prevents
  // commands from choosing a different dependency graph or configuration for
  // the same command-line selectors.
  struct EffectiveBuildSelection
  {
    std::optional<std::string> legacy_profile;
    RecipeSelection selectors;
    std::string configuration;
  };

  struct BuildSelectionRequest
  {
    std::optional<std::string> target;
    std::optional<std::string> profile;
    std::optional<std::string> system_profile;
    std::optional<std::string> style;
    std::string platform;
    std::optional<std::string> selector_configuration;
    std::string build_configuration = "Debug";
    ProfileResolution profile_resolution = ProfileResolution::automatic;
    bool require_profile = true;
    bool select_target = true;
    bool apply_build_profiles = true;
  };

  struct ReleaseVariant
  {
    std::string profile;
    std::string suffix;
  };

  struct PlatformReleaseFiles
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
    int c_standard = 0;
    std::optional<int> build_number;
    std::vector<std::filesystem::path> sources;
    std::vector<std::filesystem::path> public_headers;
    std::vector<std::filesystem::path> header_validation_headers;
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
    std::vector<std::string> macos_brew_packages;
    std::vector<std::string> linux_libraries;
    std::vector<std::string> linux_apt_packages;
    std::vector<std::string> windows_libraries;
    std::vector<ImportProfile> imports;
    std::vector<Dependency> dependencies;
    std::map<std::string, std::vector<Dependency>> dependency_profiles;
    std::map<std::string, BuildProfile> build_profiles;
    std::map<std::string, std::vector<Dependency>> system_dependency_profiles;
    std::map<std::string, BuildProfile> system_build_profiles;
    std::vector<BuildRule> build_rules;
    std::vector<DependencyRule> dependency_rules;
    std::optional<std::string> default_style;
    std::optional<std::string> default_profile;
    std::optional<std::string> default_target;
    std::vector<RuntimeFile> runtime_files;
    std::vector<std::filesystem::path> release_files;
    std::optional<std::string> release_bundle_name;
    std::vector<ReleaseVariant> release_variants;
    std::vector<ReleaseVariant> box_variants;
    PlatformReleaseFiles release_readme;
    PlatformReleaseFiles release_unblock;
    std::optional<std::string> release_notes_build_number_format;
    std::filesystem::path version_header_path;
    std::string version_header_prefix;
    std::vector<RecipeTarget> targets;
    std::vector<RecipeTarget> internal_targets;
    std::vector<std::string> selected_internal_dependencies;
    std::optional<std::string> selected_target;
  };

  // Returns the path a public header has from one of the package's public
  // include roots.  Packages conventionally use include/, but CMake interface
  // libraries such as EnTT legitimately publish headers directly from src/.
  inline std::optional<std::filesystem::path> public_header_include_path(
    const std::filesystem::path& header,
    const std::vector<std::filesystem::path>& include_directories)
  {
    const auto path_below = [&header](const std::filesystem::path& root)
      -> std::optional<std::filesystem::path>
      {
        const auto relative = header.lexically_relative(root);

        if (relative.empty()
            || relative == "."
            || relative.is_absolute()
            || relative.begin()->string() == "..")
        {
          return std::nullopt;
        }

        return relative;
      };

    if (const auto relative = path_below("include"))
      return relative;

    for (const auto& include_directory : include_directories)
    {
      if (const auto relative = path_below(include_directory))
        return relative;
    }

    return std::nullopt;
  }

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

  bool select_system_dependency_profile(Recipe& recipe,
                                        const std::optional<std::string>& profile,
                                        bool required,
                                        std::ostream& error);

  bool select_build_profile(Recipe& recipe,
                            const std::optional<std::string>& profile,
                            bool required,
                            std::string& configuration,
                            std::ostream& error);

  bool select_system_build_profile(Recipe& recipe,
                                   const std::optional<std::string>& profile,
                                   bool required,
                                   std::string& configuration,
                                   std::ostream& error);

  bool apply_selector_rules(Recipe& recipe,
                            const RecipeSelection& selection,
                            std::string& configuration,
                            std::ostream& error);

  RecipeSelection resolve_recipe_selection(
    const Recipe& recipe,
    const std::optional<std::string>& style,
    std::string platform,
    const std::optional<std::string>& configuration,
    const std::optional<std::string>& profile
  );

  bool resolve_effective_build_selection(
    Recipe& recipe,
    const BuildSelectionRequest& request,
    EffectiveBuildSelection& selection,
    std::ostream& error
  );

  bool is_valid_compile_definition(std::string_view definition);

} // namespace forge
