#pragma once

#include <cstddef>
#include <istream>
#include <string>
#include <string_view>
#include <vector>

namespace forge
{

  struct TomlStatement
  {
    std::string content;
    std::size_t line = 0;
  };

  bool read_toml_statements(std::istream& input,
                            std::vector<TomlStatement>& statements,
                            std::size_t& invalid_line);
  bool parse_toml_string(std::string_view value, std::string& result);
  bool parse_toml_string_prefix(std::string_view value,
                                std::string& result,
                                std::size_t& consumed);

} // namespace forge
