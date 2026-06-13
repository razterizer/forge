#pragma once

#include <cctype>
#include <charconv>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace forge
{

  struct InitialVersion
  {
    std::string version;
    std::optional<int> build_number;
  };

  inline std::optional<InitialVersion> parse_initial_version(std::string_view value)
  {
    std::vector<std::string_view> components;

    while (true)
    {
      const auto separator = value.find('.');
      components.push_back(value.substr(0, separator));

      if (separator == std::string_view::npos)
      {
        break;
      }

      value.remove_prefix(separator + 1);
    }

    if (components.size() != 3 && components.size() != 4)
    {
      return std::nullopt;
    }

    std::vector<int> numbers;

    for (const auto component : components)
    {
      int number = 0;
      const auto parsed =
        std::from_chars(component.data(), component.data() + component.size(), number);

      if (component.empty()
          || (component.size() > 1 && component.front() == '0')
          || parsed.ec != std::errc {}
          || parsed.ptr != component.data() + component.size()
          || number < 0)
      {
        return std::nullopt;
      }

      numbers.push_back(number);
    }

    InitialVersion result;
    result.version =
      std::to_string(numbers[0]) + "." + std::to_string(numbers[1]) + "."
      + std::to_string(numbers[2]);

    if (numbers.size() == 4)
    {
      result.build_number = numbers[3];
    }

    return result;
  }

  inline std::string version_macro_prefix(std::string_view project_name)
  {
    std::string prefix;

    for (const auto character : project_name)
    {
      prefix += std::isalnum(static_cast<unsigned char>(character))
        ? static_cast<char>(std::toupper(static_cast<unsigned char>(character)))
        : '_';
    }

    if (prefix.empty() || std::isdigit(static_cast<unsigned char>(prefix.front())))
    {
      prefix.insert(prefix.begin(), '_');
    }

    return prefix;
  }

  inline std::string qualified_initial_version(const InitialVersion& version)
  {
    return version.build_number
      ? version.version + "." + std::to_string(*version.build_number)
      : version.version;
  }

  inline std::string generated_version_header(std::string_view prefix,
                                              const InitialVersion& version)
  {
    const auto first = version.version.find('.');
    const auto second = version.version.find('.', first + 1);
    const auto qualified = qualified_initial_version(version);
    const auto macro = std::string { prefix } + "_VERSION_";
    return
      "#pragma once\n"
      "#define " + macro + "STR \"" + qualified + "\"\n"
      "#define " + macro + "MAJOR " + version.version.substr(0, first) + "\n"
      "#define " + macro + "MINOR " + version.version.substr(first + 1, second - first - 1) + "\n"
      "#define " + macro + "PATCH " + version.version.substr(second + 1) + "\n"
      "#define " + macro + "BUILD " + std::to_string(version.build_number.value_or(0)) + "\n";
  }

} // namespace forge
