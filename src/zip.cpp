#include "zip.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <fstream>
#include <set>

namespace forge
{
  namespace
  {

    std::uint16_t read_u16(const std::uint8_t* value)
    {
      return static_cast<std::uint16_t>(value[0])
        | (static_cast<std::uint16_t>(value[1]) << 8);
    }

    std::uint32_t read_u32(const std::uint8_t* value)
    {
      return static_cast<std::uint32_t>(value[0])
        | (static_cast<std::uint32_t>(value[1]) << 8)
        | (static_cast<std::uint32_t>(value[2]) << 16)
        | (static_cast<std::uint32_t>(value[3]) << 24);
    }

    bool read_exact(std::ifstream& file, void* data, std::size_t size)
    {
      file.read(static_cast<char*>(data), static_cast<std::streamsize>(size));
      return static_cast<std::size_t>(file.gcount()) == size;
    }

    bool is_safe_entry(std::string_view entry)
    {
      if (entry.empty()
          || entry.front() == '/'
          || entry.find('\\') != std::string_view::npos
          || entry.find('\0') != std::string_view::npos)
      {
        return false;
      }

      if (entry.back() == '/')
      {
        entry.remove_suffix(1);
      }

      while (!entry.empty())
      {
        const auto slash = entry.find('/');
        const auto component = entry.substr(0, slash);

        if (component.empty() || component == "." || component == "..")
        {
          return false;
        }

        if (slash == std::string_view::npos)
        {
          break;
        }

        entry.remove_prefix(slash + 1);
      }

      return true;
    }

  } // namespace

  bool read_zip_entries(const std::filesystem::path& path,
                        std::vector<std::string>& entries,
                        std::ostream& error)
  {
    std::ifstream file { path, std::ios::binary };

    if (!file)
    {
      error << "forge: could not read box archive\n";
      return false;
    }

    file.seekg(0, std::ios::end);
    const auto size = file.tellg();

    if (size < 22)
    {
      error << "forge: box is not a valid ZIP archive\n";
      return false;
    }

    const auto search_size = static_cast<std::size_t>(std::min<std::streamoff>(size, 65557));
    std::vector<std::uint8_t> tail(search_size);
    file.seekg(size - static_cast<std::streamoff>(search_size));

    if (!read_exact(file, tail.data(), tail.size()))
    {
      error << "forge: could not read box archive\n";
      return false;
    }

    std::size_t end_offset = std::string::npos;

    for (std::size_t index = tail.size() - 22;; --index)
    {
      if (read_u32(tail.data() + index) == 0x06054b50)
      {
        end_offset = index;
        break;
      }

      if (index == 0)
      {
        break;
      }
    }

    if (end_offset == std::string::npos)
    {
      error << "forge: box is not a valid ZIP archive\n";
      return false;
    }

    const auto entry_count = read_u16(tail.data() + end_offset + 10);
    const auto central_size = read_u32(tail.data() + end_offset + 12);
    const auto central_offset = read_u32(tail.data() + end_offset + 16);

    if (entry_count == 0xffff || central_size == 0xffffffff || central_offset == 0xffffffff)
    {
      error << "forge: ZIP64 boxes are not supported\n";
      return false;
    }

    file.clear();
    file.seekg(central_offset);
    entries.clear();
    std::set<std::string> unique_entries;

    for (std::size_t index = 0; index < entry_count; ++index)
    {
      std::array<std::uint8_t, 46> header {};

      if (!read_exact(file, header.data(), header.size()) || read_u32(header.data()) != 0x02014b50)
      {
        error << "forge: box has an invalid ZIP directory\n";
        return false;
      }

      const auto name_size = read_u16(header.data() + 28);
      const auto extra_size = read_u16(header.data() + 30);
      const auto comment_size = read_u16(header.data() + 32);
      std::string name(name_size, '\0');

      if (!read_exact(file, name.data(), name.size()))
      {
        error << "forge: box has an invalid ZIP directory\n";
        return false;
      }

      if (!is_safe_entry(name))
      {
        error << "forge: box contains unsafe archive entry '" << name << "'\n";
        return false;
      }

      if (!unique_entries.insert(name).second)
      {
        error << "forge: box contains duplicate archive entry '" << name << "'\n";
        return false;
      }

      entries.push_back(name);
      file.seekg(static_cast<std::streamoff>(extra_size) + comment_size, std::ios::cur);
    }

    return true;
  }

} // namespace forge
