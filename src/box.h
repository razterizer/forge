#pragma once

#include "process.h"

#include <filesystem>
#include <iosfwd>

namespace forge
{

  int create_box(const std::filesystem::path& project_directory,
                 std::ostream& output,
                 std::ostream& error);

  int create_box(const std::filesystem::path& project_directory,
                 const ProcessRunner& process_runner,
                 std::ostream& output,
                 std::ostream& error);

  int inspect_box(const std::filesystem::path& box_path,
                  const std::filesystem::path& working_directory,
                  std::ostream& output,
                  std::ostream& error);

  int inspect_box(const std::filesystem::path& box_path,
                  const std::filesystem::path& working_directory,
                  const ProcessRunner& process_runner,
                  std::ostream& output,
                  std::ostream& error);

  int verify_box(const std::filesystem::path& box_path,
                 const std::filesystem::path& working_directory,
                 std::ostream& output,
                 std::ostream& error);

  int verify_box(const std::filesystem::path& box_path,
                 const std::filesystem::path& working_directory,
                 const ProcessRunner& process_runner,
                 std::ostream& output,
                 std::ostream& error);

  int extract_box(const std::filesystem::path& box_path,
                  const std::filesystem::path& working_directory,
                  std::ostream& output,
                  std::ostream& error);

  int extract_box(const std::filesystem::path& box_path,
                  const std::filesystem::path& working_directory,
                  const ProcessRunner& process_runner,
                  std::ostream& output,
                  std::ostream& error);

} // namespace forge
