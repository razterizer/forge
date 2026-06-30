#include "cli.h"
#include "box.h"
#include "bump.h"
#include "build.h"
#include "clean.h"
#include "cli_support.h"
#include "fprocess.h"
#include "github.h"
#include "init.h"
#include "new.h"
#include "recipe.h"
#include "release.h"
#include "run.h"
#include "test.h"
#include "target_support.h"
#include "workspace.h"

#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <ostream>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace forge::cli
{
  namespace
  {

    std::string_view trim_cli(std::string_view value)
    {
      const auto first = value.find_first_not_of(" \t\r\n");

      if (first == std::string_view::npos)
        return {};

      const auto last = value.find_last_not_of(" \t\r\n");
      return value.substr(first, last - first + 1);
    }

    bool parse_cli_string(std::string_view value, std::string& result)
    {
      value = trim_cli(value);

      if (value.size() < 2 || value.front() != '"' || value.back() != '"')
        return false;

      result = std::string { value.substr(1, value.size() - 2) };
      return result.find('"') == std::string::npos;
    }

    void print_unknown_option(std::string_view argument,
                              std::ostream& error)
    {
      error << "forge: unknown option '" << argument << "'";

      if (argument == "--all-target")
        error << "; did you mean '--all-targets'?";
      else if (argument == "--release-target")
        error << "; did you mean '--release-targets'?";
      else if (argument == "--all-profile")
        error << "; did you mean '--all-profiles'?";

      error << '\n';
    }

    std::string selected_dependency_scope(const std::optional<std::string>& profile)
    {
      return profile
        ? "profile '" + *profile + "'"
        : "default dependencies";
    }

    bool selected_github_dependency_names(const std::filesystem::path& project_directory,
                                          const std::optional<std::string>& profile,
                                          const std::optional<std::string>& dependency,
                                          std::set<std::string>& names,
                                          std::ostream& error)
    {
      Recipe recipe;

      if (!read_recipe(project_directory / "forge.recipe.toml", recipe, error)
          || !select_dependency_profile(recipe, profile, true, error))
      {
        return false;
      }

      bool selected_non_github_dependency = false;

      for (const auto& candidate : recipe.dependencies)
      {
        if (dependency && candidate.name == *dependency && candidate.github.empty())
          selected_non_github_dependency = true;

        if (candidate.github.empty())
          continue;

        if (dependency && candidate.name != *dependency)
          continue;

        names.insert(candidate.name);
      }

      if (dependency && selected_non_github_dependency && names.empty())
      {
        error << "forge: dependency '" << *dependency << "' in "
              << selected_dependency_scope(profile)
              << " is not a GitHub dependency\n";
        return false;
      }

      return true;
    }

    bool dependencies_contain_selected_github(
      const std::vector<Dependency>& dependencies,
      const std::optional<std::string>& dependency
    )
    {
      return std::any_of(
        dependencies.begin(),
        dependencies.end(),
        [&dependency](const Dependency& candidate)
        {
          return !candidate.github.empty()
            && (!dependency || candidate.name == *dependency);
        }
      );
    }

    bool collect_update_profiles(const std::filesystem::path& project_directory,
                                 const BuildOptions& options,
                                 std::vector<std::optional<std::string>>& profiles,
                                 std::ostream& error)
    {
      Recipe recipe;

      if (!read_recipe(project_directory / "forge.recipe.toml", recipe, error))
        return false;

      if (dependencies_contain_selected_github(recipe.dependencies, options.update_dependency))
        profiles.push_back(std::nullopt);

      for (const auto& [name, dependencies] : recipe.dependency_profiles)
      {
        if (dependencies_contain_selected_github(dependencies, options.update_dependency))
          profiles.push_back(name);
      }

      if (profiles.empty() && !options.update_dependency)
        profiles.push_back(std::nullopt);

      if (profiles.empty())
      {
        error << "forge: GitHub dependency '" << *options.update_dependency
              << "' was not found in any dependency profile\n";
        return false;
      }

      return true;
    }

    bool store_locked_target(const std::set<std::string>& dependency_names,
                             const std::string& name,
                             const std::string& target,
                             std::set<std::string>& targets)
    {
      if (name.empty() || target.empty() || target == "any")
        return true;

      if (!dependency_names.empty() && !dependency_names.contains(name))
        return true;

      if (!is_supported_dependency_target(target))
        return false;

      targets.insert(target);
      return true;
    }

    bool collect_locked_update_targets(const std::filesystem::path& project_directory,
                                       const std::set<std::string>& dependency_names,
                                       std::set<std::string>& targets,
                                       std::ostream& error)
    {
      const auto path = project_directory / "forge.lock.toml";

      if (!std::filesystem::is_regular_file(path))
        return true;

      std::ifstream file { path };

      if (!file)
      {
        error << "forge: could not read forge.lock.toml\n";
        return false;
      }

      std::string line;
      std::string name;
      std::string target;

      while (std::getline(file, line))
      {
        const auto content = trim_cli(line);

        if (content.empty() || content.front() == '#')
          continue;

        if (content == "[[dependency]]")
        {
          if (!store_locked_target(dependency_names, name, target, targets))
          {
            error << "forge: forge.lock.toml contains an unsupported dependency target\n";
            return false;
          }

          name.clear();
          target.clear();
          continue;
        }

        const auto equals = content.find('=');

        if (equals == std::string_view::npos)
          continue;

        const auto key = trim_cli(content.substr(0, equals));
        const auto value = trim_cli(content.substr(equals + 1));
        std::string parsed;

        if (key == "name")
        {
          if (parse_cli_string(value, parsed))
            name = std::move(parsed);
        }
        else if (key == "target")
        {
          if (parse_cli_string(value, parsed))
            target = std::move(parsed);
        }
      }

      if (!store_locked_target(dependency_names, name, target, targets))
      {
        error << "forge: forge.lock.toml contains an unsupported dependency target\n";
        return false;
      }

      return true;
    }

    bool collect_update_targets(const std::filesystem::path& project_directory,
                                const BuildOptions& options,
                                bool release_targets,
                                std::vector<std::string>& targets,
                                std::ostream& error)
    {
      std::set<std::string> dependency_names;

      if (!selected_github_dependency_names(
        project_directory,
        options.profile,
        options.update_dependency,
        dependency_names,
        error
      ))
      {
        return false;
      }

      if (options.update_dependency && dependency_names.empty())
      {
        error << "forge: GitHub dependency '" << *options.update_dependency
              << "' was not found\n";
        return false;
      }

      if (release_targets)
      {
        targets = release_dependency_targets();
        return true;
      }

      std::set<std::string> locked_targets;

      if (!collect_locked_update_targets(
        project_directory,
        dependency_names,
        locked_targets,
        error
      ))
      {
        return false;
      }

      if (locked_targets.empty())
        locked_targets.insert(current_target());

      targets.assign(locked_targets.begin(), locked_targets.end());
      return true;
    }

    bool validate_named_update_dependency(const std::filesystem::path& project_directory,
                                          const BuildOptions& options,
                                          std::ostream& error)
    {
      if (!options.update_dependency)
        return true;

      std::set<std::string> dependency_names;

      if (!selected_github_dependency_names(
        project_directory,
        options.profile,
        options.update_dependency,
        dependency_names,
        error
      ))
      {
        return false;
      }

      if (dependency_names.empty())
      {
        error << "forge: GitHub dependency '" << *options.update_dependency
              << "' was not found\n";
        return false;
      }

      return true;
    }

    int run_dependency_update(const std::filesystem::path& working_directory,
                              const BuildOptions& options,
                              bool all_targets,
                              bool release_targets,
                              bool all_profiles,
                              std::ostream& output,
                              std::ostream& error)
    {
      const auto target_selector_count =
        (options.update_target ? 1 : 0)
        + (all_targets ? 1 : 0)
        + (release_targets ? 1 : 0);

      if (target_selector_count > 1)
      {
        print_update_usage(error);
        return 2;
      }

      if (all_profiles && options.profile)
      {
        print_update_usage(error);
        return 2;
      }

      if (!all_profiles)
      {
        if (!all_targets && !release_targets)
        {
          if (!validate_named_update_dependency(working_directory, options, error))
            return 2;

          return build_project(working_directory, options, output, error);
        }

        std::vector<std::string> targets;

        if (!collect_update_targets(working_directory, options, release_targets, targets, error))
          return 2;

        for (const auto& target : targets)
        {
          auto target_options = options;
          target_options.update_target = target;
          const auto result = build_project(working_directory, target_options, output, error);

          if (result != 0)
            return result;
        }

        return 0;
      }

      std::vector<std::optional<std::string>> profiles;

      if (!collect_update_profiles(working_directory, options, profiles, error))
        return 2;

      for (const auto& profile : profiles)
      {
        auto profile_options = options;
        profile_options.profile = profile;
        const auto result = run_dependency_update(
          working_directory,
          profile_options,
          all_targets,
          release_targets,
          false,
          output,
          error
        );

        if (result != 0)
          return result;
      }

      return 0;
    }

    bool read_text_file(const std::filesystem::path& path,
                        std::string& content,
                        std::ostream& error)
    {
      std::ifstream file { path };

      if (!file)
      {
        error << "forge: could not read '" << path.string() << "'\n";
        return false;
      }

      content = {
        std::istreambuf_iterator<char> { file },
        std::istreambuf_iterator<char> {}
      };
      return true;
    }

    bool write_text_file(const std::filesystem::path& path,
                         std::string_view content,
                         std::ostream& error)
    {
      const auto temporary = path.string() + ".tmp";
      std::ofstream file { temporary };

      if (!file)
      {
        error << "forge: could not write '" << path.string() << "'\n";
        return false;
      }

      file << content;

      if (!file)
      {
        error << "forge: could not write '" << path.string() << "'\n";
        return false;
      }

      file.close();
      std::error_code filesystem_error;
      const auto backup = path.string() + ".bak";
      std::filesystem::remove(backup, filesystem_error);
      filesystem_error.clear();

      if (std::filesystem::is_regular_file(path))
      {
        std::filesystem::rename(path, backup, filesystem_error);

        if (filesystem_error)
        {
          std::filesystem::remove(temporary, filesystem_error);
          error << "forge: could not replace '" << path.string() << "'\n";
          return false;
        }
      }

      std::filesystem::rename(temporary, path, filesystem_error);

      if (filesystem_error)
      {
        filesystem_error.clear();
        std::filesystem::rename(backup, path, filesystem_error);
        std::filesystem::remove(temporary, filesystem_error);
        error << "forge: could not replace '" << path.string() << "'\n";
        return false;
      }

      std::filesystem::remove(backup, filesystem_error);
      return true;
    }

    std::string dependency_section(const std::optional<std::string>& profile)
    {
      return profile
        ? "profile." + *profile + ".dependencies"
        : "dependencies";
    }

    bool replace_dependency_version_line(std::string_view line,
                                         std::string_view version,
                                         std::string& updated)
    {
      const auto version_key = line.find("version");

      if (version_key == std::string_view::npos)
        return false;

      const auto equals = line.find('=', version_key);

      if (equals == std::string_view::npos)
        return false;

      const auto open_quote = line.find('"', equals);

      if (open_quote == std::string_view::npos)
        return false;

      const auto close_quote = line.find('"', open_quote + 1);

      if (close_quote == std::string_view::npos)
        return false;

      updated.assign(line.substr(0, open_quote + 1));
      updated += version;
      updated += line.substr(close_quote);
      return true;
    }

    bool replace_dependency_version(std::string_view content,
                                    const std::optional<std::string>& profile,
                                    std::string_view dependency,
                                    std::string_view version,
                                    std::string& updated,
                                    std::ostream& error)
    {
      updated.clear();
      std::string section;
      const auto wanted_section = dependency_section(profile);
      bool replaced = false;
      std::string_view remaining = content;

      while (!remaining.empty())
      {
        const auto newline = remaining.find('\n');
        const auto line = remaining.substr(0, newline);
        const auto trimmed = trim_cli(line);

        if (trimmed.starts_with("[") && trimmed.ends_with("]"))
          section = std::string { trim_cli(trimmed.substr(1, trimmed.size() - 2)) };

        const auto equals = trimmed.find('=');
        const auto key = equals == std::string_view::npos
          ? std::string_view {}
          : trim_cli(trimmed.substr(0, equals));

        if (!replaced && section == wanted_section && key == dependency)
        {
          std::string replacement;

          if (!replace_dependency_version_line(line, version, replacement))
          {
            error << "forge: dependency '" << dependency
                  << "' has no inline version to upgrade\n";
            return false;
          }

          updated += replacement;
          replaced = true;
        }
        else
          updated += line;

        if (newline == std::string_view::npos)
          break;

        updated += '\n';
        remaining.remove_prefix(newline + 1);
      }

      if (!replaced)
      {
        error << "forge: dependency '" << dependency << "' was not found in ["
              << wanted_section << "]\n";
        return false;
      }

      return true;
    }

    bool validate_upgrade_dependency(const std::filesystem::path& project_directory,
                                     const std::optional<std::string>& profile,
                                     std::string_view dependency,
                                     std::ostream& error)
    {
      std::set<std::string> names;

      if (!selected_github_dependency_names(
        project_directory,
        profile,
        std::string { dependency },
        names,
        error
      ))
      {
        return false;
      }

      if (names.empty())
      {
        error << "forge: GitHub dependency '" << dependency << "' was not found\n";
        return false;
      }

      return true;
    }

    bool is_safe_url_component(std::string_view value)
    {
      return !value.empty()
        && value != "."
        && value != ".."
        && value.find_first_not_of(
          "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789._+-"
        ) == std::string_view::npos;
    }

    bool is_numeric_version_identifier(std::string_view value)
    {
      return !value.empty()
        && value.find_first_not_of("0123456789") == std::string_view::npos
        && (value.size() == 1 || value.front() != '0');
    }

    bool is_github_dependency_version(std::string_view value)
    {
      const auto build = value.find("+build.");

      if (build != std::string_view::npos)
      {
        if (!is_numeric_version_identifier(value.substr(build + std::string_view { "+build." }.size())))
          return false;

        value = value.substr(0, build);
      }

      const auto prerelease = value.find('-');
      const auto core = value.substr(0, prerelease);
      std::size_t offset = 0;

      for (int component = 0; component < 3; ++component)
      {
        const auto separator = core.find('.', offset);
        const auto end = component == 2 ? core.size() : separator;

        if ((component != 2 && separator == std::string_view::npos)
            || !is_numeric_version_identifier(core.substr(offset, end - offset)))
        {
          return false;
        }

        offset = end + 1;
      }

      return prerelease == std::string_view::npos
        || is_safe_url_component(value.substr(prerelease + 1));
    }

    bool upgrade_recipe_dependency(const std::filesystem::path& project_directory,
                                   const std::optional<std::string>& profile,
                                   std::string_view dependency,
                                   std::string_view version,
                                   std::ostream& error)
    {
      if (version.empty() || version.find('"') != std::string_view::npos)
      {
        error << "forge: upgrade version cannot be empty\n";
        return false;
      }

      if (!is_github_dependency_version(version))
      {
        error << "forge: upgrade version must use <major>.<minor>.<patch>[+build.<number>]\n";
        return false;
      }

      if (!validate_upgrade_dependency(project_directory, profile, dependency, error))
        return false;

      const auto path = project_directory / "forge.recipe.toml";
      std::string content;
      std::string updated;

      return read_text_file(path, content, error)
        && replace_dependency_version(content, profile, dependency, version, updated, error)
        && write_text_file(path, updated, error);
    }

    bool upgrade_recipe_dependency_profiles(
      const std::filesystem::path& project_directory,
      const std::vector<std::optional<std::string>>& profiles,
      std::string_view dependency,
      std::string_view version,
      std::ostream& error
    )
    {
      if (version.empty() || version.find('"') != std::string_view::npos)
      {
        error << "forge: upgrade version cannot be empty\n";
        return false;
      }

      if (!is_github_dependency_version(version))
      {
        error << "forge: upgrade version must use <major>.<minor>.<patch>[+build.<number>]\n";
        return false;
      }

      const auto path = project_directory / "forge.recipe.toml";
      std::string content;

      if (!read_text_file(path, content, error))
        return false;

      for (const auto& profile : profiles)
      {
        std::string updated;

        if (!replace_dependency_version(content, profile, dependency, version, updated, error))
          return false;

        content = std::move(updated);
      }

      return write_text_file(path, content, error);
    }

    bool find_recipe_github_dependency(const std::filesystem::path& project_directory,
                                       const std::optional<std::string>& profile,
                                       std::string_view dependency,
                                       Dependency& result,
                                       std::ostream& error)
    {
      Recipe recipe;

      if (!read_recipe(project_directory / "forge.recipe.toml", recipe, error)
          || !select_dependency_profile(recipe, profile, true, error))
      {
        return false;
      }

      for (const auto& candidate : recipe.dependencies)
      {
        if (candidate.name == dependency && !candidate.github.empty())
        {
          result = candidate;
          return true;
        }
      }

      for (const auto& candidate : recipe.dependencies)
      {
        if (candidate.name == dependency)
        {
          error << "forge: dependency '" << dependency << "' in "
                << selected_dependency_scope(profile)
                << " is not a GitHub dependency\n";
          return false;
        }
      }

      error << "forge: GitHub dependency '" << dependency << "' was not found\n";
      return false;
    }

    bool download_latest_release_json(const std::filesystem::path& project_directory,
                                      const Dependency& dependency,
                                      std::filesystem::path& destination,
                                      std::ostream& error)
    {
      const auto cache_directory =
        project_directory / ".forge" / "cache" / "github"
          / std::filesystem::path { dependency.github };
      std::error_code filesystem_error;
      std::filesystem::create_directories(cache_directory, filesystem_error);

      if (filesystem_error)
      {
        error << "forge: could not create the GitHub release cache\n";
        return false;
      }

      destination = cache_directory / "latest-release.json";
      auto status_path = destination;
      status_path += ".status";
      const auto script = cache_directory / "latest-release.cmake";
      std::ofstream file { script };

      if (!file)
      {
        error << "forge: could not create the GitHub release query script\n";
        return false;
      }

      file
        << "file(DOWNLOAD \"${URL}\" \"${DESTINATION}.tmp\" STATUS status TLS_VERIFY ON)\n"
        << "list(GET status 0 code)\n"
        << "file(WRITE \"${STATUS_FILE}\" \"${code}\")\n"
        << "if(NOT code EQUAL 0)\n"
        << "  file(REMOVE \"${DESTINATION}.tmp\")\n"
        << "  return()\n"
        << "endif()\n"
        << "file(REMOVE \"${DESTINATION}\")\n"
        << "file(RENAME \"${DESTINATION}.tmp\" \"${DESTINATION}\")\n";
      file.close();

      const auto url = "https://api.github.com/repos/" + dependency.github + "/releases/latest";
      std::filesystem::remove(status_path, filesystem_error);
      const auto result = run_process(
        {
          "cmake",
          "-DURL=" + url,
          "-DDESTINATION=" + destination.generic_string(),
          "-DSTATUS_FILE=" + status_path.generic_string(),
          "-P",
          script.string()
        },
        project_directory,
        error
      );

      if (result != 0)
        return false;

      std::ifstream status_file { status_path };
      int status = 0;

      if (!(status_file >> status))
        return false;

      std::filesystem::remove(status_path, filesystem_error);
      return status == 0;
    }

    bool json_string_value(std::string_view json,
                           std::string_view key,
                           std::string& value)
    {
      const auto key_pattern = "\"" + std::string { key } + "\"";
      const auto key_position = json.find(key_pattern);

      if (key_position == std::string_view::npos)
        return false;

      const auto colon = json.find(':', key_position + key_pattern.size());

      if (colon == std::string_view::npos)
        return false;

      const auto open_quote = json.find('"', colon + 1);

      if (open_quote == std::string_view::npos)
        return false;

      value.clear();

      for (auto index = open_quote + 1; index < json.size(); ++index)
      {
        const auto character = json[index];

        if (character == '"')
          return true;

        if (character == '\\')
        {
          if (index + 1 >= json.size())
            return false;

          value += json[++index];
        }
        else
          value += character;
      }

      return false;
    }

    bool is_numeric_version_component(std::string_view value)
    {
      return !value.empty()
        && value.find_first_not_of("0123456789") == std::string_view::npos;
    }

    std::optional<std::string> latest_tag_to_dependency_version(std::string_view tag)
    {
      if (tag.starts_with("release-"))
        tag.remove_prefix(std::string_view { "release-" }.size());

      if (tag.starts_with("v"))
        tag.remove_prefix(1);

      std::array<std::string_view, 4> components {};
      std::size_t component_count = 0;
      std::size_t offset = 0;
      bool consumed_tag = false;

      while (offset <= tag.size() && component_count < components.size())
      {
        const auto separator = tag.find('.', offset);
        const auto end = separator == std::string_view::npos ? tag.size() : separator;
        components[component_count++] = tag.substr(offset, end - offset);

        if (separator == std::string_view::npos)
        {
          consumed_tag = true;
          break;
        }

        offset = separator + 1;
      }

      if (component_count == 4
          && consumed_tag
          && std::all_of(
            components.begin(),
            components.end(),
            [](std::string_view component)
            {
              return is_numeric_version_component(component);
            }
          ))
      {
        return std::string { components[0] } + "."
          + std::string { components[1] } + "."
          + std::string { components[2] } + "+build."
          + std::string { components[3] };
      }

      return std::string { tag };
    }

    bool latest_dependency_version(const std::filesystem::path& project_directory,
                                   const std::optional<std::string>& profile,
                                   std::string_view dependency_name,
                                   std::string& version,
                                   std::ostream& output,
                                   std::ostream& error)
    {
      Dependency dependency;

      if (!find_recipe_github_dependency(project_directory, profile, dependency_name, dependency, error))
        return false;

      std::filesystem::path latest_json;
      output << "Resolving latest GitHub release for " << dependency.name << '\n';

      if (!download_latest_release_json(project_directory, dependency, latest_json, error))
      {
        error << "forge: could not resolve latest GitHub release for '"
              << dependency.name << "'\n";
        return false;
      }

      std::string content;

      if (!read_text_file(latest_json, content, error))
        return false;

      std::string tag;

      if (!json_string_value(content, "tag_name", tag))
      {
        error << "forge: latest GitHub release for '" << dependency.name
              << "' did not include a tag_name\n";
        return false;
      }

      const auto resolved = latest_tag_to_dependency_version(tag);

      if (!resolved || resolved->empty())
      {
        error << "forge: latest GitHub release for '" << dependency.name
              << "' has an invalid tag\n";
        return false;
      }

      version = *resolved;
      return true;
    }

    int list_profiles(const std::filesystem::path& project_directory,
                      std::ostream& output,
                      std::ostream& error)
    {
      Recipe recipe;

      if (!read_recipe(project_directory / "forge.recipe.toml", recipe, error))
        return 2;

      std::set<std::string> profiles;

      for (const auto& [name, _] : recipe.dependency_profiles)
        profiles.insert(name);

      for (const auto& [name, _] : recipe.build_profiles)
        profiles.insert(name);

      for (const auto& variant : recipe.release_variants)
        profiles.insert(variant.profile);

      for (const auto& variant : recipe.box_variants)
        profiles.insert(variant.profile);

      if (profiles.empty())
      {
        output << "No profiles declared\n";
        return 0;
      }

      std::size_t profile_width = std::string_view { "Profile" }.size();

      for (const auto& profile : profiles)
        profile_width = std::max(profile_width, profile.size());

      output << "Profiles:\n";
      output << "  Profile" << std::string(profile_width - std::string_view { "Profile" }.size(), ' ')
             << "  Roles\n";
      output << "  " << std::string(profile_width, '-') << "  -----\n";

      for (const auto& profile : profiles)
      {
        std::vector<std::string> roles;

        const auto add_role =
          [&roles](std::string role)
          {
            roles.push_back(std::move(role));
          };

        if (recipe.dependency_profiles.contains(profile))
          add_role("dependencies");

        if (recipe.build_profiles.contains(profile))
          add_role("build");

        for (const auto& variant : recipe.release_variants)
        {
          if (variant.profile == profile)
            add_role("release variant '" + variant.suffix + "'");
        }

        for (const auto& variant : recipe.box_variants)
        {
          if (variant.profile == profile)
            add_role("box variant '" + variant.suffix + "'");
        }

        output << "  " << profile << std::string(profile_width - profile.size(), ' ') << "  ";

        for (std::size_t index = 0; index < roles.size(); ++index)
        {
          if (index > 0)
            output << ", ";

          output << roles[index];
        }

        output << '\n';
      }

      return 0;
    }

  } // namespace

  int run(std::span<const std::string_view> arguments,
          std::ostream& output,
          std::ostream& error)
  {
    std::error_code filesystem_error;
    const auto working_directory = std::filesystem::current_path(filesystem_error);

    if (filesystem_error)
    {
      error << "forge: could not determine the current directory\n";
      return 2;
    }

    return run(arguments, working_directory, output, error);
  }

  int run(std::span<const std::string_view> arguments,
          const std::filesystem::path& working_directory,
          std::ostream& output,
          std::ostream& error)
  {
    if (arguments.empty() || arguments.front() == "--help" || arguments.front() == "-h")
    {
      print_help(output);
      return 0;
    }

    if (arguments.front() == "--version")
    {
      output << "forge " << version << '\n';
      return 0;
    }

    if (!is_command(arguments.front()))
    {
      error << "forge: unknown command '" << arguments.front() << "'\n";
      return 2;
    }

    if ((arguments.size() == 2
         && (arguments[1] == "--help" || arguments[1] == "-h"))
        || ((arguments.front() == "box"
             || arguments.front() == "profile"
             || arguments.front() == "workflow")
            && (arguments.back() == "--help" || arguments.back() == "-h")))
    {
      print_command_help(arguments.front(), output);
      return 0;
    }

    if (arguments.front() == "new")
    {
      NewOptions options;
      std::optional<std::string_view> name;

      for (const auto argument : arguments.subspan(1))
      {
        if (const auto value = option_value(argument, "--init-version=");
            value && set_once(options.initial_version, *value))
        {
        }
        else if (const auto value = option_value(argument, "--version-header-path=");
                 value && set_once(options.version_header_path, *value))
        {
        }
        else if (!argument.starts_with("-") && !name)
          name = argument;
        else
        {
          print_new_usage(error);
          return 2;
        }
      }

      if (!name)
      {
        print_new_usage(error);
        return 2;
      }

      return new_project(working_directory, *name, options, output, error);
    }

    if (arguments.front() == "box")
    {
      if (arguments.size() == 2 && arguments[1] == "list")
        return list_boxes(working_directory, output, error);

      if ((arguments.size() == 2 || arguments.size() == 3) && arguments[1] == "create")
      {
        const auto target = arguments.size() == 3
          ? std::optional<std::string> { arguments[2] }
          : std::nullopt;
        return create_box(working_directory, target, output, error);
      }

      if (arguments.size() == 3 && arguments[1] == "inspect")
        return inspect_box(arguments[2], working_directory, output, error);

      if (arguments.size() == 3 && arguments[1] == "verify")
        return verify_box(arguments[2], working_directory, output, error);

      if (arguments.size() == 3 && arguments[1] == "extract")
        return extract_box(arguments[2], working_directory, output, error);

      if (arguments.size() == 3 && arguments[1] == "publish")
        return publish_box(arguments[2], working_directory, output, error);

      error << "forge: usage: forge box list\n";
      error << "forge: usage: forge box create [target]\n";
      error << "forge: usage: forge box <inspect|verify|extract|publish> <path-or-filename>\n";
      return 2;
    }

    if (arguments.front() == "run" || arguments.front() == "build-and-run")
    {
      const auto build_first = arguments.front() == "build-and-run";
      const auto workspace =
        !std::filesystem::exists(working_directory / "forge.recipe.toml")
        && std::filesystem::exists(working_directory / "forge.workspace.toml");
      Recipe recipe;

      if (!workspace && !read_recipe(working_directory / "forge.recipe.toml", recipe, error))
        return 2;

      std::optional<std::string> profile;
      std::optional<std::string> target;
      std::vector<std::string_view> program_arguments;
      bool forwarding = false;

      for (const auto argument : arguments.subspan(1))
      {
        if (!forwarding && argument == "--")
          forwarding = true;
        else if (!forwarding && argument.starts_with("--profile="))
        {
          if (!build_first)
          {
            error << "forge: --profile applies to build commands; use 'forge build-and-run'\n";
            return 2;
          }

          if (!read_profile_option(argument, profile, error))
            return 2;
        }
        else if (!forwarding && (workspace || !recipe.targets.empty()) && !target)
        {
          target = std::string { argument };
        }
        else
          program_arguments.push_back(argument);
      }

      if (workspace)
      {
        if (!target)
        {
          error << "forge: workspace " << arguments.front()
                << " requires <project> or <project>/<target>\n";
          return 2;
        }

        if (build_first)
        {
          return build_and_run_workspace(
            working_directory,
            *target,
            profile,
            program_arguments,
            output,
            error
          );
        }

        return run_workspace(
          working_directory,
          *target,
          profile,
          program_arguments,
          output,
          error
        );
      }

      if (!recipe.targets.empty() && !target)
      {
        if (build_first)
        {
          return build_and_run_project(
            working_directory,
            std::nullopt,
            profile,
            program_arguments,
            output,
            error
          );
        }

        return run_project(working_directory, std::nullopt, profile, program_arguments, output, error);
      }

      if (build_first)
      {
        return build_and_run_project(
          working_directory,
          target,
          profile,
          program_arguments,
          output,
          error
        );
      }

      return run_project(
        working_directory,
        target,
        profile,
        program_arguments,
        output,
        error
      );
    }

    if (arguments.front() == "test")
    {
      std::optional<std::string> target;
      std::optional<std::string> profile;
      std::vector<std::string_view> test_arguments;
      bool forwarding = false;

      for (const auto argument : arguments.subspan(1))
      {
        if (!forwarding && argument == "--")
          forwarding = true;
        else if (!forwarding && argument.starts_with("--profile="))
        {
          if (!read_profile_option(argument, profile, error))
            return 2;
        }
        else if (!forwarding && !target)
        {
          target = std::string { argument };
        }
        else
          test_arguments.push_back(argument);
      }

      if (!std::filesystem::exists(working_directory / "forge.recipe.toml")
          && std::filesystem::exists(working_directory / "forge.workspace.toml"))
      {
        return test_workspace(
          working_directory,
          target,
          profile,
          test_arguments,
          output,
          error
        );
      }

      return test_project(working_directory, target, profile, test_arguments, output, error);
    }

    if (arguments.front() == "bump")
    {
      if (arguments.size() != 2
          || (arguments[1] != "major" && arguments[1] != "minor" && arguments[1] != "patch"))
      {
        error << "forge: usage: forge bump <major|minor|patch>\n";
        return 2;
      }

      return bump_project(working_directory, arguments[1], output, error);
    }

    if (arguments.front() == "profile")
    {
      if (arguments.size() == 2 && arguments[1] == "list")
        return list_profiles(working_directory, output, error);

      error << "forge: usage: forge profile list\n";
      return 2;
    }

    if (arguments.front() == "update")
    {
      BuildOptions options;
      options.dependencies_only = true;
      options.update_dependencies = true;
      bool all_targets = false;
      bool release_targets = false;
      bool all_profiles = false;

      for (const auto argument : arguments.subspan(1))
      {
        if (argument.starts_with("--profile="))
        {
          if (!read_profile_option(argument, options.profile, error))
            return 2;
        }
        else if (argument.starts_with("--target="))
        {
          const auto value = *option_value(argument, "--target=");

          if (!read_required_option(value, "forge: --target requires a value", error))
            return 2;

          options.update_target = std::string { value };
        }
        else if (argument == "--all-targets")
          all_targets = true;
        else if (argument == "--release-targets")
          release_targets = true;
        else if (argument == "--all-profiles")
          all_profiles = true;
        else if (!options.update_dependency && !argument.starts_with("-"))
        {
          options.update_dependency = std::string { argument };
        }
        else
        {
          if (argument.starts_with("-"))
            print_unknown_option(argument, error);

          print_update_usage(error);
          return 2;
        }
      }

      return run_dependency_update(
        working_directory,
        options,
        all_targets,
        release_targets,
        all_profiles,
        output,
        error
      );
    }

    if (arguments.front() == "upgrade")
    {
      BuildOptions options;
      options.dependencies_only = true;
      options.update_dependencies = true;
      bool all_targets = false;
      bool release_targets = false;
      bool all_profiles = false;
      std::optional<std::string> version;
      bool latest = false;

      for (const auto argument : arguments.subspan(1))
      {
        if (argument.starts_with("--profile="))
        {
          if (!read_profile_option(argument, options.profile, error))
            return 2;
        }
        else if (argument.starts_with("--target="))
        {
          const auto value = *option_value(argument, "--target=");

          if (!read_required_option(value, "forge: --target requires a value", error))
            return 2;

          options.update_target = std::string { value };
        }
        else if (argument == "--all-targets")
          all_targets = true;
        else if (argument == "--release-targets")
          release_targets = true;
        else if (argument == "--all-profiles")
          all_profiles = true;
        else if (argument == "--latest")
          latest = true;
        else if (const auto value = option_value(argument, "--to="))
        {
          if (!read_required_option(*value, "forge: --to requires a version", error))
            return 2;

          if (!set_once(version, *value))
          {
            print_upgrade_usage(error);
            return 2;
          }
        }
        else if (!options.update_dependency && !argument.starts_with("-"))
          options.update_dependency = std::string { argument };
        else
        {
          if (argument.starts_with("-"))
            print_unknown_option(argument, error);

          print_upgrade_usage(error);
          return 2;
        }
      }

      if (!options.update_dependency || (latest == version.has_value()))
      {
        print_upgrade_usage(error);
        return 2;
      }

      const auto target_selector_count =
        (options.update_target ? 1 : 0)
        + (all_targets ? 1 : 0)
        + (release_targets ? 1 : 0);

      if (target_selector_count > 1)
      {
        print_upgrade_usage(error);
        return 2;
      }

      if (all_profiles && options.profile)
      {
        print_upgrade_usage(error);
        return 2;
      }

      std::vector<std::optional<std::string>> upgrade_profiles;

      if (all_profiles)
      {
        if (!collect_update_profiles(working_directory, options, upgrade_profiles, error))
          return 2;
      }
      else
        upgrade_profiles.push_back(options.profile);

      if (latest
          && !latest_dependency_version(
            working_directory,
            upgrade_profiles.front(),
            *options.update_dependency,
            version.emplace(),
            output,
            error
          ))
      {
        return 2;
      }

      if (all_profiles)
      {
        if (!upgrade_recipe_dependency_profiles(
          working_directory,
          upgrade_profiles,
          *options.update_dependency,
          *version,
          error
        ))
        {
          return 2;
        }
      }
      else if (!upgrade_recipe_dependency(
        working_directory,
        options.profile,
        *options.update_dependency,
        *version,
        error
      ))
      {
        return 2;
      }

      output << "Upgraded dependency " << *options.update_dependency
             << " to " << *version << '\n';
      return run_dependency_update(
        working_directory,
        options,
        all_targets,
        release_targets,
        all_profiles,
        output,
        error
      );
    }

    if (arguments.front() == "release-git" || arguments.front() == "release-github")
    {
      GitReleaseOptions options;
      options.tag_format = "release-<version>";

      if (arguments.size() == 2 && arguments[1] == "--tag-force")
        options.force_tag = true;
      else if (const auto value = arguments.size() == 2
                                    ? option_value(arguments[1], "--tag=")
                                    : std::nullopt)
      {
        options.tag_format = std::string { *value };

        if (options.tag_format->empty())
        {
          error << "forge: tag format cannot be empty\n";
          return 2;
        }
      }
      else if (const auto value = arguments.size() == 2
                                    ? option_value(arguments[1], "--tag-force=")
                                    : std::nullopt)
      {
        options.tag_format = std::string { *value };
        options.force_tag = true;

        if (options.tag_format->empty())
        {
          error << "forge: tag format cannot be empty\n";
          return 2;
        }
      }
      else if (arguments.size() != 1)
      {
        print_release_git_usage(error);
        return 2;
      }

      return release_git(working_directory, options, output, error);
    }

    if (arguments.front() == "build")
    {
      BuildOptions options;

      for (const auto argument : arguments.subspan(1))
      {
        if (argument.starts_with("--profile="))
        {
          if (!read_profile_option(argument, options.profile, error))
            return 2;
        }
        else if (argument.starts_with("--define="))
        {
          const auto definition = *option_value(argument, "--define=");

          if (!is_valid_compile_definition(definition))
          {
            error << "forge: invalid compile definition '" << definition << "'\n";
            return 2;
          }

          options.compile_definitions.emplace_back(definition);
        }
        else if (!options.target)
        {
          options.target = std::string { argument };
        }
        else
        {
          print_build_usage(error);
          return 2;
        }
      }

      if (!std::filesystem::exists(working_directory / "forge.recipe.toml")
          && std::filesystem::exists(working_directory / "forge.workspace.toml"))
      {
        return build_workspace(
          working_directory,
          options.target,
          options.profile,
          options.compile_definitions,
          output,
          error
        );
      }

      return build_project(working_directory, options, output, error);
    }

    if (arguments.front() == "workflow")
    {
      if (arguments.size() == 2 && arguments[1] == "list-features")
        return list_github_workflow_features(output);

      const auto workflow_feature_operation =
        arguments.size() >= 2 && arguments[1] == "add-feature"
          ? std::optional { GithubWorkflowFeatureOperation::add }
        : arguments.size() >= 2 && arguments[1] == "update-feature"
          ? std::optional { GithubWorkflowFeatureOperation::update }
        : arguments.size() >= 2 && arguments[1] == "remove-feature"
          ? std::optional { GithubWorkflowFeatureOperation::remove }
        : std::nullopt;

      if (workflow_feature_operation && arguments.size() < 3)
      {
        print_workflow_feature_usage(arguments[1], error);
        return 2;
      }

      if (workflow_feature_operation && arguments.size() >= 3)
      {
        const auto feature = arguments[2];
        std::optional<std::filesystem::path> workflow_file;
        bool apply = false;

        for (const auto argument : arguments.subspan(3))
        {
          if (argument == "--apply" && !apply)
            apply = true;
          else if (const auto value = option_value(argument, "--file=");
                   value && set_once(workflow_file, *value))
          {
            if (!read_required_option(*value, "forge: workflow file cannot be empty", error))
              return 2;
          }
          else
          {
            print_workflow_feature_usage(arguments[1], error);
            return 2;
          }
        }

        if (!workflow_file)
        {
          print_workflow_feature_usage(arguments[1], error);
          return 2;
        }

        return change_github_workflow_feature(
          working_directory,
          *workflow_feature_operation,
          feature,
          *workflow_file,
          apply,
          output,
          error
        );
      }

      if (arguments.size() >= 2 && arguments[1] == "status")
      {
        if (arguments.size() != 3 || !arguments[2].starts_with("--file="))
        {
          error << "forge: usage: forge workflow status --file=<workflow>\n";
          return 2;
        }

        const auto value = *option_value(arguments[2], "--file=");

        if (!read_required_option(value, "forge: workflow file cannot be empty", error))
          return 2;

        return status_github_workflow_features(
          working_directory,
          std::filesystem::path { value },
          output,
          error
        );
      }

      if (arguments.size() < 2 || arguments[1] != "prepare-release")
      {
        print_prepare_release_usage(error);
        return 2;
      }

      PrepareReleaseOptions options;

      for (const auto argument : arguments.subspan(2))
      {
        if (argument == "--skip-unsupported")
        {
          if (options.skip_unsupported)
          {
            error << "forge: skip-unsupported may only be specified once\n";
            return 2;
          }

          options.skip_unsupported = true;
        }
        else if (!argument.starts_with("-") && !options.target)
        {
          options.target = std::string { argument };
        }
        else
        {
          print_prepare_release_usage(error);
          return 2;
        }
      }

      return prepare_release(working_directory, options, run_process, output, error);
    }

    if (arguments.front() == "release" || arguments.front() == "prepare-release")
    {
      if (arguments.size() == 2 && arguments[1].starts_with("-"))
      {
        error << "forge: commands do not accept arguments yet\n";
        return 2;
      }

      if (arguments.size() > 2)
      {
        error << "forge: usage: forge " << arguments.front() << " [target]\n";
        return 2;
      }

      const auto target = arguments.size() == 2
        ? std::optional<std::string> { arguments[1] }
        : std::nullopt;

      if (arguments.front() == "release")
        return release_project(working_directory, target, output, error);

      error
        << "forge: warning: 'prepare-release' is deprecated; "
        << "use 'forge workflow prepare-release'\n";
      return prepare_release(working_directory, target, output, error);
    }

    if (arguments.front() == "adopt")
    {
      AdoptOptions options;
      bool dependency_style_set = false;

      for (const auto argument : arguments.subspan(1))
      {
        if (argument == "--github")
        {
          if (dependency_style_set)
          {
            error << "forge: dependency style specified more than once\n";
            return 2;
          }

          options.dependency_style = DependencyStyle::git;
          dependency_style_set = true;
        }
        else if (const auto style = option_value(argument, "--dependency-style="))
        {
          if (dependency_style_set)
          {
            error << "forge: dependency style specified more than once\n";
            return 2;
          }

          if (*style == "local")
            options.dependency_style = DependencyStyle::local;
          else if (*style == "git")
            options.dependency_style = DependencyStyle::git;
          else
          {
            error << "forge: dependency style must be local or git\n";
            return 2;
          }

          dependency_style_set = true;
        }
        else if (const auto type = option_value(argument, "--library-type="))
        {
          if (options.library_type
              || (*type != "header_only"
                  && *type != "static_library"
                  && *type != "dynamic_library"))
          {
            error
              << "forge: library type must be header_only, static_library, or dynamic_library; "
              << "configure imported_library manually with import profiles\n";
            return 2;
          }

          options.library_type = std::string { *type };
        }
        else if (const auto value = option_value(argument, "--init-version=");
                 value && set_once(options.initial_version, *value))
        {
        }
        else if (const auto value = option_value(argument, "--version-header-path=");
                 value && set_once(options.version_header_path, *value))
        {
        }
        else
        {
          print_adopt_usage(error);
          return 2;
        }
      }

      return adopt_project(working_directory, options, run_process, output, error);
    }

    if (arguments.size() != 1)
    {
      error << "forge: commands do not accept arguments yet\n";
      return 2;
    }

    if (arguments.front() == "init")
      return init_project(working_directory, output, error);

    if (arguments.front() == "clean")
    {
      if (!std::filesystem::exists(working_directory / "forge.recipe.toml")
          && std::filesystem::exists(working_directory / "forge.workspace.toml"))
      {
        return clean_workspace(working_directory, output, error);
      }

      return clean_project(working_directory, output, error);
    }

    error << "forge: '" << arguments.front() << "' is not implemented yet\n";
    return 2;
  }

} // namespace forge::cli
