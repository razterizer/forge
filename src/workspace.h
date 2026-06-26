#pragma once

#include "build.h"

#include <filesystem>
#include <iosfwd>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace forge
{

  inline constexpr std::string_view workspace_schema_url =
    "https://raw.githubusercontent.com/razterizer/forge/main/schemas/forge.workspace.schema.json";

  struct WorkspaceProject
  {
    std::string name;
    std::filesystem::path path;
  };

  struct Workspace
  {
    std::string name;
    std::vector<WorkspaceProject> projects;
  };

  bool read_workspace(const std::filesystem::path& path,
                      Workspace& workspace,
                      std::ostream& error);

  int build_workspace(const std::filesystem::path& workspace_directory,
                      const std::optional<std::string>& project,
                      const std::optional<std::string>& profile,
                      std::ostream& output,
                      std::ostream& error);

  int build_workspace(const std::filesystem::path& workspace_directory,
                      const std::optional<std::string>& project,
                      const std::optional<std::string>& profile,
                      const std::vector<std::string>& compile_definitions,
                      std::ostream& output,
                      std::ostream& error);

  int build_workspace(const std::filesystem::path& workspace_directory,
                      const std::optional<std::string>& project,
                      const std::optional<std::string>& profile,
                      const ProcessRunner& process_runner,
                      std::ostream& output,
                      std::ostream& error);

  int build_workspace(const std::filesystem::path& workspace_directory,
                      const std::optional<std::string>& project,
                      const std::optional<std::string>& profile,
                      const std::vector<std::string>& compile_definitions,
                      const ProcessRunner& process_runner,
                      std::ostream& output,
                      std::ostream& error);

  int run_workspace(const std::filesystem::path& workspace_directory,
                    std::string_view selection,
                    const std::optional<std::string>& profile,
                    std::span<const std::string_view> arguments,
                    std::ostream& output,
                    std::ostream& error);

  int run_workspace(const std::filesystem::path& workspace_directory,
                    std::string_view selection,
                    const std::optional<std::string>& profile,
                    std::span<const std::string_view> arguments,
                    const ProcessRunner& process_runner,
                    std::ostream& output,
                    std::ostream& error);

  int build_and_run_workspace(const std::filesystem::path& workspace_directory,
                              std::string_view selection,
                              const std::optional<std::string>& profile,
                              std::span<const std::string_view> arguments,
                              std::ostream& output,
                              std::ostream& error);

  int build_and_run_workspace(const std::filesystem::path& workspace_directory,
                              std::string_view selection,
                              const std::optional<std::string>& profile,
                              std::span<const std::string_view> arguments,
                              const ProcessRunner& process_runner,
                              std::ostream& output,
                              std::ostream& error);

  int test_workspace(const std::filesystem::path& workspace_directory,
                     const std::optional<std::string>& selection,
                     const std::optional<std::string>& profile,
                     std::span<const std::string_view> arguments,
                     std::ostream& output,
                     std::ostream& error);

  int test_workspace(const std::filesystem::path& workspace_directory,
                     const std::optional<std::string>& selection,
                     const std::optional<std::string>& profile,
                     std::span<const std::string_view> arguments,
                     const ProcessRunner& process_runner,
                     std::ostream& output,
                     std::ostream& error);

  int clean_workspace(const std::filesystem::path& workspace_directory,
                      std::ostream& output,
                      std::ostream& error);

} // namespace forge
