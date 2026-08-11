#pragma once

#include <filesystem>
#include <iosfwd>
#include <string>

namespace forge
{

  bool sha256_file(const std::filesystem::path& path,
                   std::string& checksum,
                   std::ostream& error);

} // namespace forge
