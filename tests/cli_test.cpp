#include "cli.h"

#include <array>
#include <iostream>
#include <sstream>
#include <string>
#include <string_view>

namespace
{

  int failures = 0;

  void expect(bool condition, std::string_view message)
  {
    if (!condition)
    {
      std::cerr << "FAIL: " << message << '\n';
      ++failures;
    }
  }

  bool contains(const std::string& text, std::string_view fragment)
  {
    return text.find(fragment) != std::string::npos;
  }

  void test_help()
  {
    std::ostringstream output;
    std::ostringstream error;

    expect(forge::cli::run({}, output, error) == 0, "empty arguments show help");
    expect(contains(output.str(), "forge <command>"), "help includes usage");
    expect(error.str().empty(), "help does not write an error");
  }

  void test_version()
  {
    constexpr std::array arguments { std::string_view { "--version" } };
    std::ostringstream output;
    std::ostringstream error;

    expect(forge::cli::run(arguments, output, error) == 0, "version succeeds");
    expect(contains(output.str(), forge::cli::version), "version reports the current version");
  }

  void test_planned_command()
  {
    constexpr std::array arguments { std::string_view { "build" } };
    std::ostringstream output;
    std::ostringstream error;

    expect(forge::cli::run(arguments, output, error) == 2, "planned command reports unavailable");
    expect(contains(error.str(), "not implemented yet"), "planned command explains its state");
  }

  void test_unknown_command()
  {
    constexpr std::array arguments { std::string_view { "confuse" } };
    std::ostringstream output;
    std::ostringstream error;

    expect(forge::cli::run(arguments, output, error) == 2, "unknown command fails");
    expect(contains(error.str(), "unknown command"), "unknown command has a useful error");
  }

} // namespace

int main()
{
  test_help();
  test_version();
  test_planned_command();
  test_unknown_command();

  return failures == 0 ? 0 : 1;
}
