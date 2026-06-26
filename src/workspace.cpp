#include "workspace.h"

#include "recipe.h"
#include "run.h"
#include "test.h"

#include <algorithm>
#include <fstream>
#include <map>
#include <set>
#include <string>
#include <string_view>

namespace forge
{
  namespace
  {

    std::string_view trim(std::string_view value)
    {
      const auto first = value.find_first_not_of(" \t\r");

      if (first == std::string_view::npos)
      {
        return {};
      }

      return value.substr(first, value.find_last_not_of(" \t\r") - first + 1);
    }

    bool parse_string(std::string_view value, std::string& result)
    {
      value = trim(value);

      if (value.size() < 2 || value.front() != '"' || value.back() != '"')
        return false;

      result = value.substr(1, value.size() - 2);
      return result.find('"') == std::string::npos;
    }

    bool parse_paths(std::string_view value, std::vector<std::filesystem::path>& paths)
    {
      value = trim(value);

      if (value.size() < 2 || value.front() != '[' || value.back() != ']')
        return false;

      value = trim(value.substr(1, value.size() - 2));

      while (!value.empty())
      {
        if (value.front() != '"')
          return false;

        const auto end = value.find('"', 1);

        if (end == std::string_view::npos)
          return false;

        paths.emplace_back(value.substr(1, end - 1));
        value = trim(value.substr(end + 1));

        if (value.empty())
          break;

        if (value.front() != ',')
          return false;

        value = trim(value.substr(1));
      }

      return true;
    }

    bool is_inside(const std::filesystem::path& root, const std::filesystem::path& path)
    {
      const auto relative = path.lexically_relative(root);
      return !relative.empty()
        && !relative.is_absolute()
        && *relative.begin() != "..";
    }

    bool resolve_projects(const std::filesystem::path& workspace_directory,
                          Workspace& workspace,
                          std::map<std::filesystem::path, Recipe>& recipes,
                          std::ostream& error)
    {
      std::set<std::string> names;
      std::set<std::filesystem::path> paths;
      std::error_code filesystem_error;
      const auto root = std::filesystem::weakly_canonical(workspace_directory, filesystem_error);

      if (filesystem_error)
      {
        error << "forge: could not resolve workspace directory\n";
        return false;
      }

      for (auto& project : workspace.projects)
      {
        if (project.path.empty() || project.path.is_absolute())
        {
          error << "forge: workspace project paths must be relative\n";
          return false;
        }

        const auto directory =
          std::filesystem::weakly_canonical(root / project.path, filesystem_error);

        if (filesystem_error || !is_inside(root, directory))
        {
          error << "forge: workspace project path '" << project.path.generic_string()
                << "' must stay inside the workspace\n";
          return false;
        }

        Recipe recipe;

        if (!read_recipe(directory / "forge.recipe.toml", recipe, error))
          return false;

        if (!names.insert(recipe.name).second)
        {
          error << "forge: workspace contains duplicate project name '" << recipe.name << "'\n";
          return false;
        }

        if (!paths.insert(directory).second)
        {
          error << "forge: workspace contains duplicate project path '"
                << project.path.generic_string() << "'\n";
          return false;
        }

        project.name = recipe.name;
        project.path = directory;
        recipes.emplace(directory, std::move(recipe));
      }

      return true;
    }

    bool order_projects(const Workspace& workspace,
                        const std::map<std::filesystem::path, Recipe>& recipes,
                        const std::optional<std::string>& selected,
                        const std::optional<std::string>& profile,
                        std::vector<const WorkspaceProject*>& ordered,
                        std::ostream& error)
    {
      std::map<std::filesystem::path, const WorkspaceProject*> projects;

      for (const auto& project : workspace.projects)
        projects.emplace(project.path, &project);

      std::set<std::filesystem::path> active;
      std::set<std::filesystem::path> resolved;

      const auto visit = [&](const auto& self, const WorkspaceProject& project) -> bool
      {
        if (resolved.contains(project.path))
          return true;

        if (!active.insert(project.path).second)
        {
          error << "forge: workspace dependency cycle detected at '" << project.name << "'\n";
          return false;
        }

        auto recipe = recipes.at(project.path);

        if (!select_dependency_profile(recipe, profile, false, error))
          return false;

        for (const auto& dependency : recipe.dependencies)
        {
          if (dependency.path.empty())
            continue;

          std::error_code filesystem_error;
          const auto dependency_path =
            std::filesystem::weakly_canonical(project.path / dependency.path, filesystem_error);

          if (filesystem_error)
            continue;

          const auto workspace_dependency = projects.find(dependency_path);

          if (workspace_dependency != projects.end()
              && !self(self, *workspace_dependency->second))
          {
            return false;
          }
        }

        active.erase(project.path);
        resolved.insert(project.path);
        ordered.push_back(&project);
        return true;
      };

      if (selected)
      {
        const auto project = std::find_if(
          workspace.projects.begin(),
          workspace.projects.end(),
          [&selected](const WorkspaceProject& candidate)
          {
            return candidate.name == *selected;
          }
        );

        if (project == workspace.projects.end())
        {
          error << "forge: workspace has no project named '" << *selected << "'\n";
          return false;
        }

        return visit(visit, *project);
      }

      for (const auto& project : workspace.projects)
      {
        if (!visit(visit, project))
          return false;
      }

      return true;
    }

