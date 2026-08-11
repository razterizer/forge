#pragma once

#include <filesystem>
#include <iosfwd>
#include <string>
#include <vector>

namespace forge
{

  bool read_zip_entries(const std::filesystem::path& path,
                        std::vector<std::string>& entries,
                        std::ostream& error);

} // namespace forge
