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

  void write_header_only_project(const std::filesystem::path& directory)
  {
    std::filesystem::create_directories(directory / "include/hello");
    std::ofstream recipe { directory / "forge.recipe.toml" };
    recipe
      << "[project]\n"
      << "name = \"hello\"\n"
      << "version = \"0.1.0\"\n"
      << "type = \"header_only\"\n"
      << "cpp_std = 20\n\n"
      << "[sources]\n"
      << "paths = []\n"
      << "public_headers = [\"include/hello/hello.h\"]\n";

    std::ofstream { directory / "include/hello/hello.h" } << "inline int hello() { return 42; }\n";
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

  void test_build_validates_header_only_project()
  {
    TemporaryDirectory directory;
    write_header_only_project(directory.path());
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
      "header-only build succeeds"
    );
    expect(commands.size() == 2, "header-only build configures and compiles validation sources");

    const auto generated = read_file(directory.path() / ".forge/generated/CMakeLists.txt");
    const auto validation = read_file(directory.path() / ".forge/generated/header-validation/header-0.cpp");
    expect(contains(generated, "add_library(forge_project OBJECT"), "header-only build generates validation target");
    expect(contains(validation, "#include <hello/hello.h>"), "header-only build generates a header include");
    expect(contains(output.str(), "Validated 1 public header"), "header-only build reports validation");
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

  void test_build_rejects_dependency_name_mismatch()
  {
    TemporaryDirectory directory;
    const auto dependency = directory.path() / "dependency";
    const auto application = directory.path() / "application";
    std::filesystem::create_directories(dependency / "include/actual");
    std::filesystem::create_directories(application);
    std::ofstream dependency_recipe { dependency / "forge.recipe.toml" };
    dependency_recipe
      << "[project]\n"
      << "name = \"actual\"\n"
      << "version = \"1.0.0\"\n"
      << "type = \"header_only\"\n"
      << "cpp_std = 20\n\n"
      << "[sources]\n"
      << "paths = []\n"
      << "public_headers = [\"include/actual/actual.h\"]\n";
    dependency_recipe.close();
    std::ofstream { dependency / "include/actual/actual.h" } << "inline void actual() {}\n";
    std::ofstream application_recipe { application / "forge.recipe.toml" };
    application_recipe
      << "[project]\n"
      << "name = \"application\"\n"
      << "version = \"1.0.0\"\n"
      << "type = \"executable\"\n"
      << "cpp_std = 20\n\n"
      << "[sources]\n"
      << "paths = [\"main.cpp\"]\n\n"
      << "[dependencies]\n"
      << "expected = { path = \"../dependency\" }\n";
    application_recipe.close();
    std::ofstream { application / "main.cpp" } << "int main() {}\n";
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
      forge::build_project(application, runner, output, error) == 2,
      "build rejects a dependency name mismatch"
    );
    expect(invocations == 0, "invalid dependencies do not invoke external tools");
    expect(contains(error.str(), "does not match"), "build explains dependency name mismatch");
  }

  void test_build_rejects_dependency_cycle()
  {
    TemporaryDirectory directory;
    const auto application = directory.path() / "application";
    const auto first = directory.path() / "first";
    const auto second = directory.path() / "second";
    std::filesystem::create_directories(application);
    std::filesystem::create_directories(first / "include/first");
    std::filesystem::create_directories(second / "include/second");
    std::ofstream application_recipe { application / "forge.recipe.toml" };
    application_recipe
      << "[project]\n"
      << "name = \"application\"\n"
      << "version = \"1.0.0\"\n"
      << "type = \"executable\"\n"
      << "cpp_std = 20\n\n"
      << "[sources]\n"
      << "paths = [\"main.cpp\"]\n\n"
      << "[dependencies]\n"
      << "first = { path = \"../first\" }\n";
    application_recipe.close();
    std::ofstream { application / "main.cpp" } << "int main() {}\n";
    std::ofstream first_recipe { first / "forge.recipe.toml" };
    first_recipe
      << "[project]\n"
      << "name = \"first\"\n"
      << "version = \"1.0.0\"\n"
      << "type = \"header_only\"\n"
      << "cpp_std = 20\n\n"
      << "[sources]\n"
      << "paths = []\n"
      << "public_headers = [\"include/first/first.h\"]\n\n"
      << "[dependencies]\n"
      << "second = { path = \"../second\" }\n";
    first_recipe.close();
    std::ofstream { first / "include/first/first.h" } << "#pragma once\n";
    std::ofstream second_recipe { second / "forge.recipe.toml" };
    second_recipe
      << "[project]\n"
      << "name = \"second\"\n"
      << "version = \"1.0.0\"\n"
      << "type = \"header_only\"\n"
      << "cpp_std = 20\n\n"
      << "[sources]\n"
      << "paths = []\n"
      << "public_headers = [\"include/second/second.h\"]\n\n"
      << "[dependencies]\n"
      << "first = { path = \"../first\" }\n";
    second_recipe.close();
    std::ofstream { second / "include/second/second.h" } << "#pragma once\n";
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
      forge::build_project(application, runner, output, error) == 2,
      "build rejects a dependency cycle"
    );
    expect(invocations == 0, "dependency cycles do not invoke external tools");
    expect(contains(error.str(), "cycle detected"), "build explains the dependency cycle");
  }

} // namespace

int main()
{
  test_build_generates_cmake_and_commands();
  test_build_generates_static_library();
  test_build_validates_header_only_project();
  test_build_stops_after_configuration_failure();
  test_build_rejects_missing_source_without_running_process();
  test_build_rejects_dependency_name_mismatch();
  test_build_rejects_dependency_cycle();

  return failures == 0 ? 0 : 1;
}
