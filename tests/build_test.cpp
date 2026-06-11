#include "build.h"

#include <algorithm>
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

  void write_multi_target_project(const std::filesystem::path& directory)
  {
    std::filesystem::create_directories(directory / "Examples");
    std::filesystem::create_directories(directory / "Tests");
    std::filesystem::create_directories(directory / "include/hello");
    std::ofstream recipe { directory / "forge.recipe.toml" };
    recipe
      << "[project]\n"
      << "name = \"hello-suite\"\n"
      << "version = \"0.1.0\"\n\n"
      << "[target.hello]\n"
      << "type = \"header_only\"\n"
      << "cpp_std = 20\n"
      << "sources = []\n"
      << "public_headers = [\"include/hello/hello.h\"]\n"
      << "include_dirs = [\"include/hello\"]\n\n"
      << "[target.examples]\n"
      << "type = \"executable\"\n"
      << "cpp_std = 20\n"
      << "sources = [\"Examples/examples.cpp\"]\n"
      << "dependencies = [\"hello\"]\n\n"
      << "[target.unit_tests]\n"
      << "type = \"executable\"\n"
      << "cpp_std = 20\n"
      << "sources = [\"Tests/unit_tests.cpp\"]\n"
      << "dependencies = [\"hello\"]\n";

    std::ofstream { directory / "include/hello/hello.h" }
      << "inline int hello() { return 42; }\n";
    std::ofstream { directory / "Examples/examples.cpp" }
      << "#include <hello/hello.h>\nint main() { return hello() == 42 ? 0 : 1; }\n";
    std::ofstream { directory / "Tests/unit_tests.cpp" }
      << "#include <hello/hello.h>\nint main() { return hello() == 42 ? 0 : 1; }\n";
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
    expect(
      commands[0].back().find('\\') == std::string::npos,
      "configure passes the project root using CMake path separators"
    );

    const auto generated = read_file(directory.path() / ".forge/generated/CMakeLists.txt");
    expect(contains(generated, "add_executable(forge_project"), "build generates an executable target");
    expect(contains(generated, "forge-toolchain.toml"), "build records the selected toolchain identity");
    expect(contains(generated, "CMAKE_CXX_COMPILER_ID"), "toolchain identity records the compiler");
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

  void test_build_generates_recipe_and_cli_definitions()
  {
    TemporaryDirectory directory;
    write_project(directory.path());
    std::ofstream recipe { directory.path() / "forge.recipe.toml", std::ios::app };
    recipe
      << "\n[build]\n"
      << "defines = [\"RECIPE_FEATURE\", \"RECIPE_VALUE=42\"]\n";
    recipe.close();
    forge::BuildOptions options;
    options.compile_definitions = { "CLI_FEATURE", "CLI_VALUE=hello/world" };
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
      forge::build_project(directory.path(), options, runner, output, error) == 0,
      "build succeeds with recipe and CLI definitions"
    );
    const auto generated = read_file(directory.path() / ".forge/generated/CMakeLists.txt");
    expect(contains(generated, "\"RECIPE_FEATURE\""), "build generates a recipe definition");
    expect(contains(generated, "\"RECIPE_VALUE=42\""), "build generates a valued recipe definition");
    expect(contains(generated, "\"CLI_FEATURE\""), "build generates a CLI definition");
    expect(contains(generated, "\"CLI_VALUE=hello/world\""), "build preserves CLI definition values");
    expect(error.str().empty(), "definition build does not write an error");
  }

  void test_build_generates_named_target_definitions()
  {
    TemporaryDirectory directory;
    write_multi_target_project(directory.path());
    std::ofstream recipe { directory.path() / "forge.recipe.toml", std::ios::app };
    recipe
      << "\n[target.defined]\n"
      << "type = \"executable\"\n"
      << "cpp_std = 20\n"
      << "sources = [\"Examples/examples.cpp\"]\n"
      << "defines = [\"NAMED_FEATURE\"]\n";
    recipe.close();
    forge::BuildOptions options;
    options.target = "defined";
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
      forge::build_project(directory.path(), options, runner, output, error) == 0,
      "named target build succeeds with definitions"
    );
    expect(
      contains(
        read_file(directory.path() / ".forge/generated/defined/CMakeLists.txt"),
        "\"NAMED_FEATURE\""
      ),
      "build generates a named target definition"
    );
  }

  void test_build_rejects_invalid_recipe_definition()
  {
    TemporaryDirectory directory;
    write_project(directory.path());
    std::ofstream recipe { directory.path() / "forge.recipe.toml", std::ios::app };
    recipe
      << "\n[build]\n"
      << "defines = [\"NOT-VALID\"]\n";
    recipe.close();
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
      "build rejects an invalid recipe definition"
    );
    expect(invocations == 0, "invalid recipe definition does not invoke external tools");
    expect(contains(error.str(), "invalid recipe value"), "invalid recipe definition is explained");
  }

  void test_build_applies_build_profile()
  {
    TemporaryDirectory directory;
    write_project(directory.path());
    std::filesystem::create_directories(directory.path() / "include");
    std::ofstream recipe { directory.path() / "forge.recipe.toml", std::ios::app };
    recipe
      << "\n[profile.Release.build]\n"
      << "configuration = \"Release\"\n"
      << "cpp_std = 23\n"
      << "include_dirs = [\"include\"]\n"
      << "defines = [\"NDEBUG\", \"PROFILE_VALUE=42\"]\n";
    recipe.close();
    forge::BuildOptions options;
    options.profile = "Release";
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
      forge::build_project(directory.path(), options, runner, output, error) == 0,
      "build succeeds with a build profile"
    );
    const auto generated = read_file(directory.path() / ".forge/generated/CMakeLists.txt");
    expect(contains(generated, "cxx_std_23"), "build profile overrides the C++ standard");
    expect(contains(generated, "\"NDEBUG\""), "build profile adds a definition");
    expect(contains(generated, "include\""), "build profile adds an include directory");
    expect(
      commands.size() == 2
        && std::find(commands[0].begin(), commands[0].end(), "-DCMAKE_BUILD_TYPE=Release")
          != commands[0].end(),
      "build profile selects the configure-time CMake configuration"
    );
    expect(
      commands.size() == 2
        && commands[1].size() >= 5
        && commands[1][3] == "--config"
        && commands[1][4] == "Release",
      "build profile selects the build-time CMake configuration"
    );
    expect(error.str().empty(), "successful build profile does not write an error");
  }

  void test_build_selects_named_target()
  {
    TemporaryDirectory directory;
    write_multi_target_project(directory.path());
    forge::BuildOptions options;
    options.target = "examples";
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
      forge::build_project(directory.path(), options, runner, output, error) == 0,
      "build succeeds for a selected named target"
    );
    expect(
      commands.size() == 2
        && std::filesystem::path { commands[0][4] }.filename() == "examples"
        && std::filesystem::path { commands[0][4] }.parent_path().filename() == "build",
      "named target uses an isolated build directory"
    );
    const auto generated = read_file(directory.path() / ".forge/generated/examples/CMakeLists.txt");
    expect(contains(generated, "Examples/examples.cpp"), "named target includes its selected source");
    expect(!contains(generated, "Tests/unit_tests.cpp"), "named target excludes other target sources");
    expect(
      contains(generated, "add_library(forge_internal_0 INTERFACE)"),
      "build generates internal header-only target"
    );
    expect(
      contains(
        generated,
        "target_include_directories(forge_internal_0 INTERFACE "
        "\"${FORGE_PROJECT_ROOT}/include/hello\")"
      ),
      "build exposes adopted header-only include directories as interface properties"
    );
    expect(
      contains(generated, "target_link_libraries(forge_project PRIVATE forge_internal_0)"),
      "selected target links its internal dependency"
    );
    expect(contains(output.str(), "Building examples"), "build reports the selected target");
    expect(error.str().empty(), "selected named target build does not write an error");
  }

  void test_build_requires_target_for_multi_target_recipe()
  {
    TemporaryDirectory directory;
    write_multi_target_project(directory.path());
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
      "build requires a target for a multi-target recipe"
    );
    expect(invocations == 0, "ambiguous multi-target build does not invoke external tools");
    expect(contains(error.str(), "specify one of"), "ambiguous multi-target build lists choices");
  }

  void test_build_rejects_missing_internal_target()
  {
    TemporaryDirectory directory;
    write_multi_target_project(directory.path());
    std::ofstream recipe { directory.path() / "forge.recipe.toml", std::ios::app };
    recipe
      << "\n[target.broken]\n"
      << "type = \"executable\"\n"
      << "cpp_std = 20\n"
      << "sources = [\"Examples/examples.cpp\"]\n"
      << "dependencies = [\"missing\"]\n";
    recipe.close();
    forge::BuildOptions options;
    options.target = "broken";
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
      forge::build_project(directory.path(), options, runner, output, error) == 2,
      "build rejects a missing internal target"
    );
    expect(invocations == 0, "missing internal target does not invoke external tools");
    expect(contains(error.str(), "missing internal target"), "missing internal target is explained");
  }

  void test_build_rejects_internal_target_cycle()
  {
    TemporaryDirectory directory;
    std::filesystem::create_directories(directory.path() / "include/suite");
    std::ofstream { directory.path() / "include/suite/a.h" };
    std::ofstream { directory.path() / "include/suite/b.h" };
    std::ofstream { directory.path() / "main.cpp" } << "int main() {}\n";
    std::ofstream recipe { directory.path() / "forge.recipe.toml" };
    recipe
      << "[project]\nname = \"suite\"\nversion = \"0.1.0\"\n\n"
      << "[target.a]\ntype = \"header_only\"\ncpp_std = 20\nsources = []\n"
      << "public_headers = [\"include/suite/a.h\"]\ndependencies = [\"b\"]\n\n"
      << "[target.b]\ntype = \"header_only\"\ncpp_std = 20\nsources = []\n"
      << "public_headers = [\"include/suite/b.h\"]\ndependencies = [\"a\"]\n\n"
      << "[target.app]\ntype = \"executable\"\ncpp_std = 20\nsources = [\"main.cpp\"]\n"
      << "dependencies = [\"a\"]\n";
    recipe.close();
    forge::BuildOptions options;
    options.target = "app";
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
      forge::build_project(directory.path(), options, runner, output, error) == 2,
      "build rejects an internal target dependency cycle"
    );
    expect(contains(error.str(), "cycle detected"), "internal target cycle is explained");
  }

  void test_build_stages_runtime_assets()
  {
    TemporaryDirectory directory;
    write_project(directory.path());
    std::filesystem::create_directories(directory.path() / "assets");
    std::filesystem::create_directories(directory.path() / "Examples");
    std::ofstream { directory.path() / "assets/message.txt" } << "hello\n";
    std::ofstream { directory.path() / "Examples/Blocks.txt" } << "blocks\n";
    std::ofstream recipe { directory.path() / "forge.recipe.toml", std::ios::app };
    recipe
      << "\n[runtime]\n"
      << "files = [\"assets\", "
      << "{ source = \"Examples/Blocks.txt\", destination = \"Blocks.txt\" }]\n";
    recipe.close();
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
      "build succeeds with runtime assets"
    );
    expect(
      read_file(directory.path() / ".forge/build/assets/message.txt") == "hello\n",
      "build stages runtime assets beside the executable"
    );
    expect(
      read_file(directory.path() / ".forge/build/Blocks.txt") == "blocks\n",
      "build stages mapped runtime assets at their requested destination"
    );

    std::filesystem::remove(directory.path() / "assets/message.txt");
    std::ofstream { directory.path() / "assets/replacement.txt" } << "replacement\n";
    output.str({});
    error.str({});

    expect(
      forge::build_project(directory.path(), runner, output, error) == 0,
      "second build succeeds after runtime assets change"
    );
    expect(
      !std::filesystem::exists(directory.path() / ".forge/build/assets/message.txt"),
      "build removes stale runtime assets"
    );
    expect(
      std::filesystem::exists(directory.path() / ".forge/build/assets/replacement.txt"),
      "build stages replacement runtime assets"
    );
  }

  void test_build_rejects_runtime_asset_collision()
  {
    TemporaryDirectory directory;
    write_project(directory.path());
    std::ofstream { directory.path() / "hello" } << "asset\n";
    std::ofstream recipe { directory.path() / "forge.recipe.toml", std::ios::app };
    recipe
      << "\n[runtime]\n"
      << "files = [\"hello\"]\n";
    recipe.close();
    std::ostringstream output;
    std::ostringstream error;

    const forge::ProcessRunner runner =
      [&directory](const std::vector<std::string>& arguments,
                   const std::filesystem::path&,
                   std::ostream&)
      {
        if (arguments.size() > 1 && arguments[1] == "--build")
        {
          std::filesystem::create_directories(directory.path() / ".forge/build");
          std::ofstream { directory.path() / ".forge/build/hello" } << "executable\n";
        }

        return 0;
      };

    expect(
      forge::build_project(directory.path(), runner, output, error) == 2,
      "build rejects a runtime asset that collides with build output"
    );
    expect(contains(error.str(), "collides"), "build explains the runtime asset collision");
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
#ifdef _WIN32
    expect(contains(generated, "PREFIX \"\" SUFFIX \".lib\""), "build standardizes the Windows library name");
    expect(contains(output.str(), "hello.lib"), "build reports the static library artifact");
#else
    expect(contains(output.str(), "libhello.a"), "build reports the static library artifact");
#endif
  }

  void test_build_generates_dynamic_library()
  {
    TemporaryDirectory directory;
    write_library_project(directory.path());
    std::ofstream recipe { directory.path() / "forge.recipe.toml" };
    recipe
      << "[project]\n"
      << "name = \"hello\"\n"
      << "version = \"0.1.0\"\n"
      << "type = \"dynamic_library\"\n"
      << "cpp_std = 20\n\n"
      << "[sources]\n"
      << "paths = [\"src/hello.cpp\"]\n"
      << "public_headers = [\"include/hello/hello.h\"]\n";
    recipe.close();
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
      "dynamic library build succeeds"
    );

    const auto generated = read_file(directory.path() / ".forge/generated/CMakeLists.txt");
    expect(contains(generated, "add_library(forge_project SHARED"), "build generates a dynamic library target");
