#pragma once

#include <filesystem>
#include <functional>
#include <iosfwd>
#include <string>
#include <vector>

namespace forge
{

  using ProcessRunner = std::function<int(
    const std::vector<std::string>&,
    const std::filesystem::path&,
    std::ostream&)>;

  int run_process(const std::vector<std::string>& arguments,
                  const std::filesystem::path& working_directory,
                  std::ostream& error);

} // namespace forge
