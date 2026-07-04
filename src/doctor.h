#pragma once

#include <filesystem>
#include <iosfwd>

#include "fprocess.h"

namespace forge
{

  struct DoctorOptions
  {
    bool search_github = false;
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