#ifdef __APPLE__
    expect(contains(generated, "INSTALL_RPATH \"@loader_path\""), "dynamic library searches its own directory");
#elif defined(__linux__)
    expect(contains(generated, "INSTALL_RPATH \"$ORIGIN\""), "dynamic library searches its own directory");
#elif defined(_WIN32)
    expect(contains(generated, "IMPORT_PREFIX \"\" IMPORT_SUFFIX \".lib\""), "dynamic library generates a standard import library");
    expect(contains(generated, "WINDOWS_EXPORT_ALL_SYMBOLS TRUE"), "dynamic library exports symbols on Windows");
    expect(contains(output.str(), "hello.dll"), "build reports the dynamic library artifact");
#endif
  }

  void test_build_accepts_legacy_shared_library_alias()
  {
    TemporaryDirectory directory;
    write_library_project(directory.path());
    std::ofstream recipe { directory.path() / "forge.recipe.toml" };
    recipe
      << "[project]\n"
      << "name = \"hello\"\n"
      << "version = \"0.1.0\"\n"
      << "type = \"shared_library\"\n"
      << "cpp_std = 20\n\n"
      << "[sources]\n"
      << "paths = [\"src/hello.cpp\"]\n"
      << "public_headers = [\"include/hello/hello.h\"]\n";
    recipe.close();
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
      "build accepts the legacy shared_library alias"
    );
    expect(
      contains(read_file(directory.path() / ".forge/generated/CMakeLists.txt"), " SHARED"),
      "legacy shared_library alias generates a dynamic library"
    );
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

  void test_update_resolves_github_dependency()
  {
    TemporaryDirectory directory;
    write_project(directory.path());
    std::ofstream recipe { directory.path() / "forge.recipe.toml", std::ios::app };
    recipe
      << "\n[dependencies]\n"
      << "answer = { github = \"example/answer\", version = \"1.2.3+build.6\" }\n";
    recipe.close();
    std::vector<std::vector<std::string>> commands;
    std::ostringstream output;
    std::ostringstream error;
    const std::string checksum =
      "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855";

#ifdef _WIN32
    const std::string target = "windows-x86_64";
#elif __APPLE__
    const std::string target = "macos-arm64";
#else
    const std::string target = "linux-x86_64";
#endif

    const auto asset = "answer-1.2.3+build.6-" + target + ".cbox";
    const std::string release_url =
      "https://github.com/example/answer/releases/download/release-1.2.3.6/";
    std::ofstream existing_lock { directory.path() / "forge.lock.toml" };
    existing_lock
      << "format = 1\n\n"
      << "[[dependency]]\n"
      << "name = \"answer\"\n"
      << "github = \"example/answer\"\n"
      << "version = \"1.2.3+build.6\"\n"
      << "target = \"other-target\"\n"
      << "url = \"https://example.invalid/answer.cbox\"\n"
      << "sha256 = \"" << checksum << "\"\n";
    existing_lock.close();
    const auto original_lock = read_file(directory.path() / "forge.lock.toml");

    const forge::ProcessRunner runner =
      [&commands, &checksum, &asset](const std::vector<std::string>& arguments,
                                    const std::filesystem::path&,
                                    std::ostream&)
      {
        commands.push_back(arguments);

        if (arguments.size() > 2
            && arguments[1].starts_with("-DURL=")
            && arguments[1].ends_with(".sha256"))
        {
          const auto destination = arguments[2].substr(std::string { "-DDESTINATION=" }.size());
          std::ofstream { destination } << checksum << "  " << asset << '\n';
          return 0;
        }

        if (arguments.size() > 2 && arguments[1].starts_with("-DURL="))
        {
          const auto destination = arguments[2].substr(std::string { "-DDESTINATION=" }.size());
          std::ofstream { destination };
          return 0;
        }

        return 1;
      };

    forge::BuildOptions options;
    options.update_dependencies = true;
    expect(
      forge::build_project(directory.path(), options, runner, output, error) == 2,
      "GitHub dependency update reaches box validation"
    );
    expect(commands.size() >= 2, "GitHub dependency downloads checksum before box");

    if (commands.size() >= 2)
    {
      expect(
        commands[0][1] == "-DURL=" + release_url + asset + ".sha256",
        "GitHub dependency resolves the checksum asset URL"
      );
      expect(
        commands[1][1] == "-DURL=" + release_url + asset,
        "GitHub dependency resolves the box asset URL"
      );
    }

    expect(
      read_file(directory.path() / "forge.lock.toml") == original_lock,
      "failed GitHub dependency update preserves the lockfile"
    );
  }

  void test_update_resolves_github_component_dependency()
  {
    TemporaryDirectory directory;
    write_project(directory.path());
    std::ofstream recipe { directory.path() / "forge.recipe.toml", std::ios::app };
    recipe
      << "\n[dependencies]\n"
      << "answer = { github = \"example/suite\", package = \"suite\", "
         "component = \"answer\", version = \"1.2.3\" }\n";
    recipe.close();
    std::vector<std::vector<std::string>> commands;
    std::ostringstream output;
    std::ostringstream error;
    const std::string checksum =
      "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855";

#ifdef _WIN32
    const std::string target = "windows-x86_64";
#elif __APPLE__
    const std::string target = "macos-arm64";
#else
    const std::string target = "linux-x86_64";
#endif

    const auto asset = "suite-1.2.3-" + target + ".cbox";
    const forge::ProcessRunner runner =
      [&commands, &checksum, &asset](const std::vector<std::string>& arguments,
                                    const std::filesystem::path&,
                                    std::ostream&)
      {
        commands.push_back(arguments);

        if (arguments.size() > 2
            && arguments[1].starts_with("-DURL=")
            && arguments[1].ends_with(".sha256"))
        {
          const auto destination = arguments[2].substr(std::string { "-DDESTINATION=" }.size());
          std::ofstream { destination } << checksum << "  " << asset << '\n';
          return 0;
        }

        if (arguments.size() > 2 && arguments[1].starts_with("-DURL="))
        {
          const auto destination = arguments[2].substr(std::string { "-DDESTINATION=" }.size());
          std::ofstream { destination };
          return 0;
        }

        return 1;
      };

    forge::BuildOptions options;
    options.update_dependencies = true;
    expect(
      forge::build_project(directory.path(), options, runner, output, error) == 2,
      "GitHub component dependency update reaches aggregate box validation"
    );
    expect(
      commands.size() >= 2
      && commands[0][1].ends_with("/" + asset + ".sha256")
      && commands[1][1].ends_with("/" + asset),
      "GitHub component dependency resolves the enclosing package asset"
    );
  }

  void test_build_requires_and_uses_locked_github_dependency()
  {
    TemporaryDirectory directory;
    write_project(directory.path());
    std::ofstream recipe { directory.path() / "forge.recipe.toml", std::ios::app };
    recipe
      << "\n[dependencies]\n"
      << "answer = { github = \"example/answer\", version = \"1.2.3\" }\n";
    recipe.close();
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
      "build rejects an unlocked GitHub dependency"
    );
    expect(invocations == 0, "unlocked GitHub dependency does not access the network");
    expect(contains(error.str(), "run forge update answer"), "unlocked dependency explains how to resolve it");