    bool load_workspace(const std::filesystem::path& workspace_directory,
                        Workspace& workspace,
                        std::map<std::filesystem::path, Recipe>& recipes,
                        std::ostream& error)
    {
      return read_workspace(workspace_directory / "forge.workspace.toml", workspace, error)
        && resolve_projects(workspace_directory, workspace, recipes, error);
    }

    bool parse_selection(std::string_view selection,
                         std::string& project,
                         std::optional<std::string>& target,
                         std::ostream& error)
    {
      const auto slash = selection.find('/');

      if (slash == std::string_view::npos)
        project = selection;
      else
      {
        project = selection.substr(0, slash);
        target = selection.substr(slash + 1);
      }

      if (project.empty()
          || (target && target->empty())
          || (target && target->find('/') != std::string::npos))
      {
        error << "forge: workspace selection must be <project> or <project>/<target>\n";
        return false;
      }

      return true;
    }

    const WorkspaceProject* find_project(const Workspace& workspace,
                                         std::string_view name,
                                         std::ostream& error)
    {
      const auto project = std::find_if(
        workspace.projects.begin(),
        workspace.projects.end(),
        [name](const WorkspaceProject& candidate)
        {
          return candidate.name == name;
        }
      );

      if (project == workspace.projects.end())
      {
        error << "forge: workspace has no project named '" << name << "'\n";
        return nullptr;
      }

      return &*project;
    }

    bool has_tests(const Recipe& recipe)
    {
      return std::any_of(
        recipe.targets.begin(),
        recipe.targets.end(),
        [](const RecipeTarget& target)
        {
          return target.test;
        }
      );
    }

  } // namespace

  bool read_workspace(const std::filesystem::path& path,
                      Workspace& workspace,
                      std::ostream& error)
  {
    std::ifstream file { path };

    if (!file)
    {
      error << "forge: could not open '" << path.string() << "'\n";
      return false;
    }

    std::string section;
    std::string line;
    std::vector<std::filesystem::path> projects;
    std::size_t line_number = 0;

    while (std::getline(file, line))
    {
      ++line_number;
      auto content = trim(line);

      if (content.empty() || content.front() == '#')
        continue;

      if (content.front() == '[' && content.back() == ']')
      {
        section = trim(content.substr(1, content.size() - 2));
        continue;
      }

      const auto equals = content.find('=');

      if (equals == std::string_view::npos)
      {
        error << "forge: invalid workspace value on line " << line_number << '\n';
        return false;
      }

      const auto key = trim(content.substr(0, equals));
      const auto value = trim(content.substr(equals + 1));
      const auto valid =
        (section == "workspace" && key == "name" && parse_string(value, workspace.name))
        || (section == "workspace" && key == "projects" && parse_paths(value, projects));

      if (!valid)
      {
        error << "forge: invalid workspace value on line " << line_number << '\n';
        return false;
      }
    }

    if (workspace.name.empty() || projects.empty())
    {
      error << "forge: workspace requires a name and at least one project\n";
      return false;
    }

    for (auto& project : projects)
    {
      workspace.projects.push_back({ {}, std::move(project) });
    }

    return true;
  }

  int build_workspace(const std::filesystem::path& workspace_directory,
                      const std::optional<std::string>& project,
                      const std::optional<std::string>& profile,
                      std::ostream& output,
                      std::ostream& error)
  {
    return build_workspace(
      workspace_directory,
      project,
      profile,
      {},
      run_process,
      output,
      error
    );
  }

