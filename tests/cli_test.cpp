#include "cli.h"
#include "fprocess.h"
#include "init.h"
#include "sha256.h"

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

  std::size_t count_occurrences(const std::string& text, std::string_view fragment)
  {
    std::size_t count = 0;
    std::size_t position = 0;

    while ((position = text.find(fragment, position)) != std::string::npos)
    {
      ++count;
      position += fragment.size();
    }

    return count;
  }

  std::string read_file(const std::filesystem::path& path)
  {
    std::ifstream file { path };
    return {
      std::istreambuf_iterator<char> { file },
      std::istreambuf_iterator<char> {}
    };
  }

  void write_file(const std::filesystem::path& path, std::string_view contents)
  {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream file { path };
    file << contents;
  }

  std::string current_target()
  {
#ifdef _WIN32
    return "windows-x86_64";
#elif __APPLE__
    return "macos-arm64";
#else
    return "linux-x86_64";
#endif
  }

  void test_help()
  {
    std::ostringstream output;
    std::ostringstream error;

    expect(forge::cli::run({}, output, error) == 0, "empty arguments show help");
    expect(contains(output.str(), "forge <command>"), "help includes usage");
    expect(
      contains(output.str(), "adopt           Discover an existing project"),
      "help presents adopt as the primary discovery command"
    );
    expect(contains(output.str(), "init            Alias for adopt"), "help documents the init alias");
    expect(
      contains(output.str(), "forge <command> --help"),
      "help points to command-specific documentation"
    );
    expect(
      !contains(output.str(), "forge prepare-release [target]"),
      "help hides the deprecated prepare-release alias"
    );
    expect(error.str().empty(), "help does not write an error");
  }

  void test_command_help()
  {
    constexpr std::array commands {
      std::string_view { "adopt" },
      std::string_view { "box" },
      std::string_view { "new" },
      std::string_view { "build" },
      std::string_view { "run" },
      std::string_view { "test" },
      std::string_view { "update" },
      std::string_view { "bump" },
      std::string_view { "clean" },
      std::string_view { "release" },
      std::string_view { "release-git" },
      std::string_view { "workflow" },
      std::string_view { "prepare-release" }
    };

    for (const auto command : commands)
    {
      const std::array arguments { command, std::string_view { "--help" } };
      std::ostringstream output;
      std::ostringstream error;

      expect(forge::cli::run(arguments, output, error) == 0, "command help succeeds");
      expect(contains(output.str(), "Usage:") || command == "prepare-release", "command help prints usage");
      expect(error.str().empty(), "command help does not write an error");
    }

    constexpr std::array nested_arguments {
      std::string_view { "workflow" },
      std::string_view { "prepare-release" },
      std::string_view { "--help" }
    };
    std::ostringstream nested_output;
    std::ostringstream nested_error;

    expect(
      forge::cli::run(nested_arguments, nested_output, nested_error) == 0,
      "nested workflow help succeeds"
    );
    expect(
      contains(nested_output.str(), "forge workflow prepare-release [target]"),
      "nested workflow help documents its subcommand"
    );
    expect(nested_error.str().empty(), "nested workflow help does not write an error");

    constexpr std::array feature_help_arguments {
      std::string_view { "workflow" },
      std::string_view { "add-feature" },
      std::string_view { "release-boxes" },
      std::string_view { "--help" }
    };
    std::ostringstream feature_help_output;
    std::ostringstream feature_help_error;
    expect(
      forge::cli::run(feature_help_arguments, feature_help_output, feature_help_error) == 0,
      "workflow feature help succeeds"
    );
    expect(
      contains(feature_help_output.str(), "workflow add-feature release-boxes"),
      "workflow feature help documents release-boxes injection"
    );
    expect(feature_help_error.str().empty(), "workflow feature help does not write an error");

    constexpr std::array adopt_arguments {
      std::string_view { "adopt" },
      std::string_view { "--help" }
    };
    std::ostringstream adopt_output;
    std::ostringstream adopt_error;
    forge::cli::run(adopt_arguments, adopt_output, adopt_error);
    expect(
      contains(adopt_output.str(), "does not rewrite")
      && contains(adopt_output.str(), "existing build infrastructure"),
      "adopt help explains its non-migration boundary"
    );
  }

  void test_cli_builds_workspace()
  {
    TemporaryDirectory directory;
    constexpr std::array arguments {
      std::string_view { "build" },
      std::string_view { "--define=WORKSPACE_FEATURE" }
    };
    write_file(
      directory.path() / "forge.workspace.toml",
      "[workspace]\nname = \"suite\"\nprojects = [\"hello\"]\n"
    );
    write_file(
      directory.path() / "hello/forge.recipe.toml",
      "[project]\nname = \"hello\"\nversion = \"0.1.0\"\n"
      "type = \"executable\"\ncpp_std = 20\n\n[sources]\npaths = [\"main.cpp\"]\n"
    );
    write_file(
      directory.path() / "hello/main.cpp",
      "#ifndef WORKSPACE_FEATURE\n#error missing definition\n#endif\nint main() {}\n"
    );
    std::ostringstream output;
    std::ostringstream error;

    expect(
      forge::cli::run(arguments, directory.path(), output, error) == 0,
      "CLI builds a workspace from its root"
    );
    expect(contains(output.str(), "Built workspace suite"), "CLI reports the built workspace");
    expect(error.str().empty(), "successful CLI workspace build does not write an error");
  }

  void test_cli_rejects_invalid_compile_definition()
  {
    constexpr std::array arguments {
      std::string_view { "build" },
      std::string_view { "--define=NOT-VALID" }
    };
    std::ostringstream output;
    std::ostringstream error;

    expect(
      forge::cli::run(arguments, output, error) == 2,
      "CLI rejects an invalid compile definition"
    );
    expect(
      contains(error.str(), "invalid compile definition"),
      "invalid CLI definition is explained"
    );
  }

  void test_cli_runs_and_tests_workspace()
  {
    TemporaryDirectory directory;
    write_file(
      directory.path() / "forge.workspace.toml",
      "[workspace]\nname = \"suite\"\nprojects = [\"app\", \"tests\"]\n"
    );
    write_file(
      directory.path() / "app/forge.recipe.toml",
      "[project]\nname = \"app\"\nversion = \"0.1.0\"\n"
      "type = \"executable\"\ncpp_std = 20\n\n[sources]\npaths = [\"main.cpp\"]\n"
    );
    write_file(
      directory.path() / "app/main.cpp",
      "#include <string_view>\nint main(int argc, char** argv) { "
      "return argc == 2 && std::string_view { argv[1] } == \"ok\" ? 0 : 1; }\n"
    );
    write_file(
      directory.path() / "tests/forge.recipe.toml",
      "[project]\nname = \"tests\"\nversion = \"0.1.0\"\n\n"
      "[target.unit_tests]\ntype = \"executable\"\ncpp_std = 20\n"
      "sources = [\"unit_tests.cpp\"]\ntest = true\n"
    );
    write_file(directory.path() / "tests/unit_tests.cpp", "int main() { return 0; }\n");
    constexpr std::array run_arguments {
      std::string_view { "run" },
      std::string_view { "app" },
      std::string_view { "--" },
      std::string_view { "ok" }
    };
    constexpr std::array test_arguments { std::string_view { "test" } };
    constexpr std::array selected_test_arguments {
      std::string_view { "test" },
      std::string_view { "tests/unit_tests" }
    };
    std::ostringstream run_output;
    std::ostringstream run_error;
    std::ostringstream test_output;
    std::ostringstream test_error;
    std::ostringstream selected_output;
    std::ostringstream selected_error;

    expect(
      forge::cli::run(run_arguments, directory.path(), run_output, run_error) == 0,
      "CLI runs a selected workspace project"
    );
    expect(contains(run_output.str(), "Running app"), "CLI workspace run reports its project");
    expect(run_error.str().empty(), "successful CLI workspace run does not write an error");
    expect(
      forge::cli::run(test_arguments, directory.path(), test_output, test_error) == 0,
      "CLI tests every workspace project with marked tests"
    );
    expect(
      contains(test_output.str(), "Workspace tests: 1 project passed, 0 failed"),
      "CLI workspace test reports aggregate results"
    );
    expect(test_error.str().empty(), "successful CLI workspace test does not write an error");
    expect(
      forge::cli::run(
        selected_test_arguments,
        directory.path(),
        selected_output,
        selected_error
      ) == 0,
      "CLI tests a selected workspace project target"
    );
    expect(
      contains(selected_output.str(), "Testing unit_tests"),
      "CLI selected workspace test reports its target"
    );
    expect(selected_error.str().empty(), "selected CLI workspace test does not write an error");
  }

  void test_version()
  {
    constexpr std::array arguments { std::string_view { "--version" } };
    std::ostringstream output;
    std::ostringstream error;

    expect(forge::cli::run(arguments, output, error) == 0, "version succeeds");
    expect(contains(output.str(), forge::cli::version), "version reports the current version");
  }

  void test_init_alias_adopts_existing_project()
  {
    TemporaryDirectory directory;
    constexpr std::array arguments { std::string_view { "init" } };
    std::ostringstream output;
    std::ostringstream error;
    write_file(directory.path() / "main.cpp", "int main() {}\n");

    expect(
      forge::cli::run(arguments, directory.path(), output, error) == 0,
      "init alias adopts an existing project"
    );
    expect(
      std::filesystem::exists(directory.path() / "forge.recipe.toml"),
      "init alias creates the adopted recipe"
    );
    expect(
      contains(output.str(), "Adopted project"),
      "init alias reports adoption"
    );
    expect(error.str().empty(), "init alias does not write an error");
  }

  void test_init_discovers_existing_sources()
  {
    TemporaryDirectory directory;
    constexpr std::array arguments { std::string_view { "adopt" } };
    std::ostringstream output;
    std::ostringstream error;

    write_file(directory.path() / "main.cpp", "int main() {}\n");
    write_file(directory.path() / "source/game.cc", "");
    write_file(directory.path() / "source/render.cxx", "");
    write_file(directory.path() / "source/readme.txt", "");

    expect(
      forge::cli::run(arguments, directory.path(), output, error) == 0,
      "adopt succeeds"
    );
    expect(
      std::filesystem::exists(directory.path() / "forge.recipe.toml"),
      "adopt creates a recipe"
    );
    expect(contains(output.str(), "Adopted project"), "adopt reports the adopted project");
    expect(
      contains(read_file(directory.path() / "forge.recipe.toml"), "main.cpp"),
      "recipe contains a root source"
    );
    expect(
      contains(read_file(directory.path() / "forge.recipe.toml"), "source/game.cc"),
      "recipe contains a nested source"
    );
    expect(
      read_file(directory.path() / "forge.recipe.toml").starts_with("#:schema https://"),
      "adopt recipe declares its schema"
    );
    expect(
      std::filesystem::exists(directory.path() / ".github/workflows/release-linux.yml"),
      "adopt creates GitHub release workflows"
    );
    expect(
      std::filesystem::exists(directory.path() / "RELEASE_NOTES.md"),
      "adopt creates release notes"
    );
    expect(
      contains(output.str(), "[1/6] Inspecting project")
        && contains(output.str(), "[2/6] Scanning sources and headers")
        && contains(output.str(), "[3/6] Reading project metadata")
        && contains(output.str(), "[4/6] Resolving dependencies")
        && contains(output.str(), "[5/6] Writing recipe")
        && contains(output.str(), "[6/6] Creating release support"),
      "adopt reports project adoption progress"
    );
    expect(contains(output.str(), "Found 3 C++ source files"), "adopt reports discovered sources");
    expect(error.str().empty(), "adopt does not write an error");
  }

  void test_init_ignores_generated_directories()
  {
    TemporaryDirectory directory;
    constexpr std::array arguments { std::string_view { "adopt" } };
    std::ostringstream output;
    std::ostringstream error;

    write_file(directory.path() / "app.cpp", "");
    write_file(directory.path() / ".git/generated.cpp", "");
    write_file(directory.path() / ".forge/generated.cpp", "");
    write_file(directory.path() / "build/generated.cpp", "");
    write_file(directory.path() / "out/generated.cpp", "");
    write_file(directory.path() / "cmake-build-debug/generated.cpp", "");

    forge::cli::run(arguments, directory.path(), output, error);
    const auto recipe = read_file(directory.path() / "forge.recipe.toml");

    expect(contains(recipe, "app.cpp"), "adopt discovers project sources");
    expect(!contains(recipe, "generated.cpp"), "adopt ignores generated directories");
  }

  void test_init_infers_local_include_directories()
  {
    TemporaryDirectory directory;
    constexpr std::array init_arguments { std::string_view { "adopt" } };
    constexpr std::array build_arguments { std::string_view { "build" } };
    std::ostringstream output;
    std::ostringstream error;
    write_file(
      directory.path() / "main.cpp",
      "#include <suite/detail.h>\n"
      "#include <imgui.h>\n"
      "int main() { return detail() + imgui(); }\n"
    );
    write_file(directory.path() / "suite/detail.h", "inline int detail() { return 20; }\n");
    write_file(directory.path() / "vendor/imgui/imgui.h", "inline int imgui() { return 22; }\n");

    expect(
      forge::cli::run(init_arguments, directory.path(), output, error) == 0,
      "adopt infers local include directories"
    );
    const auto recipe = read_file(directory.path() / "forge.recipe.toml");
    expect(
      contains(recipe, "include_dirs = [\".\", \"vendor/imgui\"]"),
      "adopt writes unambiguous local include roots"
    );
    expect(
      contains(output.str(), "Inferred 2 local include directories"),
      "adopt reports inferred local include roots"
    );
    std::ostringstream build_output;
    std::ostringstream build_error;
    expect(
      forge::cli::run(
        build_arguments,
        directory.path(),
        build_output,
        build_error
      ) == 0,
      "recipe with inferred local include roots builds"
    );
    expect(build_error.str().empty(), "inferred include-root build does not write an error");
  }

  void test_adopt_imports_visual_studio_project()
  {
    TemporaryDirectory directory;
    constexpr std::array adopt_arguments { std::string_view { "adopt" } };
    constexpr std::array build_arguments { std::string_view { "build" } };
    constexpr std::array release_build_arguments {
      std::string_view { "build" },
      std::string_view { "--profile=Release" }
    };
    write_file(
      directory.path() / "hello.vcxproj",
      "<Project>\n"
      "  <PropertyGroup><ProjectName>HelloApp</ProjectName>"
      "<ConfigurationType>Application</ConfigurationType></PropertyGroup>\n"
      "  <ItemGroup Label=\"ProjectConfigurations\">"
      "<ProjectConfiguration Include=\"Debug|x64\" />"
      "<ProjectConfiguration Include=\"Release|x64\" /></ItemGroup>\n"
      "  <Import Project=\"config\\common.props\" />"
      "  <ImportGroup Condition=\"'$(Configuration)|$(Platform)'=='Debug|x64'\">"
      "<Import Project=\"config\\debug.props\" /></ImportGroup>"
      "  <Import Project=\"$(SolutionDir)missing.props\" />"
      "  <ItemDefinitionGroup><ClCompile>"
      "<LanguageStandard>stdcpp17</LanguageStandard>"
      "<AdditionalIncludeDirectories>include;%(AdditionalIncludeDirectories)"
      "</AdditionalIncludeDirectories>"
      "</ClCompile></ItemDefinitionGroup>\n"
      "  <ItemDefinitionGroup Condition=\"'$(Configuration)|$(Platform)'=='Debug|x64'\">"
      "<ClCompile><AdditionalIncludeDirectories>debug;%(AdditionalIncludeDirectories)"
      "</AdditionalIncludeDirectories>"
      "<PreprocessorDefinitions>DEBUG_ONLY;%(PreprocessorDefinitions)"
      "</PreprocessorDefinitions></ClCompile></ItemDefinitionGroup>\n"
      "  <ItemDefinitionGroup Condition=\"'$(Configuration)|$(Platform)'=='Release|x64'\">"
      "<ClCompile><AdditionalIncludeDirectories>release;%(AdditionalIncludeDirectories)"
      "</AdditionalIncludeDirectories>"
      "<PreprocessorDefinitions>RELEASE_ONLY;%(PreprocessorDefinitions)"
      "</PreprocessorDefinitions></ClCompile></ItemDefinitionGroup>\n"
      "  <ItemGroup><ClCompile Include=\"src\\main.cpp\" />"
      "<ClInclude Include=\"include\\hello\\hello.h\" /></ItemGroup>\n"
      "</Project>\n"
    );
    write_file(
      directory.path() / "config/common.props",
      "<Project><ItemDefinitionGroup><ClCompile>"
      "<AdditionalIncludeDirectories>$(ProjectDir)props_include;"
      "%(AdditionalIncludeDirectories)</AdditionalIncludeDirectories>"
      "<PreprocessorDefinitions>HELLO_FEATURE;VALUE=42;%(PreprocessorDefinitions)"
      "</PreprocessorDefinitions></ClCompile></ItemDefinitionGroup></Project>\n"
    );
    write_file(
      directory.path() / "config/debug.props",
      "<Project><ItemDefinitionGroup><ClCompile>"
      "<PreprocessorDefinitions>DEBUG_PROPS;%(PreprocessorDefinitions)"
      "</PreprocessorDefinitions></ClCompile></ItemDefinitionGroup></Project>\n"
    );
    write_file(directory.path() / "props_include/placeholder.h", "#pragma once\n");
    write_file(directory.path() / "debug/placeholder.h", "#pragma once\n");
    write_file(directory.path() / "release/placeholder.h", "#pragma once\n");
    write_file(
      directory.path() / "src/main.cpp",
      "#include <hello/hello.h>\n"
      "#ifndef HELLO_FEATURE\n#error missing imported definition\n#endif\n"
      "int main() { return value() == VALUE ? 0 : 1; }\n"
    );
    write_file(
      directory.path() / "include/hello/hello.h",
      "#pragma once\ninline int value() { return 42; }\n"
    );
    std::ostringstream output;
    std::ostringstream error;

    expect(
      forge::cli::run(adopt_arguments, directory.path(), output, error) == 0,
      "adopt imports a Visual Studio project"
    );
    const auto recipe = read_file(directory.path() / "forge.recipe.toml");
    expect(contains(recipe, "name = \"HelloApp\""), "adopt imports the Visual Studio project name");
    expect(contains(recipe, "cpp_std = 17"), "adopt imports the Visual Studio C++ standard");
    expect(contains(recipe, "paths = [\"src/main.cpp\"]"), "adopt normalizes Visual Studio sources");
    expect(
      contains(recipe, "include_dirs = [\"include\", \"props_include\"]"),
      "adopt imports project and props include directories"
    );
    expect(
      contains(recipe, "defines = [\"HELLO_FEATURE\", \"VALUE=42\"]"),
      "adopt imports preprocessor definitions"
    );
    expect(
      contains(recipe, "[profile.Debug.build]")
        && contains(recipe, "configuration = \"Debug\"")
        && contains(recipe, "include_dirs = [\"debug\"]")
        && contains(recipe, "defines = [\"DEBUG_ONLY\", \"DEBUG_PROPS\"]")
        && contains(recipe, "[profile.Release.build]")
        && contains(recipe, "include_dirs = [\"release\"]")
        && contains(recipe, "defines = [\"RELEASE_ONLY\"]"),
      "adopt maps Visual Studio configurations to build profiles"
    );
    expect(
      contains(output.str(), "Imported Visual Studio project hello.vcxproj"),
      "adopt reports the imported Visual Studio project"
    );
    expect(
      contains(output.str(), "Imported 2 Visual Studio build profiles")
        && contains(output.str(), "$(SolutionDir)missing.props"),
      "adopt reports imported profiles and unresolved MSBuild values"
    );
    std::ostringstream build_output;
    std::ostringstream build_error;
    expect(
      forge::cli::run(build_arguments, directory.path(), build_output, build_error) == 0,
      "imported Visual Studio project builds"
    );
    expect(build_error.str().empty(), "imported Visual Studio project build does not write an error");
    std::ostringstream release_output;
    std::ostringstream release_error;
    expect(
      forge::cli::run(
        release_build_arguments,
        directory.path(),
        release_output,
        release_error
      ) == 0,
      "imported Visual Studio Release profile builds"
    );
    expect(release_error.str().empty(), "imported Release profile does not write an error");
  }

  void test_adopt_imports_cmake_project()
  {
    TemporaryDirectory directory;
    constexpr std::array adopt_arguments { std::string_view { "adopt" } };
    constexpr std::array build_arguments { std::string_view { "build" } };
    write_file(
      directory.path() / "CMakeLists.txt",
      "cmake_minimum_required(VERSION 3.25)\n"
      "project(CMakeApp LANGUAGES CXX)\n"
      "add_executable(CMakeApp src/main.cpp)\n"
      "target_compile_features(CMakeApp PRIVATE cxx_std_23)\n"
      "target_include_directories(CMakeApp PRIVATE ${PROJECT_SOURCE_DIR}/include)\n"
      "target_compile_definitions(CMakeApp PRIVATE CMAKE_FEATURE VALUE=42)\n"
      "# add_executable(Commented bad.cpp)\n"
    );
    write_file(
      directory.path() / "src/main.cpp",
      "#include <answer.h>\n"
      "#ifndef CMAKE_FEATURE\n#error missing definition\n#endif\n"
      "int main() { return answer() == VALUE ? 0 : 1; }\n"
    );
    write_file(directory.path() / "include/answer.h", "#pragma once\ninline int answer() { return 42; }\n");
    std::ostringstream output;
    std::ostringstream error;

    expect(
      forge::cli::run(adopt_arguments, directory.path(), output, error) == 0,
      "adopt imports a CMake project"
    );
    const auto recipe = read_file(directory.path() / "forge.recipe.toml");
    expect(contains(recipe, "name = \"CMakeApp\""), "adopt imports the CMake project name");
    expect(contains(recipe, "cpp_std = 23"), "adopt imports the CMake C++ standard");
    expect(contains(recipe, "include_dirs = [\"include\"]"), "adopt imports CMake include directories");
    expect(
      contains(recipe, "defines = [\"CMAKE_FEATURE\", \"VALUE=42\"]"),
      "adopt imports CMake compile definitions"
    );
    expect(contains(output.str(), "Imported CMake project CMakeLists.txt"), "adopt reports CMake import");
    expect(!contains(recipe, "bad.cpp"), "adopt ignores commented CMake commands");
    std::ostringstream build_output;
    std::ostringstream build_error;
    expect(
      forge::cli::run(build_arguments, directory.path(), build_output, build_error) == 0,
      "imported CMake project builds"
    );
    expect(build_error.str().empty(), "imported CMake build does not write an error");
  }

  void test_adopt_preserves_cmake_interface_library_with_programs()
  {
    TemporaryDirectory directory;
    const auto library = directory.path() / "Headers";
    const auto application = directory.path() / "App";
    constexpr std::array adopt_arguments { std::string_view { "adopt" } };
    constexpr std::array build_arguments { std::string_view { "build" } };
    write_file(
      library / "CMakeLists.txt",
      "project(Headers)\n"
      "add_library(Headers INTERFACE)\n"
      "add_library(Headers::Headers ALIAS Headers)\n"
      "target_compile_features(Headers INTERFACE cxx_std_20)\n"
      "target_include_directories(Headers INTERFACE "
      "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)\n"
    );
    write_file(
      library / "include/Headers/answer.h",
      "#pragma once\ninline int answer() { return 42; }\n"
    );
    write_file(
      library / "Examples/examples.cpp",
      "#include <Headers/answer.h>\nint main() { return answer() == 42 ? 0 : 1; }\n"
    );
    write_file(
      library / "Tests/unit_tests.cpp",
      "#include <Headers/answer.h>\nint main() { return answer() == 42 ? 0 : 1; }\n"
    );
    std::ostringstream output;
    std::ostringstream error;

    expect(
      forge::cli::run(adopt_arguments, library, output, error) == 0,
      "adopt preserves a CMake interface library with auxiliary programs"
    );
    const auto recipe = read_file(library / "forge.recipe.toml");
    expect(
      contains(recipe, "[target.Headers]")
        && contains(recipe, "type = \"header_only\"")
        && contains(recipe, "[target.examples]")
        && contains(recipe, "dependencies = [\"Headers\"]")
        && contains(recipe, "[target.unit_tests]")
        && contains(recipe, "test = true"),
      "adopt generates a library target used by examples and tests"
    );
    write_file(
      application / "main.cpp",
      "#include <Headers/answer.h>\nint main() { return answer() == 42 ? 0 : 1; }\n"
    );
    std::ostringstream app_adopt_output;
    std::ostringstream app_adopt_error;
    expect(
      forge::cli::run(adopt_arguments, application, app_adopt_output, app_adopt_error) == 0,
      "adopt infers a named-target sibling library dependency"
    );
    expect(
      contains(read_file(application / "forge.recipe.toml"), "Headers = { path = \"../Headers\" }"),
      "adopt writes the named-target sibling dependency"
    );
    std::ostringstream build_output;
    std::ostringstream build_error;
    const auto build_result = forge::cli::run(
      build_arguments,
      application,
      build_output,
      build_error
    );
    expect(
      build_result == 0,
      "a path dependency automatically selects the adopted library target: " + build_error.str()
    );
    expect(build_error.str().empty(), "adopted multi-target dependency build does not write an error");
  }

  void test_adopt_accepts_library_type_hint()
  {
    TemporaryDirectory directory;
    constexpr std::array arguments {
      std::string_view { "adopt" },
      std::string_view { "--library-type=header_only" }
    };
    write_file(directory.path() / "include/Headers/answer.h", "#pragma once\n");
    write_file(directory.path() / "Examples/examples.cpp", "int main() {}\n");
    write_file(directory.path() / "Tests/unit_tests.cpp", "int main() {}\n");
    std::ostringstream output;
    std::ostringstream error;

    expect(
      forge::cli::run(arguments, directory.path(), output, error) == 0,
      "adopt accepts an explicit library type hint"
    );
    const auto recipe = read_file(directory.path() / "forge.recipe.toml");
    expect(
      contains(recipe, "type = \"header_only\"")
        && contains(recipe, "dependencies = [\"" + directory.path().filename().string() + "\"]"),
      "library type hint preserves a library target beside inferred programs"
    );
    expect(error.str().empty(), "valid library type hint does not write an error");
  }

  void test_adopt_merges_mirrored_cmake_and_visual_studio_projects()
  {
    TemporaryDirectory directory;
    constexpr std::array arguments { std::string_view { "adopt" } };
    write_file(
      directory.path() / "CMakeLists.txt",
      "project(Mirrored LANGUAGES CXX)\n"
      "add_executable(Mirrored src/main.cpp)\n"
      "target_compile_features(Mirrored PRIVATE cxx_std_20)\n"
      "target_compile_definitions(Mirrored PRIVATE FROM_CMAKE)\n"
    );
    write_file(
      directory.path() / "Mirrored.vcxproj",
      "<Project><PropertyGroup><ProjectName>MirroredNative</ProjectName>"
      "<ConfigurationType>Application</ConfigurationType>"
      "<LanguageStandard>stdcpp17</LanguageStandard></PropertyGroup>"
      "<ItemGroup><ClCompile Include=\"src\\main.cpp\" /></ItemGroup></Project>\n"
    );
    write_file(directory.path() / "src/main.cpp", "int main() {}\n");
    std::ostringstream output;
    std::ostringstream error;

    expect(
      forge::cli::run(arguments, directory.path(), output, error) == 0,
      "adopt merges mirrored CMake and Visual Studio metadata"
    );
    const auto recipe = read_file(directory.path() / "forge.recipe.toml");
    expect(
      contains(recipe, "name = \"MirroredNative\""),
      "explicit Visual Studio metadata remains authoritative"
    );
    expect(
      contains(recipe, "cpp_std = 20"),
      "mirrored adoption keeps the highest required C++ standard"
    );
    expect(contains(recipe, "FROM_CMAKE"), "mirrored CMake metadata fills additional settings");
    expect(
      contains(output.str(), "Imported Visual Studio project Mirrored.vcxproj")
        && contains(output.str(), "Merged mirrored CMake project metadata"),
      "adopt reports mirrored project metadata"
    );
    expect(error.str().empty(), "mirrored project adoption does not write an error");
  }

  void test_adopt_prefers_cmake_over_generated_solution()
  {
    TemporaryDirectory directory;
    constexpr std::array arguments { std::string_view { "adopt" } };
    write_file(directory.path() / "CMakeLists.txt", "project(CMakeRoot)\nadd_executable(CMakeRoot main.cpp)\n");
    write_file(
      directory.path() / "Generated.sln",
      "Microsoft Visual Studio Solution File, Format Version 12.00\n"
      "Project(\"{TYPE}\") = \"Generated\", \"build\\Generated.vcxproj\", \"{ID}\"\n"
    );
    write_file(
      directory.path() / "Generated.vcxproj",
      "<Project><PropertyGroup><ProjectName>Generated</ProjectName></PropertyGroup>"
      "<ItemGroup><CustomBuild Include=\"CMakeFiles\\generate.stamp\" /></ItemGroup></Project>\n"
    );
    write_file(
      directory.path() / "Generated.xcodeproj/project.pbxproj",
      "{ CMakeFiles = generated; }\n"
    );
    write_file(directory.path() / "main.cpp", "int main() {}\n");
    std::ostringstream output;
    std::ostringstream error;

    expect(
      forge::cli::run(arguments, directory.path(), output, error) == 0,
      "adopt ignores a generated solution beside a CMake source project"
    );
    expect(
      std::filesystem::exists(directory.path() / "forge.recipe.toml")
        && !std::filesystem::exists(directory.path() / "forge.workspace.toml"),
      "generated solution does not turn the CMake project into a workspace"
    );
    expect(contains(output.str(), "Imported CMake project CMakeLists.txt"), "CMake remains authoritative");
    expect(error.str().empty(), "generated solution preference does not write an error");
  }

  void test_adopt_imports_xcode_project()
  {
    TemporaryDirectory directory;
    constexpr std::array adopt_arguments { std::string_view { "adopt" } };
    constexpr std::array release_build_arguments {
      std::string_view { "build" },
      std::string_view { "--profile=Release" }
    };
    write_file(
      directory.path() / "Hello.xcodeproj/project.pbxproj",
      "{ objects = {\n"
      "TARGET = { isa = PBXNativeTarget; name = HelloXcode; "
      "productType = \"com.apple.product-type.tool\"; };\n"
      "DEBUG = { isa = XCBuildConfiguration; buildSettings = { "
      "CLANG_CXX_LANGUAGE_STANDARD = \"c++20\"; "
      "HEADER_SEARCH_PATHS = (\"$(PROJECT_DIR)/include\", \"$(inherited)\"); "
      "GCC_PREPROCESSOR_DEFINITIONS = (XCODE_COMMON, DEBUG_XCODE); }; name = Debug; };\n"
      "RELEASE = { isa = XCBuildConfiguration; buildSettings = { "
      "CLANG_CXX_LANGUAGE_STANDARD = \"c++20\"; "
      "HEADER_SEARCH_PATHS = (\"$(PROJECT_DIR)/include\", \"$(inherited)\"); "
      "GCC_PREPROCESSOR_DEFINITIONS = (XCODE_COMMON, RELEASE_XCODE); }; name = Release; };\n"
      "CONFIG = { isa = PBXFileReference; path = config/Release.xcconfig; };\n"
      "}; }\n"
    );
    write_file(directory.path() / "config/Release.xcconfig", "GCC_PREPROCESSOR_DEFINITIONS = RELEASE_CONFIG\n");
    write_file(directory.path() / "include/placeholder.h", "#pragma once\n");
    write_file(
      directory.path() / "main.cpp",
      "#ifndef XCODE_COMMON\n#error missing definition\n#endif\nint main() {}\n"
    );
    std::ostringstream output;
    std::ostringstream error;

    expect(
      forge::cli::run(adopt_arguments, directory.path(), output, error) == 0,
      "adopt imports an Xcode project"
    );
    const auto recipe = read_file(directory.path() / "forge.recipe.toml");
    expect(contains(recipe, "name = \"HelloXcode\""), "adopt imports the Xcode target name");
    expect(contains(recipe, "include_dirs = [\"include\"]"), "adopt imports Xcode header search paths");
    expect(contains(recipe, "defines = [\"XCODE_COMMON\"]"), "adopt imports common Xcode definitions");
    expect(
      contains(recipe, "[profile.Debug.build]")
        && contains(recipe, "defines = [\"DEBUG_XCODE\"]")
        && contains(recipe, "[profile.Release.build]")
        && contains(recipe, "defines = [\"RELEASE_CONFIG\", \"RELEASE_XCODE\"]"),
      "adopt maps Xcode configurations to build profiles"
    );
    expect(contains(output.str(), "Imported Xcode project Hello.xcodeproj"), "adopt reports Xcode import");
    std::ostringstream build_output;
    std::ostringstream build_error;
    expect(
      forge::cli::run(
        release_build_arguments,
        directory.path(),
        build_output,
        build_error
      ) == 0,
      "imported Xcode Release profile builds"
    );
    expect(build_error.str().empty(), "imported Xcode build does not write an error");
  }

  void test_adopt_imports_cmake_superproject_as_workspace()
  {
    TemporaryDirectory directory;
    constexpr std::array adopt_arguments { std::string_view { "adopt" } };
    constexpr std::array build_arguments { std::string_view { "build" } };
    write_file(
      directory.path() / "CMakeLists.txt",
      "project(CMakeSuite)\nadd_subdirectory(Core)\nadd_subdirectory(Tool)\n"
    );
    write_file(
      directory.path() / "Generated.sln",
      "Microsoft Visual Studio Solution File, Format Version 12.00\n"
    );
    write_file(
      directory.path() / "Core/CMakeLists.txt",
      "project(Core)\nadd_library(Core STATIC src/core.cpp)\n"
      "target_include_directories(Core PUBLIC ${PROJECT_SOURCE_DIR}/include)\n"
    );
    write_file(
      directory.path() / "Core/Generated.vcxproj",
      "<Project><ItemGroup><CustomBuild Include=\"CMakeFiles\\generate.stamp\" /></ItemGroup></Project>\n"
    );
    write_file(
      directory.path() / "Core/src/core.cpp",
      "#include <Core/core.h>\nint answer() { return 42; }\n"
    );
    write_file(directory.path() / "Core/include/Core/core.h", "#pragma once\nint answer();\n");
    write_file(
      directory.path() / "Tool/CMakeLists.txt",
      "project(Tool)\nadd_executable(Tool main.cpp)\n"
    );
    write_file(
      directory.path() / "Tool/main.cpp",
      "#include <Core/core.h>\nint main() { return answer() == 42 ? 0 : 1; }\n"
    );
    std::ostringstream output;
    std::ostringstream error;

    expect(
      forge::cli::run(adopt_arguments, directory.path(), output, error) == 0,
      "adopt imports a CMake superproject as a workspace"
    );
    const auto workspace = read_file(directory.path() / "forge.workspace.toml");
    const auto tool_recipe = read_file(directory.path() / "Tool/forge.recipe.toml");
    expect(
      contains(workspace, "name = \"CMakeSuite\"")
        && contains(workspace, "projects = [\"Core\", \"Tool\"]"),
      "CMake superproject adoption creates a workspace"
    );
    expect(
      contains(tool_recipe, "Core = { path = \"../Core\" }"),
      "CMake workspace adoption infers sibling dependencies"
    );
    expect(
      contains(output.str(), "Adopted 2 CMake projects")
        && contains(output.str(), "[1/4] Reading CMake superproject"),
      "CMake workspace adoption reports compact progress"
    );
    std::ostringstream build_output;
    std::ostringstream build_error;
    expect(
      forge::cli::run(build_arguments, directory.path(), build_output, build_error) == 0,
      "imported CMake superproject builds as a workspace"
    );
    expect(build_error.str().empty(), "imported CMake workspace build does not write an error");
  }

  void test_adopt_imports_visual_studio_solution()
  {
    TemporaryDirectory directory;
    constexpr std::array adopt_arguments { std::string_view { "adopt" } };
    constexpr std::array build_arguments { std::string_view { "build" } };
    write_file(
      directory.path() / "Suite.sln",
      "Microsoft Visual Studio Solution File, Format Version 12.00\n"
      "Project(\"{TYPE}\") = \"Core\", \"Core\\Core.vcxproj\", \"{CORE}\"\n"
      "EndProject\n"
      "Project(\"{TYPE}\") = \"App\", \"App\\App.vcxproj\", \"{APP}\"\n"
      "EndProject\n"
    );
    write_file(
      directory.path() / "Core/Core.vcxproj",
      "<Project><PropertyGroup><ProjectName>Core</ProjectName>"
      "<ConfigurationType>StaticLibrary</ConfigurationType></PropertyGroup>"
      "<ItemGroup><ClCompile Include=\"src\\core.cpp\" />"
      "<ClInclude Include=\"include\\Core\\core.h\" /></ItemGroup></Project>\n"
    );
    write_file(
      directory.path() / "Core/src/core.cpp",
      "#include <Core/core.h>\nint answer() { return 42; }\n"
    );
    write_file(directory.path() / "Core/include/Core/core.h", "#pragma once\nint answer();\n");
    write_file(
      directory.path() / "App/App.vcxproj",
      "<Project><PropertyGroup><ProjectName>App</ProjectName>"
      "<ConfigurationType>Application</ConfigurationType></PropertyGroup>"
      "<ItemGroup><ClCompile Include=\"main.cpp\" />"
      "<ProjectReference Include=\"..\\Core\\Core.vcxproj\" /></ItemGroup></Project>\n"
    );
    write_file(
      directory.path() / "App/main.cpp",
      "#include <Core/core.h>\nint main() { return answer() == 42 ? 0 : 1; }\n"
    );
    std::ostringstream output;
    std::ostringstream error;

    expect(
      forge::cli::run(adopt_arguments, directory.path(), output, error) == 0,
      "adopt imports a Visual Studio solution"
    );
    const auto workspace = read_file(directory.path() / "forge.workspace.toml");
    const auto app_recipe = read_file(directory.path() / "App/forge.recipe.toml");
    expect(contains(workspace, "name = \"Suite\""), "solution adoption imports the solution name");
    expect(
      contains(workspace, "projects = [\"App\", \"Core\"]"),
      "solution adoption creates a Forge workspace"
    );
    expect(
      contains(app_recipe, "Core = { path = \"../Core\" }"),
      "solution adoption imports project references"
    );
    expect(
      contains(output.str(), "Adopted 2 Visual Studio projects"),
      "solution adoption reports imported projects"
    );
    expect(
      contains(output.str(), "[1/4] Reading Visual Studio solution")
        && contains(output.str(), "[2/4] Adopting project App")
        && contains(output.str(), "[3/4] Adopting project Core")
        && contains(output.str(), "[4/4] Writing workspace")
        && !contains(output.str(), "[1/6] Inspecting project"),
      "solution adoption reports compact per-project progress"
    );
    std::ostringstream build_output;
    std::ostringstream build_error;
    expect(
      forge::cli::run(build_arguments, directory.path(), build_output, build_error) == 0,
      "imported Visual Studio solution builds as a workspace"
    );
    expect(build_error.str().empty(), "imported solution build does not write an error");
  }

  void test_adopt_reports_unresolved_dependency_includes()
  {
    TemporaryDirectory directory;
    constexpr std::array arguments { std::string_view { "adopt" } };
    std::ostringstream output;
    std::ostringstream error;
    write_file(
      directory.path() / "main.cpp",
      "#include <iostream>\n"
      "#include <sys/wait.h>\n"
      "#include <conio.h>\n"
      "#include <termios.h>\n"
      "#include <imgui.h>\n"
      "#include <Core/core.h>\n"
      "int main() { return 0; }\n"
    );
    write_file(
      directory.path() / "other.cpp",
      "#include <Core/core.h>\n"
    );

    expect(
      forge::cli::run(arguments, directory.path(), output, error) == 0,
      "adopt succeeds with unresolved dependency includes"
    );
    expect(
      contains(output.str(), "Found 2 unresolved dependency includes:"),
      "adopt reports the unresolved dependency count"
    );
    expect(
      contains(output.str(), "<Core/core.h> from main.cpp")
        && contains(output.str(), "<imgui.h> from main.cpp"),
      "adopt reports dependency candidates and their first source"
    );
    expect(
      !contains(output.str(), "<iostream>")
        && !contains(output.str(), "<sys/wait.h>")
        && !contains(output.str(), "<conio.h>")
        && !contains(output.str(), "<termios.h>"),
      "adopt excludes known system headers from dependency candidates"
    );
    expect(error.str().empty(), "unresolved dependency includes do not fail adoption");
  }

  void test_adopt_infers_sibling_project_dependencies()
  {
    TemporaryDirectory directory;
    const auto application = directory.path() / "app";
    const auto answer = directory.path() / "answer";
    constexpr std::array adopt_arguments { std::string_view { "adopt" } };
    constexpr std::array build_arguments { std::string_view { "build" } };
    write_file(
      answer / "forge.recipe.toml",
      "[project]\n"
      "name = \"answer\"\n"
      "version = \"1.0.0\"\n"
      "type = \"header_only\"\n"
      "cpp_std = 20\n\n"
      "[sources]\n"
      "paths = []\n"
      "public_headers = [\"include/answer/answer.h\"]\n"
    );
    write_file(
      answer / "include/answer/answer.h",
      "#pragma once\n"
      "inline int answer() { return 42; }\n"
    );
    write_file(
      application / "main.cpp",
      "#include <answer/answer.h>\n"
      "int main() { return answer() == 42 ? 0 : 1; }\n"
    );
    std::ostringstream output;
    std::ostringstream error;

    expect(
      forge::cli::run(adopt_arguments, application, output, error) == 0,
      "adopt infers a sibling Forge dependency"
    );
    const auto recipe = read_file(application / "forge.recipe.toml");
    expect(
      contains(recipe, "[dependencies]\nanswer = { path = \"../answer\" }"),
      "adopt writes the inferred sibling dependency"
    );
    expect(
      contains(output.str(), "Inferred 1 sibling project dependency:")
        && contains(output.str(), "answer = ../answer"),
      "adopt reports the inferred sibling dependency"
    );
    expect(
      !contains(output.str(), "unresolved dependency include"),
      "matched sibling includes are no longer unresolved"
    );
    std::ostringstream build_output;
    std::ostringstream build_error;
    expect(
      forge::cli::run(build_arguments, application, build_output, build_error) == 0,
      "adopted project builds with its inferred sibling dependency"
    );
    expect(build_error.str().empty(), "inferred sibling dependency build does not write an error");
  }

  void test_adopt_preserves_ambiguous_sibling_includes()
  {
    TemporaryDirectory directory;
    const auto application = directory.path() / "app";
    constexpr std::array arguments { std::string_view { "adopt" } };

    for (const auto sibling : { std::string_view { "first" }, std::string_view { "second" } })
    {
      const auto project = directory.path() / sibling;
      write_file(
        project / "forge.recipe.toml",
        "[project]\n"
        "name = \"" + std::string { sibling } + "\"\n"
        "version = \"1.0.0\"\n"
        "type = \"header_only\"\n"
        "cpp_std = 20\n\n"
        "[sources]\n"
        "paths = []\n"
        "public_headers = [\"include/common/common.h\"]\n"
      );
      write_file(project / "include/common/common.h", "#pragma once\n");
    }

    write_file(application / "main.cpp", "#include <common/common.h>\nint main() {}\n");
    std::ostringstream output;
    std::ostringstream error;

    expect(
      forge::cli::run(arguments, application, output, error) == 0,
      "adopt accepts ambiguous sibling matches"
    );
    expect(
      !contains(read_file(application / "forge.recipe.toml"), "[dependencies]"),
      "adopt does not guess between ambiguous sibling projects"
    );
    expect(
      contains(output.str(), "<common/common.h> from main.cpp"),
      "ambiguous sibling include remains unresolved"
    );
    expect(error.str().empty(), "ambiguous sibling matches do not fail adoption");
  }

  void test_adopt_suggests_github_dependencies_without_network()
  {
    TemporaryDirectory directory;
    constexpr std::array arguments { std::string_view { "adopt" } };
    write_file(
      directory.path() / ".git/config",
      "[remote \"origin\"]\n"
      "  url = https://github.com/example/application.git\n"
    );
    write_file(directory.path() / "main.cpp", "#include <answer/answer.h>\nint main() {}\n");
    std::ostringstream output;
    std::ostringstream error;

    expect(
      forge::cli::run(arguments, directory.path(), output, error) == 0,
      "adopt suggests GitHub dependencies"
    );
    expect(
      contains(output.str(), "example/answer for <answer/answer.h>"),
      "adopt derives a GitHub suggestion from the origin owner and include prefix"
    );
    expect(
      contains(output.str(), "forge adopt --github"),
      "adopt explains how to verify and pin GitHub suggestions"
    );
    expect(
      !contains(read_file(directory.path() / "forge.recipe.toml"), "[dependencies]"),
      "default adoption does not write unverified GitHub suggestions"
    );
    expect(
      !std::filesystem::exists(directory.path() / ".forge/adopt/github"),
      "default adoption does not access GitHub"
    );
    expect(error.str().empty(), "GitHub suggestions do not fail adoption");
  }

  void test_adopt_github_verifies_and_pins_dependency()
  {
    TemporaryDirectory directory;
    const auto application = directory.path() / "application";
    const auto remote = directory.path() / "fixtures" / "remote";
    constexpr std::string_view commit = "0123456789abcdef0123456789abcdef01234567";
    write_file(
      application / ".git/config",
      "[remote \"origin\"]\n"
      "  url = git@github.com:example/application.git\n"
    );
    write_file(application / "main.cpp", "#include <answer/answer.h>\nint main() {}\n");
    write_file(
      remote / "forge.recipe.toml",
      "[project]\n"
      "name = \"answer\"\n"
      "version = \"1.0.0\"\n"
      "type = \"header_only\"\n"
      "cpp_std = 20\n\n"
      "[sources]\n"
      "paths = []\n"
      "public_headers = [\"include/answer/answer.h\"]\n"
    );
    write_file(remote / "include/answer/answer.h", "#pragma once\n");
    write_file(remote / ".git/HEAD", commit);
    bool cloned = false;
    const forge::ProcessRunner runner =
      [&remote, &cloned](const std::vector<std::string>& arguments,
                         const std::filesystem::path&,
                         std::ostream&) -> int
      {
        if (arguments.size() == 7
            && arguments[0] == "git"
            && arguments[1] == "clone"
            && arguments[5] == "https://github.com/example/answer.git")
        {
          cloned = true;
          std::filesystem::copy(
            remote,
            arguments[6],
            std::filesystem::copy_options::recursive
          );
          return 0;
        }

        return 2;
      };
    forge::AdoptOptions options;
    options.github = true;
    std::ostringstream output;
    std::ostringstream error;

    expect(
      forge::adopt_project(application, options, runner, output, error) == 0,
      "adopt --github verifies a GitHub dependency"
    );
    const auto recipe = read_file(application / "forge.recipe.toml");
    expect(cloned, "adopt --github clones the suggested repository");
    expect(
      contains(
        recipe,
        "answer = { git = \"https://github.com/example/answer.git\", commit = \""
          + std::string { commit } + "\" }"
      ),
      "adopt --github writes an exact Git commit pin"
    );
    expect(
      contains(output.str(), "Pinned GitHub dependency example/answer at " + std::string { commit }),
      "adopt --github reports the exact pin"
    );
    expect(error.str().empty(), "verified GitHub adoption does not write an error");
  }

  void test_init_empty_project()
  {
    TemporaryDirectory directory;
    constexpr std::array arguments { std::string_view { "adopt" } };
    std::ostringstream output;
    std::ostringstream error;

    expect(
      forge::cli::run(arguments, directory.path(), output, error) == 0,
      "adopt accepts an empty project"
    );
    expect(
      contains(read_file(directory.path() / "forge.recipe.toml"), "paths = []"),
      "empty project has an empty source list"
    );
    expect(!std::filesystem::exists(directory.path() / "src"), "adopt does not create source files");
  }

  void test_init_infers_library_projects()
  {
    TemporaryDirectory static_directory;
    TemporaryDirectory header_directory;
    constexpr std::array arguments { std::string_view { "adopt" } };
    std::ostringstream static_output;
    std::ostringstream static_error;
    std::ostringstream header_output;
    std::ostringstream header_error;
    write_file(
      static_directory.path() / "src/answer.cpp",
      "// This library has no main() function.\n"
      "const char* description = \"main() is not an entry point here\";\n"
      "int answer() { return 42; }\n"
    );
    write_file(static_directory.path() / "include/answer/answer.h", "int answer();\n");
    write_file(
      header_directory.path() / "include/answer/answer.h",
      "#pragma once\ninline int answer() { return 42; }\n"
    );

    expect(
      forge::cli::run(arguments, static_directory.path(), static_output, static_error) == 0,
      "adopt infers a static-library project"
    );
    const auto static_recipe = read_file(static_directory.path() / "forge.recipe.toml");
    expect(
      contains(static_recipe, "type = \"static_library\""),
      "static-library inference writes the project type"
    );
    expect(
      contains(static_recipe, "public_headers = [\"include/answer/answer.h\"]"),
      "static-library inference discovers public headers"
    );
    expect(
      contains(static_output.str(), "Found 0 main() entry points"),
      "static-library inference reports no entry points"
    );
    expect(static_error.str().empty(), "static-library inference does not write an error");
    constexpr std::array build_arguments { std::string_view { "build" } };
    std::ostringstream static_build_output;
    std::ostringstream static_build_error;
    expect(
      forge::cli::run(
        build_arguments,
        static_directory.path(),
        static_build_output,
        static_build_error
      ) == 0,
      "inferred static-library recipe builds"
    );

    expect(
      forge::cli::run(arguments, header_directory.path(), header_output, header_error) == 0,
      "adopt infers a header-only project"
    );
    const auto header_recipe = read_file(header_directory.path() / "forge.recipe.toml");
    expect(
      contains(header_recipe, "type = \"header_only\""),
      "header-only inference writes the project type"
    );
    expect(contains(header_recipe, "paths = []"), "header-only inference writes empty sources");
    expect(header_error.str().empty(), "header-only inference does not write an error");
    std::ostringstream header_build_output;
    std::ostringstream header_build_error;
    expect(
      forge::cli::run(
        build_arguments,
        header_directory.path(),
        header_build_output,
        header_build_error
      ) == 0,
      "inferred header-only recipe builds"
    );
  }

  void test_init_infers_multiple_executables()
  {
    TemporaryDirectory directory;
    constexpr std::array arguments { std::string_view { "adopt" } };
    std::ostringstream output;
    std::ostringstream error;
    write_file(directory.path() / "src/common.cpp", "int answer() { return 42; }\n");
    write_file(directory.path() / "apps/editor.cpp", "int main() { return 0; }\n");
    write_file(directory.path() / "apps/game.cpp", "int main() { return 0; }\n");

    expect(
      forge::cli::run(arguments, directory.path(), output, error) == 0,
      "adopt infers multiple executable targets"
    );
    const auto recipe = read_file(directory.path() / "forge.recipe.toml");
    expect(contains(recipe, "[target.editor]"), "adopt creates the first executable target");
    expect(contains(recipe, "[target.game]"), "adopt creates the second executable target");
    expect(
      count_occurrences(recipe, "src/common.cpp") == 2,
      "adopt assigns common sources to each executable target"
    );
    expect(
      count_occurrences(recipe, "apps/editor.cpp") == 1
        && count_occurrences(recipe, "apps/game.cpp") == 1,
      "adopt keeps each entry point in its own target"
    );
    expect(
      contains(output.str(), "Found 2 main() entry points"),
      "multiple-executable inference reports its entry points"
    );
    expect(error.str().empty(), "multiple-executable inference does not write an error");
    constexpr std::array build_arguments {
      std::string_view { "build" },
      std::string_view { "editor" }
    };
    std::ostringstream build_output;
    std::ostringstream build_error;
    expect(
      forge::cli::run(build_arguments, directory.path(), build_output, build_error) == 0,
      "inferred named executable recipe builds"
    );
    expect(build_error.str().empty(), "inferred named executable build does not write an error");
  }

  void test_adopt_infers_mapped_runtime_asset()
  {
    TemporaryDirectory directory;
    constexpr std::array arguments { std::string_view { "adopt" } };
    std::ostringstream output;
    std::ostringstream error;
    write_file(
      directory.path() / "Examples/examples.cpp",
      "#include \"Utf8_examples.h\"\n"
      "int main() { return example(); }\n"
    );
    write_file(
      directory.path() / "Examples/Utf8_examples.h",
      "#pragma once\n"
      "#include <fstream>\n"
      "inline int example()\n"
      "{\n"
      "  std::ifstream file { \"Blocks.txt\" };\n"
      "  TextureFile::load(texture, \"colors.tx\");\n"
      "  sprite->load_frame(0, \"background.tx\");\n"
      "  return 0;\n"
      "}\n"
    );
    write_file(directory.path() / "Examples/Blocks.txt", "0000..007F; Basic Latin\n");
    write_file(directory.path() / "Examples/colors.tx", "colors\n");
    write_file(directory.path() / "Examples/background.tx", "background\n");
    write_file(directory.path() / "Examples/bin/Blocks.txt", "generated copy\n");

    expect(
      forge::cli::run(arguments, directory.path(), output, error) == 0,
      "adopt infers a mapped runtime asset"
    );
    const auto recipe = read_file(directory.path() / "forge.recipe.toml");
    expect(
      contains(
        recipe,
        "{ source = \"Examples/Blocks.txt\", destination = \"Blocks.txt\" }"
      ),
      "adopt maps a uniquely resolved runtime asset to the path expected by the executable"
    );
    expect(
      contains(
        recipe,
        "{ source = \"Examples/background.tx\", destination = \"background.tx\" }"
      )
        && contains(recipe, "{ source = \"Examples/colors.tx\", destination = \"colors.tx\" }"),
      "adopt infers runtime assets loaded through domain-specific load functions"
    );
    expect(
      contains(output.str(), "Examples/Blocks.txt -> Blocks.txt"),
      "adopt reports the inferred runtime asset mapping"
    );
    expect(error.str().empty(), "runtime asset inference does not write an error");
  }

  void test_init_groups_sources_by_local_include_graph()
  {
    TemporaryDirectory directory;
    constexpr std::array init_arguments { std::string_view { "adopt" } };
    std::ostringstream output;
    std::ostringstream error;
    write_file(
      directory.path() / "apps/editor.cpp",
      "#include <editor/editor.h>\n"
      "int main() { return editor(); }\n"
    );
    write_file(
      directory.path() / "apps/game.cpp",
      "#include <game/game.h>\n"
      "int main() { return game(); }\n"
    );
    write_file(
      directory.path() / "src/editor.cpp",
      "#include <editor/editor.h>\n"
      "int editor() { return 0; }\n"
    );
    write_file(
      directory.path() / "src/game.cpp",
      "#include <game/game.h>\n"
      "int game() { return 0; }\n"
    );
    write_file(directory.path() / "src/unclassified.cpp", "int unclassified() { return 0; }\n");
    write_file(directory.path() / "include/editor/editor.h", "int editor();\n");
    write_file(directory.path() / "include/game/game.h", "int game();\n");

    expect(
      forge::cli::run(init_arguments, directory.path(), output, error) == 0,
      "adopt groups multi-executable sources"
    );
    const auto recipe = read_file(directory.path() / "forge.recipe.toml");
    expect(
      count_occurrences(recipe, "src/editor.cpp") == 1,
      "adopt assigns editor implementation only to editor"
    );
    expect(
      count_occurrences(recipe, "src/game.cpp") == 1,
      "adopt assigns game implementation only to game"
    );
    expect(
      count_occurrences(recipe, "src/unclassified.cpp") == 2,
      "adopt conservatively shares unclassified sources"
    );

    for (const auto target : { std::string_view { "editor" }, std::string_view { "game" } })
    {
      const std::array build_arguments {
        std::string_view { "build" },
        target
      };
      std::ostringstream build_output;
      std::ostringstream build_error;
      expect(
        forge::cli::run(build_arguments, directory.path(), build_output, build_error) == 0,
        "source-grouped target builds"
      );
      expect(build_error.str().empty(), "source-grouped target build does not write an error");
    }
  }

  void test_init_refuses_to_overwrite()
  {
    TemporaryDirectory directory;
    constexpr std::array arguments { std::string_view { "adopt" } };
    std::ostringstream first_output;
    std::ostringstream first_error;
    std::ostringstream second_output;
    std::ostringstream second_error;

    forge::cli::run(arguments, directory.path(), first_output, first_error);
    const auto original_recipe = read_file(directory.path() / "forge.recipe.toml");

    expect(
      forge::cli::run(arguments, directory.path(), second_output, second_error) == 2,
      "adopt refuses to overwrite an existing recipe"
    );
    expect(
      read_file(directory.path() / "forge.recipe.toml") == original_recipe,
      "adopt preserves an existing recipe"
    );
    expect(contains(second_error.str(), "already exists"), "adopt explains overwrite refusal");
  }

  void test_init_preserves_existing_release_support()
  {
    TemporaryDirectory directory;
    constexpr std::array arguments { std::string_view { "adopt" } };
    std::ostringstream output;
    std::ostringstream error;
    write_file(directory.path() / "main.cpp", "int main() {}\n");
    write_file(directory.path() / "RELEASE_NOTES.md", "# Existing notes\n");
    write_file(
      directory.path() / ".github/workflows/release-linux.yml",
      "name: custom release\n"
    );

    forge::cli::run(arguments, directory.path(), output, error);

    expect(
      read_file(directory.path() / "RELEASE_NOTES.md") == "# Existing notes\n",
      "adopt preserves existing release notes"
    );
    expect(
      read_file(directory.path() / ".github/workflows/release-linux.yml")
        == "name: custom release\n",
      "adopt preserves existing GitHub workflows"
    );
    expect(
      std::filesystem::exists(directory.path() / ".github/workflows/release-windows.yml"),
      "adopt creates missing GitHub workflows"
    );
  }

  void test_workflow_adds_release_boxes_feature()
  {
    TemporaryDirectory directory;
    const auto workflow = directory.path() / ".github/workflows/custom-release.yml";
    write_file(
      workflow,
      "name: custom release\n"
      "on: push\n"
      "jobs:\n"
      "  custom-release:\n"
      "    runs-on: ubuntu-latest\n"
      "    steps:\n"
      "      - run: echo custom\n"
      "concurrency: custom-release\n"
      "env:\n"
      "  CUSTOM: true\n"
    );
    constexpr std::array preview_arguments {
      std::string_view { "workflow" },
      std::string_view { "add-feature" },
      std::string_view { "release-boxes" },
      std::string_view { "--file=.github/workflows/custom-release.yml" }
    };
    constexpr std::array apply_arguments {
      std::string_view { "workflow" },
      std::string_view { "add-feature" },
      std::string_view { "release-boxes" },
      std::string_view { "--file=.github/workflows/custom-release.yml" },
      std::string_view { "--apply" }
    };
    const auto original = read_file(workflow);
    std::ostringstream preview_output;
    std::ostringstream preview_error;

    expect(
      forge::cli::run(preview_arguments, directory.path(), preview_output, preview_error) == 0,
      "workflow feature preview succeeds"
    );
    expect(read_file(workflow) == original, "workflow feature preview does not modify the workflow");
    expect(
      contains(preview_output.str(), "Would add workflow feature release-boxes")
      && contains(preview_output.str(), "forge-release-boxes:")
      && contains(preview_output.str(), "Run again with --apply"),
      "workflow feature preview shows the managed job and apply command"
    );
    expect(preview_error.str().empty(), "workflow feature preview does not write an error");

    std::ostringstream apply_output;
    std::ostringstream apply_error;
    expect(
      forge::cli::run(apply_arguments, directory.path(), apply_output, apply_error) == 0,
      "workflow feature application succeeds"
    );
    const auto updated = read_file(workflow);
    expect(
      contains(updated, "  forge-release-boxes:\n")
      && contains(updated, "# forge-managed: release-boxes@2")
      && contains(updated, "Resolve latest Forge release")
      && contains(updated, "ref: ${{ steps.forge-release.outputs.result }}")
      && contains(updated, "if: startsWith(github.ref, 'refs/tags/')")
      && contains(updated, "forge workflow prepare-release")
      && contains(updated, "artifacts: boxes/*.cbox,boxes/*.sha256"),
      "workflow feature application injects the managed release-boxes job"
    );
    expect(
      contains(updated, "      - run: echo custom\n")
      && contains(updated, "env:\n  CUSTOM: true\n")
      && contains(updated, "concurrency: custom-release\n")
      && updated.find("forge-release-boxes:") < updated.find("concurrency:"),
      "workflow feature application preserves user content and top-level sections"
    );
    expect(apply_error.str().empty(), "workflow feature application does not write an error");

    std::ostringstream repeat_output;
    std::ostringstream repeat_error;
    expect(
      forge::cli::run(apply_arguments, directory.path(), repeat_output, repeat_error) == 0,
      "repeated workflow feature application succeeds"
    );
    expect(read_file(workflow) == updated, "repeated workflow feature application is idempotent");
    expect(
      contains(repeat_output.str(), "already present"),
      "repeated workflow feature application reports existing managed job"
    );
    expect(repeat_error.str().empty(), "repeated workflow feature application writes no error");
  }

  void test_workflow_feature_refuses_unmanaged_collision()
  {
    TemporaryDirectory directory;
    write_file(
      directory.path() / ".github/workflows/custom-release.yml",
      "jobs:\n"
      "  'forge-release-boxes':\n"
      "    runs-on: ubuntu-latest\n"
    );
    constexpr std::array arguments {
      std::string_view { "workflow" },
      std::string_view { "add-feature" },
      std::string_view { "release-boxes" },
      std::string_view { "--file=.github/workflows/custom-release.yml" },
      std::string_view { "--apply" }
    };
    std::ostringstream output;
    std::ostringstream error;

    expect(
      forge::cli::run(arguments, directory.path(), output, error) == 2,
      "workflow feature refuses a user-owned job collision"
    );
    expect(
      contains(error.str(), "exists but is not Forge-managed"),
      "workflow feature explains a user-owned job collision"
    );

    constexpr std::array unsafe_arguments {
      std::string_view { "workflow" },
      std::string_view { "add-feature" },
      std::string_view { "release-boxes" },
      std::string_view { "--file=../outside.yml" },
      std::string_view { "--apply" }
    };
    std::ostringstream unsafe_output;
    std::ostringstream unsafe_error;
    expect(
      forge::cli::run(unsafe_arguments, directory.path(), unsafe_output, unsafe_error) == 2,
      "workflow feature rejects a file outside the project"
    );
    expect(
      contains(unsafe_error.str(), "must stay inside the project"),
      "workflow feature explains an unsafe file path"
    );

    constexpr std::array update_arguments {
      std::string_view { "workflow" },
      std::string_view { "update-feature" },
      std::string_view { "release-boxes" },
      std::string_view { "--file=.github/workflows/custom-release.yml" },
      std::string_view { "--apply" }
    };
    std::ostringstream update_output;
    std::ostringstream update_error;
    expect(
      forge::cli::run(update_arguments, directory.path(), update_output, update_error) == 2,
      "workflow feature update refuses a user-owned job collision"
    );
    expect(
      contains(update_error.str(), "exists but is not Forge-managed"),
      "workflow feature update explains a user-owned job collision"
    );

    constexpr std::array remove_arguments {
      std::string_view { "workflow" },
      std::string_view { "remove-feature" },
      std::string_view { "release-boxes" },
      std::string_view { "--file=.github/workflows/custom-release.yml" },
      std::string_view { "--apply" }
    };
    std::ostringstream remove_output;
    std::ostringstream remove_error;
    expect(
      forge::cli::run(remove_arguments, directory.path(), remove_output, remove_error) == 2,
      "workflow feature removal refuses a user-owned job collision"
    );
    expect(
      contains(remove_error.str(), "exists but is not Forge-managed"),
      "workflow feature removal explains a user-owned job collision"
    );
  }

  void test_workflow_feature_lifecycle()
  {
    TemporaryDirectory directory;
    const auto workflow = directory.path() / ".github/workflows/custom-release.yml";
    write_file(
      workflow,
      "name: custom release\n"
      "jobs:\n"
      "  custom-release:\n"
      "    runs-on: ubuntu-latest\n"
    );
    constexpr std::string_view file_argument = "--file=.github/workflows/custom-release.yml";
    constexpr std::array list_arguments {
      std::string_view { "workflow" },
      std::string_view { "list-features" }
    };
    constexpr std::array status_arguments {
      std::string_view { "workflow" },
      std::string_view { "status" },
      file_argument
    };
    constexpr std::array add_arguments {
      std::string_view { "workflow" },
      std::string_view { "add-feature" },
      std::string_view { "release-boxes" },
      file_argument,
      std::string_view { "--apply" }
    };
    constexpr std::array update_preview_arguments {
      std::string_view { "workflow" },
      std::string_view { "update-feature" },
      std::string_view { "release-boxes" },
      file_argument
    };
    constexpr std::array update_arguments {
      std::string_view { "workflow" },
      std::string_view { "update-feature" },
      std::string_view { "release-boxes" },
      file_argument,
      std::string_view { "--apply" }
    };
    constexpr std::array remove_preview_arguments {
      std::string_view { "workflow" },
      std::string_view { "remove-feature" },
      std::string_view { "release-boxes" },
      file_argument
    };
    constexpr std::array remove_arguments {
      std::string_view { "workflow" },
      std::string_view { "remove-feature" },
      std::string_view { "release-boxes" },
      file_argument,
      std::string_view { "--apply" }
    };

    std::ostringstream list_output;
    std::ostringstream list_error;
    expect(
      forge::cli::run(list_arguments, directory.path(), list_output, list_error) == 0
      && contains(list_output.str(), "release-boxes"),
      "workflow feature listing describes release-boxes"
    );

    std::ostringstream missing_output;
    std::ostringstream missing_error;
    forge::cli::run(status_arguments, directory.path(), missing_output, missing_error);
    expect(contains(missing_output.str(), "release-boxes  missing"), "workflow status reports missing feature");

    std::ostringstream add_output;
    std::ostringstream add_error;
    forge::cli::run(add_arguments, directory.path(), add_output, add_error);
    const auto current = read_file(workflow);
    std::ostringstream current_output;
    std::ostringstream current_error;
    forge::cli::run(status_arguments, directory.path(), current_output, current_error);
    expect(contains(current_output.str(), "release-boxes  current"), "workflow status reports current feature");

    auto outdated = current;
    const auto marker = outdated.find("# forge-managed: release-boxes@2");
    outdated.replace(marker, std::string_view { "# forge-managed: release-boxes@2" }.size(),
                     "# forge-managed: release-boxes@1");
    write_file(workflow, outdated);
    std::ostringstream outdated_output;
    std::ostringstream outdated_error;
    forge::cli::run(status_arguments, directory.path(), outdated_output, outdated_error);
    expect(contains(outdated_output.str(), "release-boxes  outdated"), "workflow status reports outdated feature");

    std::ostringstream update_preview_output;
    std::ostringstream update_preview_error;
    forge::cli::run(update_preview_arguments, directory.path(), update_preview_output, update_preview_error);
    expect(read_file(workflow) == outdated, "workflow feature update preview does not modify the workflow");
    expect(contains(update_preview_output.str(), "Would update workflow feature"), "workflow update preview explains change");

    std::ostringstream update_output;
    std::ostringstream update_error;
    forge::cli::run(update_arguments, directory.path(), update_output, update_error);
    expect(read_file(workflow) == current, "workflow feature update restores the current managed job");

    std::ostringstream remove_preview_output;
    std::ostringstream remove_preview_error;
    forge::cli::run(remove_preview_arguments, directory.path(), remove_preview_output, remove_preview_error);
    expect(read_file(workflow) == current, "workflow feature removal preview does not modify the workflow");
    expect(contains(remove_preview_output.str(), "Would remove workflow feature"), "workflow removal preview explains change");

    std::ostringstream remove_output;
    std::ostringstream remove_error;
    forge::cli::run(remove_arguments, directory.path(), remove_output, remove_error);
    const auto removed = read_file(workflow);
    expect(
      !contains(removed, "forge-release-boxes")
      && contains(removed, "custom-release:"),
      "workflow feature removal removes only the managed job"
    );
  }

  void test_new()
  {
    TemporaryDirectory directory;
    constexpr std::array arguments {
      std::string_view { "new" },
      std::string_view { "hello" }
    };
    std::ostringstream output;
    std::ostringstream error;

    expect(
      forge::cli::run(arguments, directory.path(), output, error) == 0,
      "new succeeds"
    );
    expect(
      std::filesystem::exists(directory.path() / "hello/forge.recipe.toml"),
      "new creates a recipe"
    );
    expect(
      std::filesystem::exists(directory.path() / "hello/main.cpp"),
      "new creates a source file"
    );
    expect(
      contains(read_file(directory.path() / "hello/forge.recipe.toml"), "name = \"hello\""),
      "new recipe contains the project name"
    );
    expect(
      contains(read_file(directory.path() / "hello/forge.recipe.toml"), "paths = [\"main.cpp\"]"),
      "new recipe contains the starter source"
    );
    expect(
      read_file(directory.path() / "hello/forge.recipe.toml").starts_with("#:schema https://"),
      "new recipe declares its schema"
    );
    expect(
      std::filesystem::exists(directory.path() / "hello/.github/workflows/release-macos.yml"),
      "new creates GitHub release workflows"
    );
    expect(
      contains(
        read_file(directory.path() / "hello/.github/workflows/release-macos.yml"),
        "ref: ${{ steps.forge-release.outputs.result }}"
      ),
      "new workflows bootstrap the latest published Forge release"
    );
    expect(
      contains(
        read_file(directory.path() / "hello/.github/workflows/release-windows.yml"),
        "forge.exe workflow prepare-release"
      ),
      "new workflows prepare type-aware release assets"
    );
    expect(
      contains(
        read_file(directory.path() / "hello/.github/workflows/release-windows.yml"),
        "uses: ilammy/msvc-dev-cmd@v1"
      ),
      "new Windows workflow initializes the MSVC environment"
    );
    expect(
      contains(
        read_file(directory.path() / "hello/.github/workflows/release-windows.yml"),
        "-DCMAKE_CXX_COMPILER=cl"
      ),
      "new Windows workflow selects the MSVC compiler"
    );
    expect(
      contains(
        read_file(directory.path() / "hello/.github/workflows/release-linux.yml"),
        "for box in boxes/*.cbox"
      ),
      "new workflows publish box assets"
    );
    expect(
      contains(
        read_file(directory.path() / "hello/.github/workflows/release-linux.yml"),
        "runs-on: ubuntu-22.04"
      ),
      "new Linux workflow builds legacy release assets"
    );
    expect(
      contains(
        read_file(directory.path() / "hello/.github/workflows/release-linux.yml"),
        "needs: [build-modern, build-legacy]"
      ),
      "new Linux workflow publishes modern and legacy release assets together"
    );
    expect(
      std::filesystem::exists(directory.path() / "hello/RELEASE_NOTES.md"),
      "new creates release notes"
    );
    expect(
      contains(read_file(directory.path() / "hello/.gitignore"), "**/.forge/"),
      "new ignores Forge build state"
    );
    expect(error.str().empty(), "new does not write an error");
  }

  void test_new_refuses_existing_path()
  {
    TemporaryDirectory directory;
    constexpr std::array arguments {
      std::string_view { "new" },
      std::string_view { "hello" }
    };
    std::ostringstream output;
    std::ostringstream error;

    std::filesystem::create_directory(directory.path() / "hello");

    expect(
      forge::cli::run(arguments, directory.path(), output, error) == 2,
      "new refuses an existing path"
    );
    expect(contains(error.str(), "already exists"), "new explains existing path refusal");
  }

  void test_new_requires_simple_name()
  {
    TemporaryDirectory directory;
    constexpr std::array arguments {
      std::string_view { "new" },
      std::string_view { "nested/hello" }
    };
    std::ostringstream output;
    std::ostringstream error;

    expect(
      forge::cli::run(arguments, directory.path(), output, error) == 2,
      "new rejects path-like names"
    );
    expect(contains(error.str(), "single directory name"), "new explains invalid names");
  }

  void test_new_requires_name()
  {
    TemporaryDirectory directory;
    constexpr std::array arguments { std::string_view { "new" } };
    std::ostringstream output;
    std::ostringstream error;

    expect(
      forge::cli::run(arguments, directory.path(), output, error) == 2,
      "new requires a project name"
    );
    expect(contains(error.str(), "forge new <name>"), "new prints its usage");
  }

  void test_build_new_project()
  {
    TemporaryDirectory directory;
    constexpr std::array new_arguments {
      std::string_view { "new" },
      std::string_view { "hello" }
    };
    constexpr std::array build_arguments { std::string_view { "build" } };
    std::ostringstream new_output;
    std::ostringstream new_error;
    std::ostringstream build_output;
    std::ostringstream build_error;

    forge::cli::run(new_arguments, directory.path(), new_output, new_error);
    const auto project_directory = directory.path() / "hello";

    expect(
      forge::cli::run(build_arguments, project_directory, build_output, build_error) == 0,
      "build succeeds for a new project"
    );
    expect(
      std::filesystem::exists(project_directory / ".forge/generated/CMakeLists.txt"),
      "build generates CMake infrastructure"
    );
#ifdef _WIN32
    expect(
      std::filesystem::exists(project_directory / ".forge/build/hello.exe"),
      "build creates an executable"
    );
#else
    expect(
      std::filesystem::exists(project_directory / ".forge/build/hello"),
      "build creates an executable"
    );
#endif
    expect(contains(build_output.str(), "Built"), "build reports the resulting executable");
    expect(build_error.str().empty(), "successful build does not write an error");
  }

  void test_build_rejects_empty_project()
  {
    TemporaryDirectory directory;
    constexpr std::array init_arguments { std::string_view { "adopt" } };
    constexpr std::array build_arguments { std::string_view { "build" } };
    std::ostringstream init_output;
    std::ostringstream init_error;
    std::ostringstream build_output;
    std::ostringstream build_error;

    forge::cli::run(init_arguments, directory.path(), init_output, init_error);

    expect(
      forge::cli::run(build_arguments, directory.path(), build_output, build_error) == 2,
      "build rejects an empty project"
    );
    expect(contains(build_error.str(), "no source files"), "build explains an empty project");
  }

  void test_build_and_run_named_target()
  {
    TemporaryDirectory directory;
    write_file(
      directory.path() / "forge.recipe.toml",
      "[project]\n"
      "name = \"suite\"\n"
      "version = \"0.1.0\"\n"
      "\n"
      "[target.suite]\n"
      "type = \"header_only\"\n"
      "cpp_std = 20\n"
      "sources = []\n"
      "public_headers = [\"include/suite/message.h\"]\n"
      "\n"
      "[target.math]\n"
      "type = \"dynamic_library\"\n"
      "cpp_std = 20\n"
      "sources = [\"src/math.cpp\"]\n"
      "public_headers = [\"include/suite/math.h\"]\n"
      "dependencies = [\"suite\"]\n"
      "\n"
      "[target.examples]\n"
      "type = \"executable\"\n"
      "cpp_std = 20\n"
      "sources = [\"Examples/examples.cpp\"]\n"
      "dependencies = [\"math\"]\n"
      "\n"
      "[target.unit_tests]\n"
      "type = \"executable\"\n"
      "cpp_std = 20\n"
      "sources = [\"Tests/unit_tests.cpp\"]\n"
      "test = true\n"
    );
    write_file(
      directory.path() / "include/suite/message.h",
      "#pragma once\n"
      "inline const char* message() { return \"examples\"; }\n"
    );
    write_file(
      directory.path() / "include/suite/math.h",
      "#pragma once\n"
      "int answer();\n"
    );
    write_file(
      directory.path() / "src/math.cpp",
      "#include <suite/math.h>\n"
      "int answer() { return 42; }\n"
    );
    write_file(
      directory.path() / "Examples/examples.cpp",
      "#include <suite/message.h>\n"
      "#include <suite/math.h>\n"
      "#include <iostream>\n"
      "int main() { std::cout << message() << answer() << \"\\\\n\"; }\n"
    );
    write_file(directory.path() / "Tests/unit_tests.cpp", "int main() {}\n");
    constexpr std::array build_arguments {
      std::string_view { "build" },
      std::string_view { "examples" }
    };
    constexpr std::array run_arguments {
      std::string_view { "run" },
      std::string_view { "examples" },
      std::string_view { "--" },
      std::string_view { "--message" }
    };
    constexpr std::array test_arguments {
      std::string_view { "test" }
    };
    constexpr std::array create_library_box_arguments {
      std::string_view { "box" },
      std::string_view { "create" },
      std::string_view { "math" }
    };
    constexpr std::array create_container_box_arguments {
      std::string_view { "box" },
      std::string_view { "create" }
    };
    constexpr std::array create_executable_box_arguments {
      std::string_view { "box" },
      std::string_view { "create" },
      std::string_view { "examples" }
    };
    constexpr std::array release_arguments {
      std::string_view { "release" },
      std::string_view { "examples" }
    };
    constexpr std::array prepare_release_arguments {
      std::string_view { "workflow" },
      std::string_view { "prepare-release" },
      std::string_view { "math" }
    };
    constexpr std::array prepare_aggregate_release_arguments {
      std::string_view { "workflow" },
      std::string_view { "prepare-release" }
    };
    std::ostringstream build_output;
    std::ostringstream build_error;
    std::ostringstream run_output;
    std::ostringstream run_error;
    std::ostringstream test_output;
    std::ostringstream test_error;
    std::ostringstream box_output;
    std::ostringstream box_error;
    std::ostringstream release_output;
    std::ostringstream release_error;
    std::ostringstream prepare_output;
    std::ostringstream prepare_error;
    std::ostringstream prepare_aggregate_output;
    std::ostringstream prepare_aggregate_error;

    expect(
      forge::cli::run(build_arguments, directory.path(), build_output, build_error) == 0,
      "CLI builds a selected named target"
    );
#ifdef _WIN32
    const auto executable = directory.path() / ".forge/build/examples/examples.exe";
#else
    const auto executable = directory.path() / ".forge/build/examples/examples";
#endif
    expect(std::filesystem::exists(executable), "named target build creates an isolated executable");
    expect(
      !std::filesystem::exists(directory.path() / ".forge/build/examples/unit_tests"),
      "named target build does not build another target"
    );
    expect(
      forge::cli::run(run_arguments, directory.path(), run_output, run_error) == 0,
      "CLI runs a selected named target"
    );
    expect(contains(run_output.str(), "Running examples"), "CLI reports the selected named target");
    expect(
      forge::cli::run(test_arguments, directory.path(), test_output, test_error) == 0,
      "CLI builds and runs marked test targets"
    );
    expect(contains(test_output.str(), "1 passed, 0 failed"), "CLI reports the test summary");
    expect(
      forge::cli::run(
        create_library_box_arguments,
        directory.path(),
        box_output,
        box_error
      ) == 0,
      "CLI creates a box for a selected named library target"
    );
    const auto math_box = directory.path() / ".forge/boxes"
      / ("math-0.1.0-" + current_target() + ".cbox");
    const auto suite_box = directory.path() / ".forge/boxes"
      / "suite-0.1.0-ho.cbox";
    expect(std::filesystem::exists(math_box), "named library box is created");
    expect(std::filesystem::exists(suite_box), "internal named dependency box is created");
    const auto math_box_string = math_box.string();
    const std::array inspect_arguments {
      std::string_view { "box" },
      std::string_view { "inspect" },
      std::string_view { math_box_string }
    };
    std::ostringstream inspect_output;
    std::ostringstream inspect_error;
    expect(
      forge::cli::run(inspect_arguments, directory.path(), inspect_output, inspect_error) == 0,
      "CLI inspects a selected named library box"
    );
    expect(
      contains(inspect_output.str(), "name = \"suite\""),
      "named library box embeds its internal dependency"
    );
    std::ostringstream container_output;
    std::ostringstream container_error;
    expect(
      forge::cli::run(
        create_container_box_arguments,
        directory.path(),
        container_output,
        container_error
      ) == 0,
      "CLI creates a multi-component platform box"
    );
    const auto container_box = directory.path() / ".forge/boxes"
      / ("suite-0.1.0-" + current_target() + ".cbox");
    expect(std::filesystem::exists(container_box), "multi-component platform box is created");
    const auto container_box_string = container_box.string();
    const std::array inspect_container_arguments {
      std::string_view { "box" },
      std::string_view { "inspect" },
      std::string_view { container_box_string }
    };
    std::ostringstream inspect_container_output;
    std::ostringstream inspect_container_error;
    expect(
      forge::cli::run(
        inspect_container_arguments,
        directory.path(),
        inspect_container_output,
        inspect_container_error
      ) == 0,
      "CLI inspects a multi-component platform box"
    );
    expect(
      contains(inspect_container_output.str(), "format = 3")
      && contains(inspect_container_output.str(), "Package: suite 0.1.0")
      && contains(inspect_container_output.str(), "Components:")
      && contains(inspect_container_output.str(), "math (dynamic_library)")
      && contains(inspect_container_output.str(), "examples (executable)")
      && contains(inspect_container_output.str(), "name = \"math\"")
      && contains(inspect_container_output.str(), "name = \"examples\""),
      "multi-component inspection summarizes and declares named targets"
    );
    constexpr std::array list_box_arguments {
      std::string_view { "box" },
      std::string_view { "list" }
    };
    std::ostringstream list_box_output;
    std::ostringstream list_box_error;
    expect(
      forge::cli::run(
        list_box_arguments,
        directory.path(),
        list_box_output,
        list_box_error
      ) == 0,
      "CLI lists multi-component platform boxes"
    );
    expect(
      contains(list_box_output.str(), container_box.filename().string())
      && contains(list_box_output.str(), "components:")
      && contains(list_box_output.str(), "math (dynamic_library)")
      && contains(list_box_output.str(), "examples (executable)"),
      "box list summarizes selectable components"
    );
    expect(
      forge::cli::run(
        create_executable_box_arguments,
        directory.path(),
        box_output,
        box_error
      ) == 0,
      "CLI creates a box for a selected named executable target"
    );
    expect(
      forge::cli::run(release_arguments, directory.path(), release_output, release_error) == 0,
      "CLI releases a selected named executable target"
    );
    expect(
      std::filesystem::exists(directory.path() / ".forge/release/examples-0.1.0.zip"),
      "named executable release creates an archive"
    );
#ifdef _WIN32
    const auto staged_internal_runtime =
      directory.path() / ".forge/release/examples-0.1.0/forge_internal_1.dll";
#elif __APPLE__
    const auto staged_internal_runtime =
      directory.path() / ".forge/release/examples-0.1.0/runtime/libforge_internal_1.dylib";
#else
    const auto staged_internal_runtime =
      directory.path() / ".forge/release/examples-0.1.0/runtime/libforge_internal_1.so";
#endif
    expect(
      std::filesystem::exists(staged_internal_runtime),
      "named executable release stages its internal dynamic library"
    );
    expect(
      forge::cli::run(
        prepare_release_arguments,
        directory.path(),
        prepare_output,
        prepare_error
      ) == 0,
      "CLI prepares hosted assets for a selected named library target"
    );
    expect(
      std::filesystem::exists(directory.path() / "boxes" / math_box.filename()),
      "named library hosted assets publish its box"
    );
    expect(
      forge::cli::run(
        prepare_aggregate_release_arguments,
        directory.path(),
        prepare_aggregate_output,
        prepare_aggregate_error
      ) == 0,
      "CLI prepares hosted assets for a multi-component platform box"
    );
    expect(
      std::filesystem::exists(directory.path() / "boxes" / container_box.filename()),
      "multi-component hosted assets publish the aggregate box"
    );
    expect(
      std::filesystem::exists(
        directory.path() / "boxes" / (container_box.filename().string() + ".sha256")
      ),
      "multi-component hosted assets publish the aggregate checksum"
    );
    expect(build_error.str().empty(), "named target CLI build does not write an error");
    expect(run_error.str().empty(), "named target CLI run does not write an error");
    expect(test_error.str().empty(), "successful CLI test does not write an error");
    expect(box_error.str().empty(), "successful named target boxes do not write an error");
    expect(inspect_error.str().empty(), "successful named target box inspection does not write an error");
    expect(release_error.str().empty(), "successful named target release does not write an error");
    expect(prepare_error.str().empty(), "successful named target hosted assets do not write an error");
    expect(
      prepare_aggregate_error.str().empty(),
      "successful multi-component hosted assets do not write an error"
    );
  }

  void test_update_rejects_unknown_dependency()
  {
    TemporaryDirectory directory;
    constexpr std::array new_arguments {
      std::string_view { "new" },
      std::string_view { "hello" }
    };
    constexpr std::array update_arguments {
      std::string_view { "update" },
      std::string_view { "missing" }
    };
    constexpr std::array update_all_arguments { std::string_view { "update" } };
    std::ostringstream new_output;
    std::ostringstream new_error;
    std::ostringstream update_all_output;
    std::ostringstream update_all_error;
    std::ostringstream update_output;
    std::ostringstream update_error;
    forge::cli::run(new_arguments, directory.path(), new_output, new_error);
    const auto project_directory = directory.path() / "hello";

    expect(
      forge::cli::run(
        update_all_arguments,
        project_directory,
        update_all_output,
        update_all_error
      ) == 0,
      "update succeeds without GitHub dependencies"
    );
    expect(
      !std::filesystem::exists(project_directory / ".forge/build/hello"),
      "update does not build the current project"
    );
    expect(contains(update_all_output.str(), "Updated locked dependencies"), "update reports success");

    expect(
      forge::cli::run(
        update_arguments,
        project_directory,
        update_output,
        update_error
      ) == 2,
      "update rejects an unknown dependency"
    );
    expect(contains(update_error.str(), "was not found"), "unknown update dependency is explained");
  }

  void test_run_new_project()
  {
    TemporaryDirectory directory;
    constexpr std::array new_arguments {
      std::string_view { "new" },
      std::string_view { "hello" }
    };
    constexpr std::array run_arguments { std::string_view { "run" } };
    std::ostringstream new_output;
    std::ostringstream new_error;
    std::ostringstream run_output;
    std::ostringstream run_error;

    forge::cli::run(new_arguments, directory.path(), new_output, new_error);

    expect(
      forge::cli::run(run_arguments, directory.path() / "hello", run_output, run_error) == 0,
      "run succeeds for a new project"
    );
    expect(contains(run_output.str(), "Running hello"), "run reports the launched project");
    expect(run_error.str().empty(), "successful run does not write an error");
  }

  void test_release_new_project()
  {
    TemporaryDirectory directory;
    constexpr std::array new_arguments {
      std::string_view { "new" },
      std::string_view { "hello" }
    };
    constexpr std::array release_arguments { std::string_view { "release" } };
    std::ostringstream new_output;
    std::ostringstream new_error;
    std::ostringstream release_output;
    std::ostringstream release_error;

    forge::cli::run(new_arguments, directory.path(), new_output, new_error);
    const auto project_directory = directory.path() / "hello";

    expect(
      forge::cli::run(release_arguments, project_directory, release_output, release_error) == 0,
      "release succeeds for a new project"
    );
    expect(
      std::filesystem::exists(project_directory / ".forge/release/hello-0.1.0.zip"),
      "release creates a zip archive"
    );
    expect(contains(release_output.str(), "Released"), "release reports the archive");
    expect(release_error.str().empty(), "successful release does not write an error");
  }

  void test_prepare_executable_release()
  {
    TemporaryDirectory directory;
    constexpr std::array new_arguments {
      std::string_view { "new" },
      std::string_view { "hello" }
    };
    constexpr std::array release_arguments {
      std::string_view { "workflow" },
      std::string_view { "prepare-release" }
    };
    std::ostringstream new_output;
    std::ostringstream new_error;
    std::ostringstream release_output;
    std::ostringstream release_error;

    forge::cli::run(new_arguments, directory.path(), new_output, new_error);
    const auto project_directory = directory.path() / "hello";

    expect(
      forge::cli::run(release_arguments, project_directory, release_output, release_error) == 0,
      "hosted executable release assets succeed"
    );

    std::size_t archives = 0;

    for (const auto& entry : std::filesystem::directory_iterator { project_directory / ".forge/release" })
    {
      if (entry.path().extension() == ".zip")
      {
        ++archives;
        expect(
          contains(entry.path().filename().string(), "hello-0.1.0-"),
          "hosted executable archive includes its target"
        );
      }
    }

    expect(archives == 1, "hosted executable release creates one archive");
    expect(
      std::filesystem::exists(project_directory / ".forge/release/RELEASE_NOTES.md"),
      "hosted executable release prepares focused notes"
    );
    expect(contains(release_output.str(), "Prepared release assets"), "hosted assets report success");
    expect(release_error.str().empty(), "hosted executable release does not write an error");
  }

  void test_prepare_release_alias_warns()
  {
    TemporaryDirectory directory;
    constexpr std::array arguments { std::string_view { "prepare-release" } };
    std::ostringstream output;
    std::ostringstream error;

    expect(
      forge::cli::run(arguments, directory.path(), output, error) == 2,
      "deprecated prepare-release alias remains accepted"
    );
    expect(
      contains(error.str(), "'prepare-release' is deprecated")
      && contains(error.str(), "forge workflow prepare-release"),
      "deprecated prepare-release alias points to the workflow command"
    );
  }

  void test_release_rejects_empty_tag_format()
  {
    TemporaryDirectory directory;
    constexpr std::array arguments {
      std::string_view { "release" },
      std::string_view { "--tag=" }
    };
    std::ostringstream output;
    std::ostringstream error;

    expect(
      forge::cli::run(arguments, directory.path(), output, error) == 2,
      "local release rejects tag arguments"
    );
    expect(contains(error.str(), "do not accept arguments"), "local release explains tag arguments");
  }

  void test_release_github_rejects_empty_tag_format()
  {
    TemporaryDirectory directory;
    constexpr std::array arguments {
      std::string_view { "release-github" },
      std::string_view { "--tag=" }
    };
    std::ostringstream output;
    std::ostringstream error;

    expect(
      forge::cli::run(arguments, directory.path(), output, error) == 2,
      "GitHub release rejects an empty tag format"
    );
    expect(contains(error.str(), "cannot be empty"), "GitHub release explains an empty tag format");
  }

  void test_release_git_force_rejects_empty_tag_format()
  {
    TemporaryDirectory directory;
    constexpr std::array arguments {
      std::string_view { "release-git" },
      std::string_view { "--tag-force=" }
    };
    std::ostringstream output;
    std::ostringstream error;

    expect(
      forge::cli::run(arguments, directory.path(), output, error) == 2,
      "forced Git release rejects an empty tag format"
    );
    expect(contains(error.str(), "cannot be empty"), "forced Git release explains an empty tag format");
  }

  void test_release_git_rejects_redundant_tag_argument()
  {
    TemporaryDirectory directory;
    constexpr std::array arguments {
      std::string_view { "release-git" },
      std::string_view { "--tag" }
    };
    std::ostringstream output;
    std::ostringstream error;

    expect(
      forge::cli::run(arguments, directory.path(), output, error) == 2,
      "Git release rejects a redundant tag argument"
    );
    expect(contains(error.str(), "usage: forge release-git"), "Git release reports valid tag forms");
  }

  void test_clean_removes_generated_state()
  {
    TemporaryDirectory directory;
    constexpr std::array arguments { std::string_view { "clean" } };
    std::ostringstream output;
    std::ostringstream error;

    write_file(directory.path() / ".forge/build/app", "");
    write_file(directory.path() / ".forge/release/app.zip", "");
    write_file(directory.path() / "forge.recipe.toml", "");
    write_file(directory.path() / "main.cpp", "int main() {}\n");

    expect(
      forge::cli::run(arguments, directory.path(), output, error) == 0,
      "clean succeeds"
    );
    expect(!std::filesystem::exists(directory.path() / ".forge"), "clean removes Forge state");
    expect(std::filesystem::exists(directory.path() / "main.cpp"), "clean preserves project files");
    expect(contains(output.str(), "Cleaned"), "clean reports removed state");
    expect(error.str().empty(), "successful clean does not write an error");
  }

  void test_clean_accepts_already_clean_project()
  {
    TemporaryDirectory directory;
    constexpr std::array arguments { std::string_view { "clean" } };
    std::ostringstream output;
    std::ostringstream error;

    write_file(directory.path() / "forge.recipe.toml", "");

    expect(
      forge::cli::run(arguments, directory.path(), output, error) == 0,
      "clean accepts a project without Forge state"
    );
    expect(contains(output.str(), "Already clean"), "clean reports an already clean project");
    expect(error.str().empty(), "already clean project does not write an error");
  }

  void test_clean_refuses_non_project_directory()
  {
    TemporaryDirectory directory;
    constexpr std::array arguments { std::string_view { "clean" } };
    std::ostringstream output;
    std::ostringstream error;

    write_file(directory.path() / ".forge/important.txt", "");

    expect(
      forge::cli::run(arguments, directory.path(), output, error) == 2,
      "clean rejects a non-project directory"
    );
    expect(
      std::filesystem::exists(directory.path() / ".forge/important.txt"),
      "clean preserves non-project state"
    );
    expect(contains(error.str(), "forge.recipe.toml"), "clean explains the required recipe");
  }

  void test_box_round_trip()
  {
    TemporaryDirectory directory;
    constexpr std::array new_arguments {
      std::string_view { "new" },
      std::string_view { "hello" }
    };
    constexpr std::array create_arguments {
      std::string_view { "box" },
      std::string_view { "create" }
    };
    std::ostringstream new_output;
    std::ostringstream new_error;
    std::ostringstream create_output;
    std::ostringstream create_error;

    forge::cli::run(new_arguments, directory.path(), new_output, new_error);
    const auto project_directory = directory.path() / "hello";
    std::ofstream recipe { project_directory / "forge.recipe.toml", std::ios::app };
    recipe
      << "\n[build]\n"
      << "number = 6\n";
    recipe.close();

    expect(
      forge::cli::run(create_arguments, project_directory, create_output, create_error) == 0,
      "box create succeeds for a new project"
    );

    std::filesystem::path box_path;

    for (const auto& entry : std::filesystem::directory_iterator { project_directory / ".forge/boxes" })
    {
      if (entry.path().extension() == ".cbox")
      {
        box_path = entry.path();
      }
    }

    expect(!box_path.empty(), "box create produces a cbox archive");
    expect(
      contains(box_path.filename().string(), "hello-0.1.0+build.6-"),
      "box filename includes build metadata"
    );
    const auto box_path_string = box_path.string();
    const auto box_filename = box_path.filename().string();

    constexpr std::array list_arguments {
      std::string_view { "box" },
      std::string_view { "list" }
    };
    std::ostringstream generated_list_output;
    std::ostringstream generated_list_error;
    expect(
      forge::cli::run(
        list_arguments,
        project_directory,
        generated_list_output,
        generated_list_error
      ) == 0,
      "box list succeeds"
    );
    expect(
      contains(generated_list_output.str(), "Generated boxes:")
      && contains(generated_list_output.str(), box_filename)
      && contains(generated_list_output.str(), "hello 0.1.0+build.6")
      && contains(generated_list_output.str(), "executable"),
      "box list reports generated box package metadata"
    );

    const std::array inspect_arguments {
      std::string_view { "box" },
      std::string_view { "inspect" },
      std::string_view { box_filename }
    };
    std::ostringstream inspect_output;
    std::ostringstream inspect_error;
    expect(
      forge::cli::run(inspect_arguments, project_directory, inspect_output, inspect_error) == 0,
      "box inspect succeeds"
    );
    expect(contains(inspect_output.str(), "format = 2"), "box inspect prints the manifest");
    expect(
      contains(inspect_output.str(), "Package: hello 0.1.0+build.6")
      && contains(inspect_output.str(), "Type: executable")
      && contains(inspect_output.str(), "Manifest:"),
      "box inspect prints a package summary before the manifest"
    );
    expect(contains(inspect_output.str(), "build = 6"), "box inspect prints the build number");
    expect(contains(inspect_output.str(), "sha256 = \""), "box inspect prints the artifact checksum");

    const std::array verify_arguments {
      std::string_view { "box" },
      std::string_view { "verify" },
      std::string_view { box_path_string }
    };
    std::ostringstream verify_output;
    std::ostringstream verify_error;
    expect(
      forge::cli::run(verify_arguments, project_directory, verify_output, verify_error) == 0,
      "box verify succeeds"
    );
    expect(contains(verify_output.str(), "Verified"), "box verify reports success");

    const std::array publish_arguments {
      std::string_view { "box" },
      std::string_view { "publish" },
      std::string_view { box_filename }
    };
    std::ostringstream publish_output;
    std::ostringstream publish_error;
    expect(
      forge::cli::run(publish_arguments, project_directory, publish_output, publish_error) == 0,
      "box publish succeeds"
    );
    const auto published_box = project_directory / "boxes" / box_path.filename();
    const auto published_checksum =
      project_directory / "boxes" / (box_path.filename().string() + ".sha256");
    expect(std::filesystem::exists(published_box), "box publish copies the box");
    expect(std::filesystem::exists(published_checksum), "box publish writes a checksum file");
    expect(
      contains(read_file(published_checksum), box_path.filename().string()),
      "box checksum names the published box"
    );
    expect(contains(publish_output.str(), "Published locally "), "box publish reports local publication");
    expect(contains(publish_output.str(), "Checksum "), "box publish reports the checksum");

    std::ostringstream published_list_output;
    std::ostringstream published_list_error;
    expect(
      forge::cli::run(
        list_arguments,
        project_directory,
        published_list_output,
        published_list_error
      ) == 0,
      "box list succeeds after publication"
    );
    expect(
      contains(published_list_output.str(), "Published boxes:")
      && contains(published_list_output.str(), box_filename),
      "box list reports published boxes"
    );

    std::ostringstream republish_output;
    std::ostringstream republish_error;
    expect(
      forge::cli::run(publish_arguments, project_directory, republish_output, republish_error) == 0,
      "box publish is idempotent"
    );
    expect(republish_error.str().empty(), "idempotent box publish does not write an error");

    const std::array extract_arguments {
      std::string_view { "box" },
      std::string_view { "extract" },
      std::string_view { box_path_string }
    };
    std::ostringstream extract_output;
    std::ostringstream extract_error;
    expect(
      forge::cli::run(extract_arguments, directory.path(), extract_output, extract_error) == 0,
      "box extract succeeds"
    );
    expect(
      std::filesystem::exists(directory.path() / box_path.stem() / "cbox.toml"),
      "box extract restores the manifest"
    );
  }

  void test_static_library_box_round_trip()
  {
    TemporaryDirectory directory;
    const auto project_directory = directory.path() / "hello-library";
    write_file(
      project_directory / "forge.recipe.toml",
      "[project]\n"
      "name = \"hello\"\n"
      "version = \"1.0.0\"\n"
      "type = \"static_library\"\n"
      "cpp_std = 20\n\n"
      "[sources]\n"
      "paths = [\"src/hello.cpp\"]\n"
      "public_headers = [\"include/hello/hello.h\"]\n"
    );
    write_file(project_directory / "include/hello/hello.h", "int hello();\n");
    write_file(
      project_directory / "src/hello.cpp",
      "#include <hello/hello.h>\n"
      "int hello() { return 42; }\n"
    );
    constexpr std::array create_arguments {
      std::string_view { "box" },
      std::string_view { "create" }
    };
    std::ostringstream create_output;
    std::ostringstream create_error;

    expect(
      forge::cli::run(create_arguments, project_directory, create_output, create_error) == 0,
      "box create succeeds for a static library"
    );

    std::filesystem::path box_path;

    for (const auto& entry : std::filesystem::directory_iterator { project_directory / ".forge/boxes" })
    {
      if (entry.path().extension() == ".cbox")
      {
        box_path = entry.path();
      }
    }

    expect(!box_path.empty(), "static library box create produces a cbox archive");
    const auto box_path_string = box_path.string();
    const std::array inspect_arguments {
      std::string_view { "box" },
      std::string_view { "inspect" },
      std::string_view { box_path_string }
    };
    std::ostringstream inspect_output;
    std::ostringstream inspect_error;

    expect(
      forge::cli::run(inspect_arguments, project_directory, inspect_output, inspect_error) == 0,
      "static library box inspect succeeds"
    );
    expect(contains(inspect_output.str(), "type = \"static_library\""), "manifest identifies a static library");
    expect(contains(inspect_output.str(), "[toolchain]"), "manifest declares the compiled toolchain");
    expect(contains(inspect_output.str(), "compiler = \""), "manifest declares the compiler");
    expect(contains(inspect_output.str(), "runtime = \""), "manifest declares the runtime ABI");
    expect(contains(inspect_output.str(), "path = \"include/hello/hello.h\""), "manifest declares the public header");
#ifdef _WIN32
    expect(contains(inspect_output.str(), "path = \"lib/hello.lib\""), "manifest declares the static library");
#else
    expect(contains(inspect_output.str(), "path = \"lib/libhello.a\""), "manifest declares the static library");
#endif

    const std::array extract_arguments {
      std::string_view { "box" },
      std::string_view { "extract" },
      std::string_view { box_path_string }
    };
    std::ostringstream extract_output;
    std::ostringstream extract_error;

    expect(
      forge::cli::run(extract_arguments, directory.path(), extract_output, extract_error) == 0,
      "static library box extract succeeds"
    );
    expect(
      std::filesystem::exists(directory.path() / box_path.stem() / "include/hello/hello.h"),
      "static library box extracts its public header"
    );
#ifdef _WIN32
    expect(
      std::filesystem::exists(directory.path() / box_path.stem() / "lib/hello.lib"),
      "static library box extracts its library"
    );
#else
    expect(
      std::filesystem::exists(directory.path() / box_path.stem() / "lib/libhello.a"),
      "static library box extracts its library"
    );
#endif

    constexpr std::array release_arguments {
      std::string_view { "workflow" },
      std::string_view { "prepare-release" }
    };
    std::ostringstream release_output;
    std::ostringstream release_error;
    expect(
      forge::cli::run(release_arguments, project_directory, release_output, release_error) == 0,
      "hosted static-library release assets succeed"
    );
    expect(
      std::filesystem::exists(project_directory / "boxes" / box_path.filename()),
      "hosted library release publishes its box"
    );
    expect(
      std::filesystem::exists(
        project_directory / "boxes" / (box_path.filename().string() + ".sha256")
      ),
      "hosted library release publishes its checksum"
    );
    expect(
      std::filesystem::exists(project_directory / ".forge/release/RELEASE_NOTES.md"),
      "hosted library release prepares focused notes"
    );
    expect(release_error.str().empty(), "hosted static-library release does not write an error");
  }

  void test_header_only_box_round_trip()
  {
    TemporaryDirectory directory;
    const auto project_directory = directory.path() / "hello-headers";
    write_file(
      project_directory / "forge.recipe.toml",
      "[project]\n"
      "name = \"hello\"\n"
      "version = \"1.0.0\"\n"
      "type = \"header_only\"\n"
      "cpp_std = 20\n\n"
      "[sources]\n"
      "paths = []\n"
      "public_headers = [\"include/hello/hello.h\"]\n"
    );
    write_file(
      project_directory / "include/hello/hello.h",
      "#pragma once\n"
      "inline int hello() { return 42; }\n"
    );
    constexpr std::array create_arguments {
      std::string_view { "box" },
      std::string_view { "create" }
    };
    std::ostringstream create_output;
    std::ostringstream create_error;

    expect(
      forge::cli::run(create_arguments, project_directory, create_output, create_error) == 0,
      "box create succeeds for a header-only project"
    );

    std::filesystem::path box_path;

    for (const auto& entry : std::filesystem::directory_iterator { project_directory / ".forge/boxes" })
    {
      if (entry.path().extension() == ".cbox")
      {
        box_path = entry.path();
      }
    }

    expect(!box_path.empty(), "header-only box create produces a cbox archive");
    expect(
      box_path.filename() == "hello-1.0.0-ho.cbox",
      "header-only box filename is platform-independent"
    );
    const auto box_path_string = box_path.string();
    const std::array inspect_arguments {
      std::string_view { "box" },
      std::string_view { "inspect" },
      std::string_view { box_path_string }
    };
    std::ostringstream inspect_output;
    std::ostringstream inspect_error;

    expect(
      forge::cli::run(inspect_arguments, project_directory, inspect_output, inspect_error) == 0,
      "header-only box inspect succeeds"
    );
    expect(contains(inspect_output.str(), "Target: any"), "header-only box inspection reports any target");
    expect(contains(inspect_output.str(), "type = \"header_only\""), "manifest identifies header-only package");
    expect(contains(inspect_output.str(), "kind = \"public_header\""), "manifest declares a public header");
    expect(!contains(inspect_output.str(), "static_library"), "header-only box does not declare a library");
    constexpr std::array list_arguments {
      std::string_view { "box" },
      std::string_view { "list" }
    };
    std::ostringstream list_output;
    std::ostringstream list_error;
    expect(
      forge::cli::run(list_arguments, project_directory, list_output, list_error) == 0,
      "header-only box list succeeds"
    );
    expect(
      contains(list_output.str(), "hello 1.0.0 [any]  header_only"),
      "header-only box list reports any target"
    );

    const std::array extract_arguments {
      std::string_view { "box" },
      std::string_view { "extract" },
      std::string_view { box_path_string }
    };
    std::ostringstream extract_output;
    std::ostringstream extract_error;

    expect(
      forge::cli::run(extract_arguments, directory.path(), extract_output, extract_error) == 0,
      "header-only box extract succeeds"
    );
    expect(
      std::filesystem::exists(directory.path() / box_path.stem() / "include/hello/hello.h"),
      "header-only box extracts its public header"
    );
    expect(
      !std::filesystem::exists(directory.path() / box_path.stem() / "lib"),
      "header-only box does not extract a library directory"
    );
  }

  void test_imported_library_box_round_trip()
  {
    TemporaryDirectory directory;
    const auto project = directory.path() / "vendor-sdk";
    write_file(
      project / "forge.recipe.toml",
      "[project]\n"
      "name = \"vendor-sdk\"\n"
      "version = \"4.2.0\"\n"
      "type = \"imported_library\"\n\n"
      "[import." + current_target() + "]\n"
      "compiler = \"ExampleCompiler\"\n"
      "compiler_version = \"1.0\"\n"
      "cpp_std = 20\n"
      "configuration = \"Debug\"\n"
      "runtime = \"default\"\n"
      "public_headers = [\"vendor/include\"]\n"
      "static_libraries = [\"vendor/lib/sdk.a\"]\n"
      "dynamic_libraries = [\"vendor/runtime/sdk.so\"]\n"
    );
    write_file(project / "vendor/include/vendor/sdk.h", "int sdk();\n");
    write_file(project / "vendor/lib/sdk.a", "static library\n");
    write_file(project / "vendor/runtime/sdk.so", "dynamic library\n");
    constexpr std::array create_arguments {
      std::string_view { "box" },
      std::string_view { "create" }
    };
    std::ostringstream create_output;
    std::ostringstream create_error;

    expect(
      forge::cli::run(create_arguments, project, create_output, create_error) == 0,
      "box create succeeds for an imported library"
    );
    expect(
      !std::filesystem::exists(project / ".forge/generated/CMakeLists.txt"),
      "imported-library box creation does not generate a build"
    );

    std::filesystem::path box;

    for (const auto& entry : std::filesystem::directory_iterator { project / ".forge/boxes" })
    {
      if (entry.path().extension() == ".cbox")
      {
        box = entry.path();
      }
    }

    expect(!box.empty(), "imported-library box creation produces a cbox archive");
    const auto box_string = box.string();
    const std::array verify_arguments {
      std::string_view { "box" },
      std::string_view { "verify" },
      std::string_view { box_string }
    };
    std::ostringstream verify_output;
    std::ostringstream verify_error;

    expect(
      forge::cli::run(verify_arguments, project, verify_output, verify_error) == 0,
      "imported-library box verifies"
    );
    expect(contains(verify_output.str(), "Verified"), "imported-library verification reports success");
    expect(create_error.str().empty(), "imported-library box creation does not write an error");
    expect(verify_error.str().empty(), "imported-library verification does not write an error");
  }

  void test_run_with_imported_library_dependency()
  {
    TemporaryDirectory directory;
    const auto first = directory.path() / "first-source";
    const auto second = directory.path() / "second-source";
    const auto third = directory.path() / "third-source";
    const auto imported = directory.path() / "vendor-sdk";
    const auto application = directory.path() / "app";

    const auto write_static_project =
      [](const std::filesystem::path& project,
         std::string_view name,
         std::string_view value)
      {
        write_file(
          project / "forge.recipe.toml",
          "[project]\n"
          "name = \"" + std::string { name } + "\"\n"
          "version = \"1.0.0\"\n"
          "type = \"static_library\"\n"
          "cpp_std = 20\n\n"
          "[sources]\n"
          "paths = [\"src/" + std::string { name } + ".cpp\"]\n"
          "public_headers = [\"include/" + std::string { name } + "/" + std::string { name } + ".h\"]\n"
        );
        write_file(
          project / "include" / name / (std::string { name } + ".h"),
          "int " + std::string { name } + "();\n"
        );
        write_file(
          project / "src" / (std::string { name } + ".cpp"),
          "#include <" + std::string { name } + "/" + std::string { name } + ".h>\n"
          "int " + std::string { name } + "() { return " + std::string { value } + "; }\n"
        );
      };

    write_static_project(first, "first", "20");
    write_static_project(second, "second", "22");
    write_file(
      third / "forge.recipe.toml",
      "[project]\n"
      "name = \"third\"\n"
      "version = \"1.0.0\"\n"
      "type = \"dynamic_library\"\n"
      "cpp_std = 20\n\n"
      "[sources]\n"
      "paths = [\"src/third.cpp\"]\n"
      "public_headers = [\"include/third/third.h\"]\n"
    );
    write_file(third / "include/third/third.h", "int third();\n");
    write_file(
      third / "src/third.cpp",
      "#include <third/third.h>\n"
      "int third() { return 1; }\n"
    );
    constexpr std::array build_arguments { std::string_view { "build" } };
    std::ostringstream first_output;
    std::ostringstream first_error;
    std::ostringstream second_output;
    std::ostringstream second_error;
    std::ostringstream third_output;
    std::ostringstream third_error;
    expect(
      forge::cli::run(build_arguments, first, first_output, first_error) == 0,
      "first imported static library builds"
    );
    expect(
      forge::cli::run(build_arguments, second, second_output, second_error) == 0,
      "second imported static library builds"
    );
    expect(
      forge::cli::run(build_arguments, third, third_output, third_error) == 0,
      "imported dynamic library builds"
    );

#ifdef _WIN32
    const auto first_library = first / ".forge/build/first.lib";
    const auto second_library = second / ".forge/build/second.lib";
    const auto third_runtime = third / ".forge/build/third.dll";
    const auto third_import_library = third / ".forge/build/third.lib";
#elif __APPLE__
    const auto first_library = first / ".forge/build/libfirst.a";
    const auto second_library = second / ".forge/build/libsecond.a";
    const auto third_runtime = third / ".forge/build/libthird.dylib";
#else
    const auto first_library = first / ".forge/build/libfirst.a";
    const auto second_library = second / ".forge/build/libsecond.a";
    const auto third_runtime = third / ".forge/build/libthird.so";
#endif

    std::filesystem::create_directories(imported / "vendor/lib");
    std::filesystem::create_directories(imported / "vendor/runtime");
    std::filesystem::copy_file(first_library, imported / "vendor/lib" / first_library.filename());
    std::filesystem::copy_file(second_library, imported / "vendor/lib" / second_library.filename());
    std::filesystem::copy_file(third_runtime, imported / "vendor/runtime" / third_runtime.filename());
#ifdef _WIN32
    std::filesystem::copy_file(
      third_import_library,
      imported / "vendor/lib" / third_import_library.filename()
    );
#endif
    write_file(imported / "vendor/include/first/first.h", "int first();\n");
    write_file(imported / "vendor/include/second/second.h", "int second();\n");
    write_file(imported / "vendor/include/third/third.h", "int third();\n");
    auto imported_recipe =
      "[project]\n"
      "name = \"vendor-sdk\"\n"
      "version = \"4.2.0\"\n"
      "type = \"imported_library\"\n\n"
      "[import." + current_target() + "]\n"
      + read_file(first / ".forge/build/forge-toolchain.toml")
      + "public_headers = [\"vendor/include\"]\n"
      "static_libraries = [\"vendor/lib/" + first_library.filename().string()
      + "\", \"vendor/lib/" + second_library.filename().string() + "\"]\n"
      "dynamic_libraries = [\"vendor/runtime/" + third_runtime.filename().string() + "\"]\n";
#ifdef _WIN32
    imported_recipe +=
      "import_libraries = [\"vendor/lib/" + third_import_library.filename().string() + "\"]\n";
#endif
    write_file(
      imported / "forge.recipe.toml",
      imported_recipe
    );
    write_file(
      application / "forge.recipe.toml",
      "[project]\n"
      "name = \"app\"\n"
      "version = \"1.0.0\"\n"
      "type = \"executable\"\n"
      "cpp_std = 20\n\n"
      "[sources]\n"
      "paths = [\"main.cpp\"]\n\n"
      "[dependencies]\n"
      "vendor-sdk = { path = \"../vendor-sdk\" }\n"
    );
    write_file(
      application / "main.cpp",
      "#include <first/first.h>\n"
      "#include <second/second.h>\n"
      "#include <third/third.h>\n"
      "int main() { return first() + second() + third() == 43 ? 0 : 1; }\n"
    );
    constexpr std::array run_arguments { std::string_view { "run" } };
    std::ostringstream run_output;
    std::ostringstream run_error;

    expect(
      forge::cli::run(run_arguments, application, run_output, run_error) == 0,
      "run links every library from an imported-library dependency"
    );
    expect(
      std::filesystem::exists(application / ".forge/deps/vendor-sdk/include/first/first.h"),
      "build installs imported-library headers"
    );
    expect(
      std::filesystem::exists(
        application / ".forge/build/runtime" / third_runtime.filename()
      ),
      "build stages every imported dynamic-library runtime"
    );
    constexpr std::array release_arguments { std::string_view { "release" } };
    std::ostringstream release_output;
    std::ostringstream release_error;
    expect(
      forge::cli::run(release_arguments, application, release_output, release_error) == 0,
      "release succeeds with an imported-library dependency"
    );
    expect(
      std::filesystem::exists(
#ifdef _WIN32
        application / ".forge/release/app-1.0.0" / third_runtime.filename()
#else
        application / ".forge/release/app-1.0.0/runtime" / third_runtime.filename()
#endif
      ),
      "release stages every imported dynamic-library runtime"
    );
    expect(first_error.str().empty(), "first imported static library build does not write an error");
    expect(second_error.str().empty(), "second imported static library build does not write an error");
    expect(third_error.str().empty(), "imported dynamic library build does not write an error");
    expect(run_error.str().empty(), "imported-library dependency run does not write an error");
    expect(release_error.str().empty(), "imported-library dependency release does not write an error");

    const auto compiler = imported_recipe.find("compiler = ");
    imported_recipe.replace(
      compiler,
      imported_recipe.find('\n', compiler) - compiler,
      "compiler = \"IncompatibleCompiler\""
    );
    write_file(imported / "forge.recipe.toml", imported_recipe);
    std::ostringstream incompatible_output;
    std::ostringstream incompatible_error;
    expect(
      forge::cli::run(run_arguments, application, incompatible_output, incompatible_error) == 2,
      "build rejects an ABI-incompatible imported-library dependency"
    );
    expect(
      contains(incompatible_error.str(), "toolchain is incompatible"),
      "incompatible imported-library toolchain is explained"
    );
  }

  void test_run_with_local_dependencies()
  {
    TemporaryDirectory directory;
    const auto answer = directory.path() / "answer";
    const auto header_only = directory.path() / "doubled";
    const auto calculator = directory.path() / "calculator";
    const auto application = directory.path() / "app";
    write_file(
      answer / "forge.recipe.toml",
      "[project]\n"
      "name = \"answer\"\n"
      "version = \"1.0.0\"\n"
      "type = \"static_library\"\n"
      "cpp_std = 20\n\n"
      "[sources]\n"
      "paths = [\"src/answer.cpp\"]\n"
      "public_headers = [\"include/answer/answer.h\"]\n"
    );
    write_file(answer / "include/answer/answer.h", "int answer();\n");
    write_file(
      answer / "src/answer.cpp",
      "#include <answer/answer.h>\n"
      "int answer() { return 42; }\n"
    );
    write_file(
      header_only / "forge.recipe.toml",
      "[project]\n"
      "name = \"doubled\"\n"
      "version = \"1.0.0\"\n"
      "type = \"header_only\"\n"
      "cpp_std = 20\n\n"
      "[sources]\n"
      "paths = []\n"
      "public_headers = [\"include/doubled/doubled.h\"]\n"
    );
    write_file(
      header_only / "include/doubled/doubled.h",
      "inline int doubled(int value) { return value * 2; }\n"
    );
    write_file(
      calculator / "forge.recipe.toml",
      "[project]\n"
      "name = \"calculator\"\n"
      "version = \"1.0.0\"\n"
      "type = \"static_library\"\n"
      "cpp_std = 20\n\n"
      "[sources]\n"
      "paths = [\"src/calculator.cpp\"]\n"
      "public_headers = [\"include/calculator/calculator.h\"]\n\n"
      "[dependencies]\n"
      "answer = { path = \"../answer\" }\n"
      "doubled = { path = \"../doubled\" }\n"
    );
    write_file(calculator / "include/calculator/calculator.h", "int calculate();\n");
    write_file(
      calculator / "src/calculator.cpp",
      "#include <answer/answer.h>\n"
      "#include <calculator/calculator.h>\n"
      "#include <doubled/doubled.h>\n"
      "int calculate() { return doubled(answer()); }\n"
    );
    write_file(
      application / "forge.recipe.toml",
      "[project]\n"
      "name = \"app\"\n"
      "version = \"1.0.0\"\n"
      "type = \"executable\"\n"
      "cpp_std = 20\n\n"
      "[sources]\n"
      "paths = [\"main.cpp\"]\n\n"
      "[dependencies]\n"
      "answer = { path = \"../answer\" }\n"
      "calculator = { path = \"../calculator\" }\n"
    );
    write_file(
      application / "main.cpp",
      "#include <calculator/calculator.h>\n"
      "int main() { return calculate() == 84 ? 0 : 1; }\n"
    );
    constexpr std::array run_arguments { std::string_view { "run" } };
    std::ostringstream output;
    std::ostringstream error;

    expect(
      forge::cli::run(run_arguments, application, output, error) == 0,
      "run succeeds with local dependencies"
    );
    expect(
      std::filesystem::exists(application / ".forge/deps/answer/lib"),
      "build installs a shared transitive static-library dependency box"
    );
    expect(
      std::filesystem::exists(application / ".forge/deps/doubled/include/doubled/doubled.h"),
      "build installs a transitive header-only dependency box"
    );
    expect(
      std::filesystem::exists(application / ".forge/deps/calculator/lib"),
      "build installs a direct static-library dependency box"
    );
    const auto generated = read_file(application / ".forge/generated/CMakeLists.txt");
    expect(contains(generated, "forge_dependency_0"), "build imports a static-library dependency");
    expect(contains(generated, ".forge/deps/doubled/include"), "build adds dependency include paths");
    expect(
      generated.find(".forge/deps/calculator/lib") < generated.find(".forge/deps/answer/lib"),
      "build links a static library before its transitive dependency"
    );
    expect(
      count_occurrences(output.str(), "Resolving dependency answer") == 1,
      "build resolves a shared dependency once"
    );
    expect(contains(output.str(), "Running app"), "run launches the linked executable");
    expect(error.str().empty(), "successful dependency build does not write an error");

    std::ostringstream cached_output;
    std::ostringstream cached_error;
    expect(
      forge::cli::run(run_arguments, application, cached_output, cached_error) == 0,
      "second run succeeds with cached source dependency boxes"
    );
    expect(
      contains(cached_output.str(), "Using cached dependency answer"),
      "second build reuses a compatible leaf dependency box"
    );
    expect(
      contains(cached_output.str(), "Using cached dependency calculator"),
      "second build reuses a compatible parent dependency box"
    );
    expect(
      !contains(cached_output.str(), "Resolving dependency"),
      "second build does not rebuild compatible source dependencies"
    );

    write_file(
      answer / "src/answer.cpp",
      "#include <answer/answer.h>\n"
      "int answer() { return 43; }\n"
    );
    std::ostringstream changed_output;
    std::ostringstream changed_error;
    expect(
      forge::cli::run(run_arguments, application, changed_output, changed_error) == 1,
      "changed dependency rebuild reaches the updated application result"
    );
    expect(
      contains(changed_output.str(), "Resolving dependency answer"),
      "changed dependency source rebuilds its box"
    );
    expect(
      contains(changed_output.str(), "Resolving dependency calculator"),
      "changed dependency box invalidates its dependent parent box"
    );
    expect(
      contains(changed_output.str(), "Using cached dependency doubled"),
      "unchanged sibling dependency still reuses its cached box"
    );
    expect(changed_error.str().empty(), "cache invalidation run does not write an error");
  }

  void test_run_with_pinned_git_dependency()
  {
    TemporaryDirectory directory;
    const auto answer = directory.path() / "answer";
    const auto application = directory.path() / "app";
    write_file(
      answer / "forge.recipe.toml",
      "[project]\n"
      "name = \"answer\"\n"
      "version = \"1.0.0\"\n"
      "type = \"static_library\"\n"
      "cpp_std = 20\n\n"
      "[sources]\n"
      "paths = [\"src/answer.cpp\"]\n"
      "public_headers = [\"include/answer/answer.h\"]\n"
    );
    write_file(answer / "include/answer/answer.h", "int answer();\n");
    write_file(
      answer / "src/answer.cpp",
      "#include <answer/answer.h>\n"
      "int answer() { return 42; }\n"
    );
    std::ostringstream git_error;
    expect(
      forge::run_process({ "git", "init", "--quiet", "--initial-branch=main" }, answer, git_error) == 0
        && forge::run_process({ "git", "config", "user.name", "Forge Tests" }, answer, git_error) == 0
        && forge::run_process(
          { "git", "config", "user.email", "forge-tests@example.invalid" },
          answer,
          git_error
        ) == 0
        && forge::run_process({ "git", "add", "." }, answer, git_error) == 0
        && forge::run_process(
          { "git", "commit", "--quiet", "-m", "Add answer library" },
          answer,
          git_error
        ) == 0,
      "pinned Git dependency fixture is committed"
    );
    auto commit = read_file(answer / ".git/refs/heads/main");

    if (!commit.empty() && commit.back() == '\n')
    {
      commit.pop_back();
    }

    write_file(
      application / "forge.recipe.toml",
      "[project]\n"
      "name = \"app\"\n"
      "version = \"1.0.0\"\n"
      "type = \"executable\"\n"
      "cpp_std = 20\n\n"
      "[sources]\n"
      "paths = [\"main.cpp\"]\n\n"
      "[dependencies]\n"
      "answer = { git = \"../answer\", commit = \"" + commit + "\" }\n"
    );
    write_file(
      application / "main.cpp",
      "#include <answer/answer.h>\n"
      "int main() { return answer() == 42 ? 0 : 1; }\n"
    );
    constexpr std::array run_arguments { std::string_view { "run" } };
    std::ostringstream first_output;
    std::ostringstream first_error;
    expect(
      forge::cli::run(run_arguments, application, first_output, first_error) == 0,
      "run succeeds with a pinned Git dependency"
    );
    expect(
      contains(first_output.str(), "Fetching Git dependency answer at " + commit),
      "first build fetches the exact Git commit"
    );
    expect(first_error.str().empty(), "pinned Git dependency run does not write an error");

    std::ostringstream cached_output;
    std::ostringstream cached_error;
    expect(
      forge::cli::run(run_arguments, application, cached_output, cached_error) == 0,
      "second run succeeds with a cached pinned Git dependency"
    );
    expect(
      contains(cached_output.str(), "Using cached Git dependency answer"),
      "second build reuses the exact Git checkout"
    );
    expect(cached_error.str().empty(), "cached Git dependency run does not write an error");

    write_file(
      application / "forge.recipe.toml",
      "[project]\n"
      "name = \"app\"\n"
      "version = \"1.0.0\"\n"
      "type = \"executable\"\n"
      "cpp_std = 20\n\n"
      "[sources]\n"
      "paths = [\"main.cpp\"]\n\n"
      "[dependencies]\n"
      "answer = { git = \"../answer\", commit = \"deadbeef\" }\n"
    );
    constexpr std::array build_arguments { std::string_view { "build" } };
    std::ostringstream abbreviated_output;
    std::ostringstream abbreviated_error;
    expect(
      forge::cli::run(build_arguments, application, abbreviated_output, abbreviated_error) == 2,
      "build rejects an abbreviated Git commit"
    );
    expect(
      contains(abbreviated_error.str(), "invalid recipe value"),
      "abbreviated Git commit rejection is explained"
    );
  }

  void test_dependency_profiles()
  {
    TemporaryDirectory directory;
    const auto development = directory.path() / "answer-dev";
    const auto pinned = directory.path() / "answer-pinned";
    const auto application = directory.path() / "app";
    const auto write_answer = [](const std::filesystem::path& project, int value)
    {
      write_file(
        project / "forge.recipe.toml",
        "[project]\n"
        "name = \"answer\"\n"
        "version = \"1.0.0\"\n"
        "type = \"static_library\"\n"
        "cpp_std = 20\n\n"
        "[sources]\n"
        "paths = [\"src/answer.cpp\"]\n"
        "public_headers = [\"include/answer/answer.h\"]\n"
      );
      write_file(project / "include/answer/answer.h", "int answer();\n");
      write_file(
        project / "src/answer.cpp",
        "#include <answer/answer.h>\n"
        "int answer() { return " + std::to_string(value) + "; }\n"
      );
    };
    write_answer(development, 7);
    write_answer(pinned, 42);
    std::ostringstream git_error;
    expect(
      forge::run_process({ "git", "init", "--quiet", "--initial-branch=main" }, pinned, git_error) == 0
        && forge::run_process({ "git", "config", "user.name", "Forge Tests" }, pinned, git_error) == 0
        && forge::run_process(
          { "git", "config", "user.email", "forge-tests@example.invalid" },
          pinned,
          git_error
        ) == 0
        && forge::run_process({ "git", "add", "." }, pinned, git_error) == 0
        && forge::run_process(
          { "git", "commit", "--quiet", "-m", "Add pinned answer" },
          pinned,
          git_error
        ) == 0,
      "dependency profile Git fixture is committed"
    );
    auto commit = read_file(pinned / ".git/refs/heads/main");

    if (!commit.empty() && commit.back() == '\n')
    {
      commit.pop_back();
    }

    write_file(
      application / "forge.recipe.toml",
      "[project]\n"
      "name = \"app\"\n"
      "version = \"1.0.0\"\n"
      "type = \"executable\"\n"
      "cpp_std = 20\n\n"
      "[sources]\n"
      "paths = [\"main.cpp\"]\n\n"
      "[profile.dev.dependencies]\n"
      "answer = { path = \"../answer-dev\" }\n\n"
      "[profile.pinned.dependencies]\n"
      "answer = { git = \"../answer-pinned\", commit = \"" + commit + "\" }\n"
    );
    constexpr std::array dev_arguments {
      std::string_view { "run" },
      std::string_view { "--profile=dev" }
    };
    constexpr std::array pinned_arguments {
      std::string_view { "run" },
      std::string_view { "--profile=pinned" }
    };
    constexpr std::array missing_arguments {
      std::string_view { "build" },
      std::string_view { "--profile=missing" }
    };
    write_file(
      application / "main.cpp",
      "#include <answer/answer.h>\n"
      "int main() { return answer() == 7 ? 0 : 1; }\n"
    );
    std::ostringstream dev_output;
    std::ostringstream dev_error;
    expect(
      forge::cli::run(dev_arguments, application, dev_output, dev_error) == 0,
      "dev dependency profile uses the local project"
    );
    expect(dev_error.str().empty(), "dev dependency profile does not write an error");

    write_file(
      application / "main.cpp",
      "#include <answer/answer.h>\n"
      "int main() { return answer() == 42 ? 0 : 1; }\n"
    );
    std::ostringstream pinned_output;
    std::ostringstream pinned_error;
    expect(
      forge::cli::run(pinned_arguments, application, pinned_output, pinned_error) == 0,
      "pinned dependency profile uses the exact Git project"
    );
    expect(
      contains(pinned_output.str(), "Fetching Git dependency answer at " + commit),
      "pinned dependency profile fetches its declared commit"
    );
    expect(pinned_error.str().empty(), "pinned dependency profile does not write an error");

    std::ostringstream missing_output;
    std::ostringstream missing_error;
    expect(
      forge::cli::run(missing_arguments, application, missing_output, missing_error) == 2,
      "build rejects a missing dependency profile"
    );
    expect(
      contains(missing_error.str(), "no profile named 'missing'"),
      "missing profile is explained"
    );
  }

  void test_run_with_local_box_dependency()
  {
    TemporaryDirectory directory;
    const auto doubled = directory.path() / "doubled";
    const auto answer = directory.path() / "answer";
    const auto packages = directory.path() / "packages";
    const auto application = directory.path() / "app";
    write_file(
      doubled / "forge.recipe.toml",
      "[project]\n"
      "name = \"doubled\"\n"
      "version = \"1.0.0\"\n"
      "type = \"header_only\"\n"
      "cpp_std = 20\n\n"
      "[sources]\n"
      "paths = []\n"
      "public_headers = [\"include/doubled/doubled.h\"]\n"
    );
    write_file(
      doubled / "include/doubled/doubled.h",
      "inline int doubled(int value) { return value * 2; }\n"
    );
    write_file(
      answer / "forge.recipe.toml",
      "[project]\n"
      "name = \"answer\"\n"
      "version = \"1.0.0\"\n"
      "type = \"static_library\"\n"
      "cpp_std = 20\n\n"
      "[sources]\n"
      "paths = [\"src/answer.cpp\"]\n"
      "public_headers = [\"include/answer/answer.h\"]\n\n"
      "[dependencies]\n"
      "doubled = { path = \"../doubled\" }\n"
    );
    write_file(answer / "include/answer/answer.h", "int answer();\n");
    write_file(
      answer / "src/answer.cpp",
      "#include <answer/answer.h>\n"
      "#include <doubled/doubled.h>\n"
      "int answer() { return doubled(21); }\n"
    );
    constexpr std::array create_arguments {
      std::string_view { "box" },
      std::string_view { "create" }
    };
    std::ostringstream create_output;
    std::ostringstream create_error;

    expect(
      forge::cli::run(create_arguments, answer, create_output, create_error) == 0,
      "box dependency fixture is created"
    );

    std::filesystem::create_directories(packages);
    std::filesystem::path box_path;

    for (const auto& entry : std::filesystem::directory_iterator { answer / ".forge/boxes" })
    {
      if (entry.path().extension() == ".cbox")
      {
        box_path = packages / entry.path().filename();
        std::filesystem::copy_file(entry.path(), box_path);
      }
    }

    std::filesystem::remove_all(answer);
    std::filesystem::remove_all(doubled);
    write_file(
      application / "forge.recipe.toml",
      "[project]\n"
      "name = \"app\"\n"
      "version = \"1.0.0\"\n"
      "type = \"executable\"\n"
      "cpp_std = 20\n\n"
      "[sources]\n"
      "paths = [\"main.cpp\"]\n\n"
      "[dependencies]\n"
      "answer = { box = \"../packages/" + box_path.filename().generic_string() + "\" }\n"
    );
    write_file(
      application / "main.cpp",
      "#include <answer/answer.h>\n"
      "int main() { return answer() == 42 ? 0 : 1; }\n"
    );
    constexpr std::array run_arguments { std::string_view { "run" } };
    std::ostringstream output;
    std::ostringstream error;

    expect(
      forge::cli::run(run_arguments, application, output, error) == 0,
      "run succeeds with a local box dependency"
    );
    expect(
      std::filesystem::exists(application / ".forge/deps/answer/include/answer/answer.h"),
      "build installs headers from the local box"
    );
    expect(
      std::filesystem::exists(application / ".forge/deps/answer/lib"),
      "build installs the library from the local box"
    );
    expect(
      std::filesystem::exists(application / ".forge/deps/doubled/include/doubled/doubled.h"),
      "build installs the transitive dependency embedded in the local box"
    );
    expect(error.str().empty(), "successful local box dependency does not write an error");

    write_file(
      application / "forge.recipe.toml",
      "[project]\n"
      "name = \"app\"\n"
      "version = \"1.0.0\"\n"
      "type = \"executable\"\n"
      "cpp_std = 20\n\n"
      "[sources]\n"
      "paths = [\"main.cpp\"]\n\n"
      "[dependencies]\n"
      "wrong-name = { box = \"../packages/" + box_path.filename().generic_string() + "\" }\n"
    );
    constexpr std::array build_arguments { std::string_view { "build" } };
    std::ostringstream mismatch_output;
    std::ostringstream mismatch_error;

    expect(
      forge::cli::run(build_arguments, application, mismatch_output, mismatch_error) == 2,
      "build rejects a local box with a different package name"
    );
    expect(contains(mismatch_error.str(), "does not match box name"), "box name mismatch is explained");
  }

  void test_run_with_downloadable_box_dependency()
  {
    TemporaryDirectory directory;
    const auto answer = directory.path() / "answer";
    const auto application = directory.path() / "app";
    write_file(
      answer / "forge.recipe.toml",
      "[project]\n"
      "name = \"answer\"\n"
      "version = \"1.0.0\"\n"
      "type = \"static_library\"\n"
      "cpp_std = 20\n\n"
      "[sources]\n"
      "paths = [\"src/answer.cpp\"]\n"
      "public_headers = [\"include/answer/answer.h\"]\n"
    );
    write_file(answer / "include/answer/answer.h", "int answer();\n");
    write_file(
      answer / "src/answer.cpp",
      "#include <answer/answer.h>\n"
      "int answer() { return 42; }\n"
    );
    constexpr std::array create_arguments {
      std::string_view { "box" },
      std::string_view { "create" }
    };
    std::ostringstream create_output;
    std::ostringstream create_error;

    expect(
      forge::cli::run(create_arguments, answer, create_output, create_error) == 0,
      "downloadable box dependency fixture is created"
    );

    std::filesystem::path box_path;

    for (const auto& entry : std::filesystem::directory_iterator { answer / ".forge/boxes" })
    {
      if (entry.path().extension() == ".cbox")
      {
        box_path = entry.path();
      }
    }

    std::string checksum;
    std::ostringstream checksum_error;
    expect(
      forge::sha256_file(box_path, checksum, checksum_error),
      "downloadable box fixture checksum is calculated"
    );
    auto box_url = box_path.generic_string();

#ifdef _WIN32
    box_url = "file:///" + box_url;
#else
    box_url = "file://" + box_url;
#endif

    write_file(
      application / "forge.recipe.toml",
      "[project]\n"
      "name = \"app\"\n"
      "version = \"1.0.0\"\n"
      "type = \"executable\"\n"
      "cpp_std = 20\n\n"
      "[sources]\n"
      "paths = [\"main.cpp\"]\n\n"
      "[dependencies]\n"
      "answer = { url = \"" + box_url + "\", sha256 = \"" + checksum + "\" }\n"
    );
    write_file(
      application / "main.cpp",
      "#include <answer/answer.h>\n"
      "int main() { return answer() == 42 ? 0 : 1; }\n"
    );
    constexpr std::array run_arguments { std::string_view { "run" } };
    std::ostringstream first_output;
    std::ostringstream first_error;

    expect(
      forge::cli::run(run_arguments, application, first_output, first_error) == 0,
      "run succeeds with a downloadable box dependency"
    );
    expect(contains(first_output.str(), "Downloading dependency answer"), "first build downloads the box");

    std::ostringstream second_output;
    std::ostringstream second_error;
    expect(
      forge::cli::run(run_arguments, application, second_output, second_error) == 0,
      "cached downloadable box dependency succeeds"
    );
    expect(!contains(second_output.str(), "Downloading dependency answer"), "second build reuses the cached box");
    expect(second_error.str().empty(), "cached downloadable box dependency does not write an error");

    const std::string wrong_checksum(64, '0');
    write_file(
      application / "forge.recipe.toml",
      "[project]\n"
      "name = \"app\"\n"
      "version = \"1.0.0\"\n"
      "type = \"executable\"\n"
      "cpp_std = 20\n\n"
      "[sources]\n"
      "paths = [\"main.cpp\"]\n\n"
      "[dependencies]\n"
      "answer = { url = \"" + box_url + "\", sha256 = \"" + wrong_checksum + "\" }\n"
    );
    constexpr std::array build_arguments { std::string_view { "build" } };
    std::ostringstream mismatch_output;
    std::ostringstream mismatch_error;

    expect(
      forge::cli::run(build_arguments, application, mismatch_output, mismatch_error) == 2,
      "downloadable box dependency rejects a checksum mismatch"
    );
    expect(contains(mismatch_error.str(), "checksum does not match"), "checksum mismatch is explained");
    expect(
      !std::filesystem::exists(application / ".forge/cache/downloads" / (wrong_checksum + ".cbox")),
      "checksum mismatch is removed from the download cache"
    );
  }

  void test_run_and_release_with_dynamic_dependency()
  {
    TemporaryDirectory directory;
    const auto answer = directory.path() / "answer";
    const auto dynamic_library = directory.path() / "greeting";
    const auto application = directory.path() / "app";
    write_file(
      answer / "forge.recipe.toml",
      "[project]\n"
      "name = \"answer\"\n"
      "version = \"1.0.0\"\n"
      "type = \"dynamic_library\"\n"
      "cpp_std = 20\n\n"
      "[sources]\n"
      "paths = [\"src/answer.cpp\"]\n"
      "public_headers = [\"include/answer/answer.h\"]\n"
    );
    write_file(answer / "include/answer/answer.h", "int answer();\n");
    write_file(
      answer / "src/answer.cpp",
      "#include <answer/answer.h>\n"
      "int answer() { return 42; }\n"
    );
    write_file(
      dynamic_library / "forge.recipe.toml",
      "[project]\n"
      "name = \"greeting\"\n"
      "version = \"1.0.0\"\n"
      "type = \"dynamic_library\"\n"
      "cpp_std = 20\n\n"
      "[sources]\n"
      "paths = [\"src/greeting.cpp\"]\n"
      "public_headers = [\"include/greeting/greeting.h\"]\n\n"
      "[dependencies]\n"
      "answer = { path = \"../answer\" }\n"
    );
    write_file(dynamic_library / "include/greeting/greeting.h", "int greeting();\n");
    write_file(
      dynamic_library / "src/greeting.cpp",
      "#include <answer/answer.h>\n"
      "#include <greeting/greeting.h>\n"
      "int greeting() { return answer(); }\n"
    );
    write_file(
      application / "forge.recipe.toml",
      "[project]\n"
      "name = \"app\"\n"
      "version = \"1.0.0\"\n"
      "type = \"executable\"\n"
      "cpp_std = 20\n\n"
      "[sources]\n"
      "paths = [\"main.cpp\"]\n\n"
      "[dependencies]\n"
      "greeting = { path = \"../greeting\" }\n"
    );
    write_file(
      application / "main.cpp",
      "#include <greeting/greeting.h>\n"
      "int main() { return greeting() == 42 ? 0 : 1; }\n"
    );
    constexpr std::array run_arguments { std::string_view { "run" } };
    std::ostringstream run_output;
    std::ostringstream run_error;

    expect(
      forge::cli::run(run_arguments, application, run_output, run_error) == 0,
      "run succeeds with a dynamic-library dependency"
    );
#ifdef __APPLE__
    constexpr std::string_view runtime_filename = "libgreeting.dylib";
    constexpr std::string_view transitive_runtime_filename = "libanswer.dylib";
#elif defined(__linux__)
    constexpr std::string_view runtime_filename = "libgreeting.so";
    constexpr std::string_view transitive_runtime_filename = "libanswer.so";
#else
    constexpr std::string_view runtime_filename = "greeting.dll";
    constexpr std::string_view transitive_runtime_filename = "answer.dll";
#endif
    expect(
      std::filesystem::exists(application / ".forge/build/runtime" / runtime_filename),
      "build stages the dynamic-library runtime"
    );
    expect(
      std::filesystem::exists(
        application / ".forge/build/runtime" / transitive_runtime_filename
      ),
      "build stages the transitive dynamic-library runtime"
    );
    expect(
      std::filesystem::exists(application / ".forge/deps/greeting/runtime" / runtime_filename),
      "build installs the dynamic-library box"
    );
    const auto dynamic_manifest = read_file(application / ".forge/deps/greeting/cbox.toml");
    expect(
      contains(dynamic_manifest, "type = \"dynamic_library\""),
      "new boxes identify dynamic-library packages with the public terminology"
    );
    expect(
      contains(dynamic_manifest, "kind = \"dynamic_library\""),
      "new boxes identify dynamic-library artifacts with the public terminology"
    );
#ifdef _WIN32
    expect(
      contains(dynamic_manifest, "kind = \"import_library\""),
      "Windows dynamic-library boxes identify their import library"
    );
    expect(
      std::filesystem::exists(application / ".forge/deps/greeting/lib/greeting.lib"),
      "build installs the dynamic-library import library"
    );
    expect(
      std::filesystem::exists(application / ".forge/build" / runtime_filename),
      "build copies the dynamic-library runtime beside the executable"
    );
#endif
    constexpr std::array release_arguments { std::string_view { "release" } };
    std::ostringstream release_output;
    std::ostringstream release_error;

    expect(
      forge::cli::run(release_arguments, application, release_output, release_error) == 0,
      "release succeeds with a dynamic-library dependency"
    );
    expect(
      std::filesystem::exists(
#ifdef _WIN32
        application / ".forge/release/app-1.0.0" / runtime_filename
#else
        application / ".forge/release/app-1.0.0/runtime" / runtime_filename
#endif
      ),
      "release stages the dynamic-library runtime"
    );
    expect(
      std::filesystem::exists(
#ifdef _WIN32
        application / ".forge/release/app-1.0.0" / transitive_runtime_filename
#else
        application / ".forge/release/app-1.0.0/runtime" / transitive_runtime_filename
#endif
      ),
      "release stages the transitive dynamic-library runtime"
    );
    auto released_executable = application / ".forge/release/app-1.0.0/app";
#ifdef _WIN32
    released_executable += ".exe";
#endif
    const std::vector<std::string> released_arguments { released_executable.string() };
    std::ostringstream released_error;
    expect(
      forge::run_process(
        released_arguments,
        released_executable.parent_path(),
        released_error
      ) == 0,
      "released executable loads its dynamic-library runtime"
    );
    expect(run_error.str().empty(), "dynamic-library run does not write an error");
    expect(release_error.str().empty(), "dynamic-library release does not write an error");
    expect(released_error.str().empty(), "released executable does not write an error");
  }

  void test_bump_updates_recipe_and_release_notes()
  {
    TemporaryDirectory directory;
    write_file(
      directory.path() / "forge.recipe.toml",
      "[project]\n"
      "name = \"hello\"\n"
      "version = \"1.3.5\"\n"
      "type = \"executable\"\n"
      "cpp_std = 20\n\n"
      "[build]\n"
      "number = 21\n\n"
      "[sources]\n"
      "paths = [\"main.cpp\"]\n"
    );
    write_file(
      directory.path() / "RELEASE_NOTES.md",
      "# Release notes\n\n"
      "## 1.3.5\n\n"
      "- Previous release.\n"
    );
    constexpr std::array arguments {
      std::string_view { "bump" },
      std::string_view { "major" }
    };
    std::ostringstream output;
    std::ostringstream error;

    expect(
      forge::cli::run(arguments, directory.path(), output, error) == 0,
      "major bump succeeds"
    );
    const auto recipe = read_file(directory.path() / "forge.recipe.toml");
    const auto notes = read_file(directory.path() / "RELEASE_NOTES.md");
    expect(contains(recipe, "version = \"2.0.0\""), "major bump resets minor and patch versions");
    expect(contains(recipe, "number = 22"), "bump increments an existing build number");
    expect(contains(notes, "## 2.0.0\n\n- Describe changes."), "bump prepares release notes");
    expect(notes.find("## 2.0.0") < notes.find("## 1.3.5"), "new release notes appear first");
    expect(contains(output.str(), "Bumped 1.3.5 to 2.0.0"), "bump reports the version change");
    expect(error.str().empty(), "successful bump does not write an error");
  }

  void test_bump_creates_release_notes()
  {
    TemporaryDirectory directory;
    write_file(
      directory.path() / "forge.recipe.toml",
      "[project]\n"
      "name = \"hello\"\n"
      "version = \"1.3.5\"\n"
      "type = \"executable\"\n"
      "cpp_std = 20\n\n"
      "[sources]\n"
      "paths = [\"main.cpp\"]\n"
    );
    constexpr std::array arguments {
      std::string_view { "bump" },
      std::string_view { "patch" }
    };
    std::ostringstream output;
    std::ostringstream error;

    expect(
      forge::cli::run(arguments, directory.path(), output, error) == 0,
      "patch bump succeeds without existing release notes"
    );
    expect(
      contains(read_file(directory.path() / "forge.recipe.toml"), "version = \"1.3.6\""),
      "patch bump increments only the patch version"
    );
    expect(
      contains(read_file(directory.path() / "RELEASE_NOTES.md"), "## 1.3.6"),
      "bump creates missing release notes"
    );
  }

  void test_bump_includes_build_number_in_release_notes()
  {
    TemporaryDirectory directory;
    write_file(
      directory.path() / "forge.recipe.toml",
      "[project]\n"
      "name = \"hello\"\n"
      "version = \"1.3.5\"\n"
      "type = \"executable\"\n"
      "cpp_std = 20\n\n"
      "[build]\n"
      "number = 21\n\n"
      "[sources]\n"
      "paths = [\"main.cpp\"]\n\n"
      "[release]\n"
      "build_number_format = \"dotted\"\n"
    );
    constexpr std::array arguments {
      std::string_view { "bump" },
      std::string_view { "patch" }
    };
    std::ostringstream output;
    std::ostringstream error;

    expect(
      forge::cli::run(arguments, directory.path(), output, error) == 0,
      "bump creates build-qualified release notes when requested"
    );
    expect(
      contains(read_file(directory.path() / "RELEASE_NOTES.md"), "## 1.3.6.22"),
      "release-note heading includes the incremented build number"
    );
    expect(
      contains(output.str(), "Prepared RELEASE_NOTES.md section 1.3.6.22"),
      "bump reports the build-qualified release-note heading"
    );
    expect(error.str().empty(), "build-qualified release-note bump does not write an error");

    TemporaryDirectory semver_directory;
    write_file(
      semver_directory.path() / "forge.recipe.toml",
      "[project]\n"
      "name = \"hello\"\n"
      "version = \"1.3.5\"\n"
      "type = \"executable\"\n"
      "cpp_std = 20\n\n"
      "[build]\n"
      "number = 21\n\n"
      "[sources]\n"
      "paths = [\"main.cpp\"]\n\n"
      "[release]\n"
      "build_number_format = \"semver\"\n"
    );
    std::ostringstream semver_output;
    std::ostringstream semver_error;
    expect(
      forge::cli::run(arguments, semver_directory.path(), semver_output, semver_error) == 0,
      "bump supports SemVer build-qualified release notes"
    );
    expect(
      contains(read_file(semver_directory.path() / "RELEASE_NOTES.md"), "## 1.3.6+build.22"),
      "SemVer release-note heading includes build metadata"
    );
    expect(semver_error.str().empty(), "SemVer release-note bump does not write an error");

    TemporaryDirectory missing_build_directory;
    write_file(
      missing_build_directory.path() / "forge.recipe.toml",
      "[project]\n"
      "name = \"hello\"\n"
      "version = \"1.3.5\"\n"
      "type = \"executable\"\n"
      "cpp_std = 20\n\n"
      "[sources]\n"
      "paths = [\"main.cpp\"]\n\n"
      "[release]\n"
      "build_number_format = \"dotted\"\n"
    );
    std::ostringstream missing_build_output;
    std::ostringstream missing_build_error;
    expect(
      forge::cli::run(
        arguments,
        missing_build_directory.path(),
        missing_build_output,
        missing_build_error
      ) == 2,
      "release-note build-number format requires a build number"
    );
    expect(
      contains(missing_build_error.str(), "requires build.number"),
      "missing build number is explained"
    );
  }

  void test_bump_rejects_invalid_or_duplicate_version()
  {
    TemporaryDirectory directory;
    write_file(
      directory.path() / "forge.recipe.toml",
      "[project]\n"
      "name = \"hello\"\n"
      "version = \"1.3\"\n"
      "type = \"executable\"\n"
      "cpp_std = 20\n\n"
      "[sources]\n"
      "paths = [\"main.cpp\"]\n"
    );
    constexpr std::array minor_arguments {
      std::string_view { "bump" },
      std::string_view { "minor" }
    };
    std::ostringstream invalid_output;
    std::ostringstream invalid_error;

    expect(
      forge::cli::run(minor_arguments, directory.path(), invalid_output, invalid_error) == 2,
      "bump rejects a non-SemVer project version"
    );
    expect(contains(invalid_error.str(), "<major>.<minor>.<patch>"), "invalid version is explained");

    write_file(
      directory.path() / "forge.recipe.toml",
      "[project]\n"
      "name = \"hello\"\n"
      "version = \"1.3.5\"\n"
      "type = \"executable\"\n"
      "cpp_std = 20\n\n"
      "[sources]\n"
      "paths = [\"main.cpp\"]\n"
    );
    write_file(directory.path() / "RELEASE_NOTES.md", "# Release notes\n\n## 1.4.0\n");
    const auto original_recipe = read_file(directory.path() / "forge.recipe.toml");
    std::ostringstream duplicate_output;
    std::ostringstream duplicate_error;

    expect(
      forge::cli::run(minor_arguments, directory.path(), duplicate_output, duplicate_error) == 2,
      "bump rejects duplicate release notes"
    );
    expect(
      read_file(directory.path() / "forge.recipe.toml") == original_recipe,
      "duplicate release notes leave the recipe unchanged"
    );
    expect(contains(duplicate_error.str(), "already exist"), "duplicate release notes are explained");
  }

  void test_bump_rejects_invalid_component()
  {
    constexpr std::array arguments {
      std::string_view { "bump" },
      std::string_view { "build" }
    };
    std::ostringstream output;
    std::ostringstream error;

    expect(forge::cli::run(arguments, output, error) == 2, "bump rejects an invalid component");
    expect(contains(error.str(), "forge bump <major|minor|patch>"), "bump prints its usage");
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
  test_command_help();
  test_cli_builds_workspace();
  test_cli_rejects_invalid_compile_definition();
  test_cli_runs_and_tests_workspace();
  test_version();
  test_init_alias_adopts_existing_project();
  test_init_discovers_existing_sources();
  test_init_ignores_generated_directories();
  test_init_infers_local_include_directories();
  test_adopt_imports_visual_studio_project();
  test_adopt_imports_cmake_project();
  test_adopt_preserves_cmake_interface_library_with_programs();
  test_adopt_accepts_library_type_hint();
  test_adopt_merges_mirrored_cmake_and_visual_studio_projects();
  test_adopt_prefers_cmake_over_generated_solution();
  test_adopt_imports_xcode_project();
  test_adopt_imports_cmake_superproject_as_workspace();
  test_adopt_imports_visual_studio_solution();
  test_adopt_reports_unresolved_dependency_includes();
  test_adopt_infers_sibling_project_dependencies();
  test_adopt_preserves_ambiguous_sibling_includes();
  test_adopt_suggests_github_dependencies_without_network();
  test_adopt_github_verifies_and_pins_dependency();
  test_init_empty_project();
  test_init_infers_library_projects();
  test_init_infers_multiple_executables();
  test_adopt_infers_mapped_runtime_asset();
  test_init_groups_sources_by_local_include_graph();
  test_init_refuses_to_overwrite();
  test_init_preserves_existing_release_support();
  test_workflow_adds_release_boxes_feature();
  test_workflow_feature_refuses_unmanaged_collision();
  test_workflow_feature_lifecycle();
  test_new();
  test_new_refuses_existing_path();
  test_new_requires_simple_name();
  test_new_requires_name();
  test_build_new_project();
  test_build_rejects_empty_project();
  test_build_and_run_named_target();
  test_update_rejects_unknown_dependency();
  test_run_new_project();
  test_release_new_project();
  test_prepare_executable_release();
  test_prepare_release_alias_warns();
  test_release_rejects_empty_tag_format();
  test_release_github_rejects_empty_tag_format();
  test_release_git_force_rejects_empty_tag_format();
  test_release_git_rejects_redundant_tag_argument();
  test_clean_removes_generated_state();
  test_clean_accepts_already_clean_project();
  test_clean_refuses_non_project_directory();
  test_box_round_trip();
  test_static_library_box_round_trip();
  test_header_only_box_round_trip();
  test_imported_library_box_round_trip();
  test_run_with_imported_library_dependency();
  test_run_with_local_dependencies();
  test_run_with_pinned_git_dependency();
  test_dependency_profiles();
  test_run_with_local_box_dependency();
  test_run_with_downloadable_box_dependency();
  test_run_and_release_with_dynamic_dependency();
  test_bump_updates_recipe_and_release_notes();
  test_bump_creates_release_notes();
  test_bump_includes_build_number_in_release_notes();
  test_bump_rejects_invalid_or_duplicate_version();
  test_bump_rejects_invalid_component();
  test_unknown_command();

  return failures == 0 ? 0 : 1;
}