#ifdef _WIN32
    const std::string target = "windows-x86_64";
#elif __APPLE__
    const std::string target = "macos-arm64";
#else
    const std::string target = "linux-x86_64";
#endif

    const std::string checksum =
      "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855";
    std::ofstream lock { directory.path() / "forge.lock.toml" };
    lock
      << "format = 1\n\n"
      << "[[dependency]]\n"
      << "name = \"answer\"\n"
      << "github = \"example/answer\"\n"
      << "version = \"1.2.3\"\n"
      << "target = \"" << target << "\"\n"
      << "url = \"https://example.invalid/answer.cbox\"\n"
      << "sha256 = \"" << checksum << "\"\n";
    lock.close();
    std::filesystem::create_directories(directory.path() / ".forge/cache/downloads");
    std::ofstream { directory.path() / ".forge/cache/downloads" / (checksum + ".cbox") };
    invocations = 0;
    output.str({});
    error.str({});

    expect(
      forge::build_project(directory.path(), runner, output, error) == 2,
      "locked GitHub dependency reaches box validation"
    );
    expect(invocations == 0, "locked GitHub dependency skips checksum and box downloads");
    expect(contains(output.str(), "Using locked dependency answer"), "build reports the locked dependency");

    write_project(directory.path());
    std::ofstream changed_recipe { directory.path() / "forge.recipe.toml", std::ios::app };
    changed_recipe
      << "\n[dependencies]\n"
      << "answer = { github = \"example/answer\", version = \"1.2.4\" }\n";
    changed_recipe.close();
    invocations = 0;
    output.str({});
    error.str({});

    expect(
      forge::build_project(directory.path(), runner, output, error) == 2,
      "build rejects a recipe that conflicts with its lockfile"
    );
    expect(invocations == 0, "lockfile conflict does not access the network");
    expect(contains(error.str(), "conflicts with forge.lock.toml"), "lockfile conflict is explained");
  }

  void test_build_validates_locked_github_component_identity()
  {
    TemporaryDirectory directory;
    write_project(directory.path());
    std::ofstream recipe { directory.path() / "forge.recipe.toml", std::ios::app };
    recipe
      << "\n[dependencies]\n"
      << "answer = { github = \"example/suite\", package = \"suite\", "
         "component = \"answer\", version = \"1.2.3\" }\n";
    recipe.close();

#ifdef _WIN32
    const std::string target = "windows-x86_64";
#elif __APPLE__
    const std::string target = "macos-arm64";
#else
    const std::string target = "linux-x86_64";
#endif

    const std::string checksum =
      "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855";
    std::ofstream lock { directory.path() / "forge.lock.toml" };
    lock
      << "format = 2\n\n"
      << "[[dependency]]\n"
      << "name = \"answer\"\n"
      << "github = \"example/suite\"\n"
      << "package = \"suite\"\n"
      << "component = \"other\"\n"
      << "version = \"1.2.3\"\n"
      << "target = \"" << target << "\"\n"
      << "url = \"https://example.invalid/suite.cbox\"\n"
      << "sha256 = \"" << checksum << "\"\n";
    lock.close();
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
      "build rejects a lock selecting a different GitHub component"
    );
    expect(invocations == 0, "GitHub component lock conflict does not access the network");
    expect(
      contains(error.str(), "conflicts with forge.lock.toml"),
      "GitHub component lock conflict is explained"
    );
  }

  void test_build_rejects_incomplete_github_dependency()
  {
    TemporaryDirectory directory;
    write_project(directory.path());
    std::ofstream recipe { directory.path() / "forge.recipe.toml", std::ios::app };
    recipe
      << "\n[dependencies]\n"
      << "answer = { github = \"example/answer\" }\n";
    recipe.close();
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
      "build rejects an incomplete GitHub dependency"
    );
    expect(invocations == 0, "invalid GitHub dependency does not invoke external tools");
    expect(contains(error.str(), "invalid recipe value"), "incomplete GitHub dependency is explained");
  }

} // namespace

int main()
{
  test_build_generates_cmake_and_commands();
  test_build_generates_static_library();
  test_build_generates_dynamic_library();
  test_build_accepts_legacy_shared_library_alias();
  test_build_validates_header_only_project();
  test_build_stops_after_configuration_failure();
  test_build_generates_recipe_and_cli_definitions();
  test_build_generates_named_target_definitions();
  test_build_rejects_invalid_recipe_definition();
  test_build_applies_build_profile();
  test_build_selects_named_target();
  test_build_requires_target_for_multi_target_recipe();
  test_build_rejects_missing_internal_target();
  test_build_rejects_internal_target_cycle();
  test_build_stages_runtime_assets();
  test_build_rejects_runtime_asset_collision();
  test_build_rejects_missing_source_without_running_process();
  test_build_rejects_dependency_name_mismatch();
  test_build_rejects_dependency_cycle();
  test_update_resolves_github_dependency();
  test_update_resolves_github_component_dependency();
  test_build_requires_and_uses_locked_github_dependency();
  test_build_validates_locked_github_component_identity();
  test_build_rejects_incomplete_github_dependency();

  return failures == 0 ? 0 : 1;
}