  int build_workspace(const std::filesystem::path& workspace_directory,
                      const std::optional<std::string>& project,
                      const std::optional<std::string>& profile,
                      const ProcessRunner& process_runner,
                      std::ostream& output,
                      std::ostream& error)
  {
    return build_workspace(
      workspace_directory,
      project,
      profile,
      {},
      process_runner,
      output,
      error
    );
  }

  int build_workspace(const std::filesystem::path& workspace_directory,
                      const std::optional<std::string>& project,
                      const std::optional<std::string>& profile,
                      const std::vector<std::string>& compile_definitions,
                      std::ostream& output,
                      std::ostream& error)
  {
    return build_workspace(
      workspace_directory,
      project,
      profile,
      compile_definitions,
      run_process,
      output,
      error
    );
  }

  int build_workspace(const std::filesystem::path& workspace_directory,
                      const std::optional<std::string>& project,
                      const std::optional<std::string>& profile,
                      const std::vector<std::string>& compile_definitions,
                      const ProcessRunner& process_runner,
                      std::ostream& output,
                      std::ostream& error)
  {
    Workspace workspace;

    std::map<std::filesystem::path, Recipe> recipes;

    if (!load_workspace(workspace_directory, workspace, recipes, error))
      return 2;

    std::vector<const WorkspaceProject*> ordered;

    if (!order_projects(workspace, recipes, project, profile, ordered, error))
      return 2;

    std::set<std::filesystem::path> dependency_projects;

    for (const auto* current : ordered)
    {
      auto recipe = recipes.at(current->path);

      if (!select_dependency_profile(recipe, profile, false, error))
        return 2;

      for (const auto& dependency : recipe.dependencies)
      {
        if (dependency.path.empty())
          continue;

        std::error_code filesystem_error;
        const auto dependency_path =
          std::filesystem::weakly_canonical(current->path / dependency.path, filesystem_error);

        if (!filesystem_error && recipes.contains(dependency_path))
          dependency_projects.insert(dependency_path);
      }
    }

    output << "Building workspace " << workspace.name << '\n';

    for (const auto* current : ordered)
    {
      if ((project && current->name != *project)
          || (!project && dependency_projects.contains(current->path)))
      {
        continue;
      }

      auto recipe = recipes.at(current->path);

      if (recipe.targets.empty())
      {
        BuildOptions options;
        options.profile = profile;
        options.compile_definitions = compile_definitions;

        if (build_project(current->path, options, process_runner, output, error) != 0)
          return 2;
      }
      else
      {
        for (const auto& target : recipe.targets)
        {
          BuildOptions options;
          options.profile = profile;
          options.target = target.name;
          options.compile_definitions = compile_definitions;

          if (build_project(current->path, options, process_runner, output, error) != 0)
            return 2;
        }
      }
    }

    output << "Built workspace " << workspace.name << '\n';
    return 0;
  }

  int run_workspace(const std::filesystem::path& workspace_directory,
                    std::string_view selection,
                    const std::optional<std::string>& profile,
                    std::span<const std::string_view> arguments,
                    std::ostream& output,
                    std::ostream& error)
  {
    return run_workspace(
      workspace_directory,
      selection,
      profile,
      arguments,
      run_process,
      output,
      error
    );
  }

  int run_workspace(const std::filesystem::path& workspace_directory,
                    std::string_view selection,
                    const std::optional<std::string>& profile,
                    std::span<const std::string_view> arguments,
                    const ProcessRunner& process_runner,
                    std::ostream& output,
                    std::ostream& error)
  {
    Workspace workspace;
    std::map<std::filesystem::path, Recipe> recipes;

    if (!load_workspace(workspace_directory, workspace, recipes, error))
      return 2;

    std::string project_name;
    std::optional<std::string> target;

    if (!parse_selection(selection, project_name, target, error))
      return 2;

    const auto* project = find_project(workspace, project_name, error);

    if (!project)
      return 2;

    return run_project(
      project->path,
      target,
      profile,
      arguments,
      process_runner,
      output,
      error
    );
  }

  int build_and_run_workspace(const std::filesystem::path& workspace_directory,
                              std::string_view selection,
                              const std::optional<std::string>& profile,
                              std::span<const std::string_view> arguments,
                              std::ostream& output,
                              std::ostream& error)
  {
    return build_and_run_workspace(
      workspace_directory,
      selection,
      profile,
      arguments,
      run_process,
      output,
      error
    );
  }

