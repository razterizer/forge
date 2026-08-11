#pragma once

#include "fprocess.h"
#include "build.h"

#include <filesystem>
#include <iosfwd>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace forge
{

  bool select_cached_run_variant(
    const std::filesystem::path& project_directory,
    const std::optional<std::string>& target,
    const std::optional<std::string>& configuration,
    const std::optional<std::string>& style,
    const std::optional<std::string>& profile,
    std::ostream& error
  );

  int run_project(const std::filesystem::path& project_directory,
                  std::span<const std::string_view> arguments,
                  std::ostream& output,
                  std::ostream& error);

  int run_project(const std::filesystem::path& project_directory,
                  const std::optional<std::string>& target,
                  std::span<const std::string_view> arguments,
                  std::ostream& output,
                  std::ostream& error);

  int run_project(const std::filesystem::path& project_directory,
                  const std::optional<std::string>& target,
                  const std::optional<std::string>& profile,
                  std::span<const std::string_view> arguments,
                  std::ostream& output,
                  std::ostream& error);

  int run_project(const std::filesystem::path& project_directory,
                  const std::optional<std::string>& target,
                  const std::optional<std::string>& profile,
                  const std::optional<std::string>& system_profile,
                  std::span<const std::string_view> arguments,
                  std::ostream& output,
                  std::ostream& error);

  int run_project(const std::filesystem::path& project_directory,
                  std::span<const std::string_view> arguments,
                  const ProcessRunner& process_runner,
                  std::ostream& output,
                  std::ostream& error);

  int run_project(const std::filesystem::path& project_directory,
                  const std::optional<std::string>& target,
                  std::span<const std::string_view> arguments,
                  const ProcessRunner& process_runner,
                  std::ostream& output,
                  std::ostream& error);

  int run_project(const std::filesystem::path& project_directory,
                  const std::optional<std::string>& target,
                  const std::optional<std::string>& profile,
                  std::span<const std::string_view> arguments,
                  const ProcessRunner& process_runner,
                  std::ostream& output,
                  std::ostream& error);

  int run_project(const std::filesystem::path& project_directory,
                  const std::optional<std::string>& target,
                  const std::optional<std::string>& profile,
                  const std::optional<std::string>& system_profile,
                  std::span<const std::string_view> arguments,
                  const ProcessRunner& process_runner,
                  std::ostream& output,
                  std::ostream& error);

  int build_and_run_project(const std::filesystem::path& project_directory,
                            std::span<const std::string_view> arguments,
                            std::ostream& output,
                            std::ostream& error);

  int build_and_run_project(const std::filesystem::path& project_directory,
                            const BuildOptions& options,
                            std::span<const std::string_view> arguments,
                            std::ostream& output,
                            std::ostream& error);

  int build_and_run_project(const std::filesystem::path& project_directory,
                            const std::optional<std::string>& target,
                            std::span<const std::string_view> arguments,
                            std::ostream& output,
                            std::ostream& error);

  int build_and_run_project(const std::filesystem::path& project_directory,
                            const std::optional<std::string>& target,
                            const std::optional<std::string>& profile,
                            std::span<const std::string_view> arguments,
                            std::ostream& output,
                            std::ostream& error);

  int build_and_run_project(const std::filesystem::path& project_directory,
                            const std::optional<std::string>& target,
                            const std::optional<std::string>& profile,
                            const std::optional<std::string>& system_profile,
                            std::span<const std::string_view> arguments,
                            std::ostream& output,
                            std::ostream& error);

  int build_and_run_project(const std::filesystem::path& project_directory,
                            std::span<const std::string_view> arguments,
                            const ProcessRunner& process_runner,
                            std::ostream& output,
                            std::ostream& error);

  int build_and_run_project(const std::filesystem::path& project_directory,
                            const std::optional<std::string>& target,
                            std::span<const std::string_view> arguments,
                            const ProcessRunner& process_runner,
                            std::ostream& output,
                            std::ostream& error);

  int build_and_run_project(const std::filesystem::path& project_directory,
                            const std::optional<std::string>& target,
                            const std::optional<std::string>& profile,
                            std::span<const std::string_view> arguments,
                            const ProcessRunner& process_runner,
                            std::ostream& output,
                            std::ostream& error);

  int build_and_run_project(const std::filesystem::path& project_directory,
                            const std::optional<std::string>& target,
                            const std::optional<std::string>& profile,
                            const std::optional<std::string>& system_profile,
                            std::span<const std::string_view> arguments,
                            const ProcessRunner& process_runner,
                            std::ostream& output,
                            std::ostream& error);

} // namespace forge
