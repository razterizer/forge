#include "build.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace
{

  int failures = 0;

  class TemporaryDirectory
  {
  public:
    TemporaryDirectory()
    {
      const auto suffix = std::chrono::steady_clock::now().time_since_epoch().count();
      path_ = std::filesystem::temp_directory_path() / ("forge-build-test-" + std::to_string(suffix));
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

  void write_project(const std::filesystem::path& directory)
  {
    std::ofstream recipe { directory / "forge.recipe.toml" };
    recipe
      << "[project]\n"
      << "name = \"hello\"\n"
      << "version = \"0.1.0\"\n"
      << "type = \"executable\"\n"
      << "cpp_std = 20\n\n"
      << "[sources]\n"
      << "paths = [\"main.cpp\"]\n";

    std::ofstream source { directory / "main.cpp" };
    source << "int main() {}\n";
  }

  void write_library_project(const std::filesystem::path& directory)
  {
    std::filesystem::create_directories(directory / "src");
    std::filesystem::create_directories(directory / "include/hello");
    std::ofstream recipe { directory / "forge.recipe.toml" };
    recipe
      << "[project]\n"
      << "name = \"hello\"\n"
      << "version = \"0.1.0\"\n"
      << "type = \"static_library\"\n"
      << "cpp_std = 20\n\n"
      << "[sources]\n"
      << "paths = [\"src/hello.cpp\"]\n"
      << "public_headers = [\"include/hello/hello.h\"]\n";

    std::ofstream { directory / "include/hello/hello.h" } << "int hello();\n";
    std::ofstream { directory / "src/hello.cpp" } << "#include <hello/hello.h>\nint hello() { return 42; }\n";
  }

  void test_build_generates_cmake_and_commands()
  {
    TemporaryDirectory directory;
    write_project(directory.path());
    std::vector<std::vector<std::string>> commands;
    std::ostringstream output;
    std::ostringstream error;

    const forge::ProcessRunner runner =
      [&commands](const std::vector<std::string>& arguments,
                  const std::filesystem::path&,
                  std::ostream&)
      {
        commands.push_back(arguments);
        return 0;
      };

    expect(
      forge::build_project(directory.path(), runner, output, error) == 0,
      "build succeeds when generated commands succeed"
    );
    expect(commands.size() == 2, "build invokes configure and build commands");
    expect(commands[0].size() > 1 && commands[0][0] == "cmake", "configure uses CMake");
    expect(commands[1].size() > 2 && commands[1][1] == "--build", "build uses CMake build mode");

    const auto generated = read_file(directory.path() / ".forge/generated/CMakeLists.txt");
    expect(contains(generated, "add_executable(forge_project"), "build generates an executable target");
    expect(contains(generated, "main.cpp"), "build includes recipe sources");
    expect(contains(generated, "cxx_std_20"), "build includes the requested C++ standard");
    expect(error.str().empty(), "successful unit build does not write an error");
  }

  void test_build_stops_after_configuration_failure()
  {
    TemporaryDirectory directory;
    write_project(directory.path());
    int invocations = 0;
    std::ostringstream output;
    std::ostringstream error;

    const forge::ProcessRunner runner =
      [&invocations](const std::vector<std::string>&,
                     const std::filesystem::path&,
                     std::ostream&)
      {
        ++invocations;
        return 1;
      };

    expect(
      forge::build_project(directory.path(), runner, output, error) == 2,
      "build reports configuration failure"
    );
    expect(invocations == 1, "build stops after configuration failure");
    expect(contains(error.str(), "configuration failed"), "build explains configuration failure");
  }

  void test_build_generates_static_library()
  {
    TemporaryDirectory directory;
    write_library_project(directory.path());
    std::ostringstream output;
    std::ostringstream error;

    const forge::ProcessRunner runner =
      [](const std::vector<std::string>&,
         const std::filesystem::path&,
         std::ostream&)
      {
        return 0;
      };

    expect(
      forge::build_project(directory.path(), runner, output, error) == 0,
      "static library build succeeds"
    );

    const auto generated = read_file(directory.path() / ".forge/generated/CMakeLists.txt");
    expect(contains(generated, "add_library(forge_project STATIC"), "build generates a static library target");
    expect(contains(generated, "include/hello/hello.h"), "build includes public headers");
    expect(contains(generated, "target_include_directories"), "build exposes the include directory");
    expect(contains(output.str(), "libhello.a"), "build reports the static library artifact");
  }

  void test_build_rejects_missing_source_without_running_process()
  {
    TemporaryDirectory directory;
    write_project(directory.path());
    std::filesystem::remove(directory.path() / "main.cpp");
    int invocations = 0;
    std::ostringstream output;
    std::ostringstream error;

    const forge::ProcessRunner runner =
      [&invocations](const std::vector<std::string>&,
                     const std::filesystem::path&,
                     std::ostream&)
      {
        ++invocations;
        return 0;
      };

    expect(
      forge::build_project(directory.path(), runner, output, error) == 2,
      "build rejects a missing source"
    );
    expect(invocations == 0, "invalid recipes do not invoke external tools");
    expect(contains(error.str(), "does not exist"), "build explains the missing source");
  }

} // namespace

int main()
{
  test_build_generates_cmake_and_commands();
  test_build_generates_static_library();
  test_build_stops_after_configuration_failure();
  test_build_rejects_missing_source_without_running_process();

  return failures == 0 ? 0 : 1;
}
