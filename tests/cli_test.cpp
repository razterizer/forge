#include "cli.h"

#include <array>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <sstream>
#include <string>
#include <string_view>

namespace
{

  int failures = 0;

  class TemporaryDirectory
  {
  public:
    TemporaryDirectory()
    {
      const auto suffix = std::chrono::steady_clock::now().time_since_epoch().count();
      path_ = std::filesystem::temp_directory_path() / ("forge-test-" + std::to_string(suffix));
      std::filesystem::create_directories(path_);
    }

    ~TemporaryDirectory()
    {
      std::error_code error;
      std::filesystem::remove_all(path_, error);
    }

    const std::filesystem::path& path() const
    {
      return path_;
    }

  private:
    std::filesystem::path path_;
  };

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

  std::string read_file(const std::filesystem::path& path)
  {
    std::ifstream file { path };
    return {
      std::istreambuf_iterator<char> { file },
      std::istreambuf_iterator<char> {}
    };
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

  void test_init()
  {
    TemporaryDirectory directory;
    constexpr std::array arguments { std::string_view { "init" } };
    std::ostringstream output;
    std::ostringstream error;

    expect(
      forge::cli::run(arguments, directory.path(), output, error) == 0,
      "init succeeds"
    );
    expect(
      std::filesystem::exists(directory.path() / "forge.recipe.toml"),
      "init creates a recipe"
    );
    expect(
      std::filesystem::exists(directory.path() / "src/main.cpp"),
      "init creates a source file"
    );
    expect(
      contains(read_file(directory.path() / "forge.recipe.toml"), directory.path().filename().string()),
      "recipe contains the project name"
    );
    expect(error.str().empty(), "init does not write an error");
  }

  void test_init_refuses_to_overwrite()
  {
    TemporaryDirectory directory;
    constexpr std::array arguments { std::string_view { "init" } };
    std::ostringstream first_output;
    std::ostringstream first_error;
    std::ostringstream second_output;
    std::ostringstream second_error;

    forge::cli::run(arguments, directory.path(), first_output, first_error);
    const auto original_recipe = read_file(directory.path() / "forge.recipe.toml");

    expect(
      forge::cli::run(arguments, directory.path(), second_output, second_error) == 2,
      "init refuses to overwrite an existing recipe"
    );
    expect(
      read_file(directory.path() / "forge.recipe.toml") == original_recipe,
      "init preserves an existing recipe"
    );
    expect(contains(second_error.str(), "already exists"), "init explains overwrite refusal");
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
  test_init();
  test_init_refuses_to_overwrite();
  test_unknown_command();

  return failures == 0 ? 0 : 1;
}
