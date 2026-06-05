#pragma once

#include <filesystem>
#include <iosfwd>
#include <string>
#include <vector>

namespace forge
{

  int run_process(const std::vector<std::string>& arguments,
                  const std::filesystem::path& working_directory,
                  std::ostream& error);

} // namespace forge