  int build_and_run_workspace(const std::filesystem::path& workspace_directory,
                              std::string_view selection,
                              const std::optional<std::string>& profile,
                              std::span<const std::string_view> arguments,
                              const ProcessRunner& process_runner,
                              std::ostream& output,
                              std::ostream& error)
  {
    Workspace workspace;
    std::map<std::filesystem::path, Recipe> recipes;

    if (!load_workspace(workspace_directory, workspace, recipes, error))
      return 2;

    std::string project_name;
    std::optional<std::string> target;

    if (!parse_selection(selection, project_name, target, error))
      return 2;

    const auto* project = find_project(workspace, project_name, error);

    if (!project)
      return 2;

    return build_and_run_project(
      project->path,
      target,
      profile,
      arguments,
      process_runner,
      output,
      error
    );
  }

  int test_workspace(const std::filesystem::path& workspace_directory,
                     const std::optional<std::string>& selection,
                     const std::optional<std::string>& profile,
                     std::span<const std::string_view> arguments,
                     std::ostream& output,
                     std::ostream& error)
  {
    return test_workspace(
      workspace_directory,
      selection,
      profile,
      arguments,
      run_process,
      output,
      error
    );
  }

  int test_workspace(const std::filesystem::path& workspace_directory,
                     const std::optional<std::string>& selection,
                     const std::optional<std::string>& profile,
                     std::span<const std::string_view> arguments,
                     const ProcessRunner& process_runner,
                     std::ostream& output,
                     std::ostream& error)
  {
    Workspace workspace;
    std::map<std::filesystem::path, Recipe> recipes;

    if (!load_workspace(workspace_directory, workspace, recipes, error))
      return 2;

    if (selection)
    {
      std::string project_name;
      std::optional<std::string> target;

      if (!parse_selection(*selection, project_name, target, error))
        return 2;

      const auto* project = find_project(workspace, project_name, error);

      if (!project)
        return 2;

      return test_project(
        project->path,
        target,
        profile,
        arguments,
        process_runner,
        output,
        error
      );
    }

    std::size_t passed = 0;
    std::size_t failed = 0;
    bool command_failed = false;

    for (const auto& project : workspace.projects)
    {
      if (!has_tests(recipes.at(project.path)))
        continue;

      output << "Testing project " << project.name << '\n';
      const auto result = test_project(
        project.path,
        std::nullopt,
        profile,
        arguments,
        process_runner,
        output,
        error
      );

      if (result == 0)
        ++passed;
      else
      {
        ++failed;
        command_failed = command_failed || result == 2;
      }
    }

    if (passed + failed == 0)
    {
      error << "forge: workspace contains no projects with named test targets\n";
      return 2;
    }

    output << "Workspace tests: " << passed << " project";

    if (passed != 1)
      output << 's';

    output << " passed, "
           << failed << " failed\n";

    if (failed == 0)
      return 0;

    return command_failed ? 2 : 1;
  }

  int clean_workspace(const std::filesystem::path& workspace_directory,
                      std::ostream& output,
                      std::ostream& error)
  {
    Workspace workspace;
    std::map<std::filesystem::path, Recipe> recipes;

    if (!load_workspace(workspace_directory, workspace, recipes, error))
      return 2;

    std::error_code filesystem_error;
    const auto workspace_forge_directory = workspace_directory / ".forge";
    const auto root_removed =
      std::filesystem::remove_all(workspace_forge_directory, filesystem_error);

    if (filesystem_error)
    {
      error
        << "forge: could not clean '" << workspace_forge_directory.string()
        << "': " << filesystem_error.message() << '\n';
      return 2;
    }

    std::size_t cleaned_projects = root_removed == 0 ? 0 : 1;

    for (const auto& project : workspace.projects)
    {
      const auto forge_directory = project.path / ".forge";
      const auto removed = std::filesystem::remove_all(forge_directory, filesystem_error);

      if (filesystem_error)
      {
        error
          << "forge: could not clean '" << forge_directory.string()
          << "': " << filesystem_error.message() << '\n';
        return 2;
      }

      if (removed != 0)
        ++cleaned_projects;
    }

    if (cleaned_projects == 0)
      output << "Workspace " << workspace.name << " is already clean\n";
    else
      output << "Cleaned workspace " << workspace.name << '\n';

    return 0;
  }

} // namespace forge
