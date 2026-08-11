#pragma once

#include <filesystem>
#include <iosfwd>

#include "fprocess.h"
#include "project_scan.h"

namespace forge
{

  struct DoctorOptions
  {
    bool search_github = false;
    LocalDependencySearch local_search = LocalDependencySearch::nearby;
  };

  int doctor_project(const std::filesystem::path& project_directory,
                     std::ostream& output,
                     std::ostream& error);

  int doctor_project(const std::filesystem::path& project_directory,
                     const DoctorOptions& options,
                     const ProcessRunner& process_runner,
                     std::ostream& output,
                     std::ostream& error);

} // namespace forge
