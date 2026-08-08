#include "toml_support.h"

#include <cstdint>

namespace forge
{
  namespace
  {

    std::string_view trim(std::string_view value)
    {
      const auto first = value.find_first_not_of(" \t\r\n");

      if (first == std::string_view::npos)
        return {};

      return value.substr(first, value.find_last_not_of(" \t\r\n") - first + 1);
    }

    bool hex_value(char character, std::uint32_t& value)
    {
      if (character >= '0' && character <= '9')
        value = static_cast<std::uint32_t>(character - '0');
      else if (character >= 'a' && character <= 'f')
        value = static_cast<std::uint32_t>(character - 'a' + 10);
      else if (character >= 'A' && character <= 'F')
        value = static_cast<std::uint32_t>(character - 'A' + 10);
      else
        return false;

      return true;
    }

    bool append_codepoint(std::uint32_t codepoint, std::string& result)
    {
      if (codepoint > 0x10ffff || (codepoint >= 0xd800 && codepoint <= 0xdfff))
        return false;

      if (codepoint <= 0x7f)
        result += static_cast<char>(codepoint);
      else if (codepoint <= 0x7ff)
      {
        result += static_cast<char>(0xc0 | (codepoint >> 6));
        result += static_cast<char>(0x80 | (codepoint & 0x3f));
      }
      else if (codepoint <= 0xffff)
      {
        result += static_cast<char>(0xe0 | (codepoint >> 12));
        result += static_cast<char>(0x80 | ((codepoint >> 6) & 0x3f));
        result += static_cast<char>(0x80 | (codepoint & 0x3f));
      }
      else
      {
        result += static_cast<char>(0xf0 | (codepoint >> 18));
        result += static_cast<char>(0x80 | ((codepoint >> 12) & 0x3f));
        result += static_cast<char>(0x80 | ((codepoint >> 6) & 0x3f));
        result += static_cast<char>(0x80 | (codepoint & 0x3f));
      }

      return true;
    }

    bool append_unicode_escape(std::string_view value,
                               std::size_t& index,
                               std::size_t digits,
                               std::string& result)
    {
      if (index + digits > value.size())
        return false;

      std::uint32_t codepoint = 0;

      for (std::size_t offset = 0; offset < digits; ++offset)
      {
        std::uint32_t digit = 0;

        if (!hex_value(value[index + offset], digit))
          return false;

        codepoint = (codepoint << 4) | digit;
      }

      index += digits;
      return append_codepoint(codepoint, result);
    }

  } // namespace

  bool read_toml_statements(std::istream& input,
                            std::vector<TomlStatement>& statements,
                            std::size_t& invalid_line)
  {
    statements.clear();
    invalid_line = 0;
    std::string statement;
    std::string line;
    std::size_t statement_line = 0;
    std::size_t line_number = 0;
    int square_depth = 0;
    int brace_depth = 0;

    while (std::getline(input, line))
    {
      ++line_number;
      std::string content;
      bool quoted = false;
      bool escaped = false;

      for (const auto character : line)
      {
        if (quoted)
        {
          content += character;

          if (escaped)
            escaped = false;
          else if (character == '\\')
            escaped = true;
          else if (character == '"')
            quoted = false;

          continue;
        }

        if (character == '#')
          break;

        content += character;

        if (character == '"')
          quoted = true;
        else if (character == '[')
          ++square_depth;
        else if (character == ']')
          --square_depth;
        else if (character == '{')
          ++brace_depth;
        else if (character == '}')
          --brace_depth;

        if (square_depth < 0 || brace_depth < 0)
        {
          invalid_line = line_number;
          return false;
        }
      }

      if (quoted || escaped)
      {
        invalid_line = line_number;
        return false;
      }

      const auto trimmed = trim(content);

      if (!trimmed.empty())
      {
        if (statement.empty())
          statement_line = line_number;
        else
          statement += ' ';

        statement += trimmed;
      }

      if (square_depth == 0 && brace_depth == 0 && !statement.empty())
      {
        statements.push_back({ std::move(statement), statement_line });
        statement.clear();
      }
    }

    if (square_depth != 0 || brace_depth != 0 || !statement.empty())
    {
      invalid_line = statement_line == 0 ? line_number : statement_line;
      return false;
    }

    return true;
  }

  bool parse_toml_string_prefix(std::string_view value,
                                std::string& result,
                                std::size_t& consumed)
  {
    if (value.empty() || value.front() != '"')
      return false;

    result.clear();
    std::size_t index = 1;

    while (index < value.size())
    {
      const auto character = static_cast<unsigned char>(value[index++]);

      if (character == '"')
      {
        consumed = index;
        return true;
      }

      if (character != '\\')
      {
        if ((character < 0x20 && character != '\t') || character == 0x7f)
          return false;

        result += static_cast<char>(character);
        continue;
      }

      if (index >= value.size())
        return false;

      const auto escape = value[index++];

      switch (escape)
      {
        case 'b': result += '\b'; break;
        case 't': result += '\t'; break;
        case 'n': result += '\n'; break;
        case 'f': result += '\f'; break;
        case 'r': result += '\r'; break;
        case '"': result += '"'; break;
        case '\\': result += '\\'; break;
        case 'u':
          if (!append_unicode_escape(value, index, 4, result))
            return false;
          break;
        case 'U':
          if (!append_unicode_escape(value, index, 8, result))
            return false;
          break;
        default:
          return false;
      }
    }

    return false;
  }

  bool parse_toml_string(std::string_view value, std::string& result)
  {
    value = trim(value);
    std::size_t consumed = 0;
    return parse_toml_string_prefix(value, result, consumed)
      && trim(value.substr(consumed)).empty();
  }

} // namespace forge
