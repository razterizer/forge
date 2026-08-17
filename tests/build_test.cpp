#include "build.h"
#include "fprocess.h"
#include "recipe.h"
#include "test_support.h"

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <map>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

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

  void write_c_project(const std::filesystem::path& directory)
  {
    std::ofstream recipe { directory / "forge.recipe.toml" };
    recipe
      << "[project]\n"
      << "name = \"hello-c\"\n"
      << "version = \"0.1.0\"\n"
      << "type = \"executable\"\n"
      << "c_std = 11\n\n"
      << "[sources]\n"
      << "paths = [\"main.c\"]\n";

    std::ofstream source { directory / "main.c" };
    source << "int main(void) { return 0; }\n";
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

  void test_recipe_accepts_toml_comments_multiline_arrays_and_escapes()
  {
    TemporaryDirectory directory;
    std::ofstream recipe_file { directory.path() / "forge.recipe.toml" };
    recipe_file
      << "[project] # project metadata\n"
      << "name = \"hello\\u002dworld\" # Unicode escape\n"
      << "version = \"0.1.0\"\n"
      << "type = \"executable\"\n"
      << "cpp_std = 20\n\n"
      << "[sources]\n"
      << "paths = [\n"
      << "  \"main.cpp\", # trailing comments are valid TOML\n"
      << "]\n";
    recipe_file.close();
    forge::Recipe recipe;
    std::ostringstream error;

    expect(
      forge::read_recipe(directory.path() / "forge.recipe.toml", recipe, error),
      "recipe accepts TOML comments, multiline arrays, and escapes"
    );
    expect(recipe.name == "hello-world", "recipe decodes TOML Unicode escapes");
    expect(
      recipe.sources.size() == 1 && recipe.sources.front() == "main.cpp",
      "recipe parses a multiline source array"
    );
    expect(error.str().empty(), "valid TOML recipe writes no error");
  }

  void write_u16(std::ofstream& file, std::uint16_t value)
  {
    file.put(static_cast<char>(value & 0xff));
    file.put(static_cast<char>((value >> 8) & 0xff));
  }

  void write_u32(std::ofstream& file, std::uint32_t value)
  {
    write_u16(file, static_cast<std::uint16_t>(value & 0xffff));
    write_u16(file, static_cast<std::uint16_t>((value >> 16) & 0xffff));
  }

  void write_test_zip(const std::filesystem::path& path,
                      const std::vector<std::string>& entries)
  {
    std::ofstream file { path, std::ios::binary };
    const auto central_offset = static_cast<std::uint32_t>(file.tellp());

    for (const auto& entry : entries)
    {
      write_u32(file, 0x02014b50);
      write_u16(file, 20);
      write_u16(file, 20);
      write_u16(file, 0);
      write_u16(file, 0);
      write_u16(file, 0);
      write_u16(file, 0);
      write_u32(file, 0);
      write_u32(file, 0);
      write_u32(file, 0);
      write_u16(file, static_cast<std::uint16_t>(entry.size()));
      write_u16(file, 0);
      write_u16(file, 0);
      write_u16(file, 0);
      write_u16(file, 0);
      write_u32(file, 0);
      write_u32(file, 0);
      file << entry;
    }

    const auto central_size =
      static_cast<std::uint32_t>(static_cast<std::streamoff>(file.tellp()) - central_offset);
    write_u32(file, 0x06054b50);
    write_u16(file, 0);
    write_u16(file, 0);
    write_u16(file, static_cast<std::uint16_t>(entries.size()));
    write_u16(file, static_cast<std::uint16_t>(entries.size()));
    write_u32(file, central_size);
    write_u32(file, central_offset);
    write_u16(file, 0);
  }

  bool fake_cmake_tar(const std::vector<std::string>& arguments,
                      const std::filesystem::path& working_directory,
                      std::map<std::filesystem::path, std::filesystem::path>& archives)
  {
    if (arguments.size() < 5 || arguments[0] != "cmake" || arguments[1] != "-E" || arguments[2] != "tar")
      return false;

    if (arguments[3] == "cf" && arguments.size() >= 7)
    {
      const std::filesystem::path archive = arguments[4];
      std::vector<std::string> entries;

      for (std::size_t index = 6; index < arguments.size(); ++index)
      {
        const auto root = std::filesystem::path { arguments[index] };

        if (std::filesystem::is_directory(working_directory / root))
        {
          entries.push_back(root.generic_string() + "/");

          for (const auto& entry : std::filesystem::recursive_directory_iterator { working_directory / root })
          {
            const auto relative = entry.path().lexically_relative(working_directory).generic_string();

            if (entry.is_directory())
              entries.push_back(relative + "/");
            else if (entry.is_regular_file())
              entries.push_back(relative);
          }
        }
        else
        {
          entries.push_back(root.generic_string());
        }
      }

      write_test_zip(archive, entries);
      archives[archive] = working_directory;
      return true;
    }

    if (arguments[3] == "xf" && arguments.size() >= 5)
    {
      const auto source = archives.find(arguments[4]);

      if (source == archives.end())
        return false;

      for (const auto& entry : std::filesystem::directory_iterator { source->second })
      {
        std::filesystem::copy(
          entry.path(),
          working_directory / entry.path().filename(),
          std::filesystem::copy_options::recursive | std::filesystem::copy_options::overwrite_existing
        );
      }

      return true;
    }

    return false;
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
      << "include_dirs = [\"include/hello\"]\n"
      << "macos_frameworks = [\"AudioToolbox\"]\n"
      << "linux_libraries = [\"asound\"]\n"
      << "windows_libraries = [\"ole32\"]\n\n"
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
    expect(
      contains(generated, "add_compile_definitions(NOMINMAX)"),
      "generated CMake disables Windows min and max macros"
    );
    expect(contains(generated, "add_executable(forge_project"), "build generates an executable target");
    expect(contains(generated, "forge-toolchain.toml"), "build records the selected toolchain identity");
    expect(contains(generated, "CMAKE_CXX_COMPILER_ID"), "toolchain identity records the compiler");
    expect(contains(generated, "main.cpp"), "build includes recipe sources");
    expect(contains(generated, "cxx_std_20"), "build includes the requested C++ standard");
    expect(error.str().empty(), "successful unit build does not write an error");
  }

  void test_build_generates_c_cmake()
  {
    TemporaryDirectory directory;
    write_c_project(directory.path());
    std::ostringstream output;
    std::ostringstream error;

    const forge::ProcessRunner runner =
      [](const std::vector<std::string>&, const std::filesystem::path&, std::ostream&)
      {
        return 0;
      };

    expect(
      forge::build_project(directory.path(), runner, output, error) == 0,
      "build accepts a C-only recipe without a C++ standard"
    );
    const auto generated = read_file(directory.path() / ".forge/generated/CMakeLists.txt");
    expect(contains(generated, "project(forge_project LANGUAGES CXX C)"), "build enables C for C sources");
    expect(contains(generated, "main.c"), "build includes C sources");
    expect(contains(generated, "c_std_11"), "build applies the requested C standard");
    expect(!contains(generated, "target_compile_features(forge_project PUBLIC cxx_std_"), "C-only targets do not request a C++ standard");
    expect(error.str().empty(), "C-only recipe does not write an error");
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
                     const std::filesystem::path& working_directory,
                     std::ostream&)
      {
        ++invocations;
        std::filesystem::create_directories(working_directory / ".forge/build");
        std::ofstream { working_directory / ".forge/build/forge-toolchain.toml" }
          << "compiler = \"ExampleCompiler\"\n"
          << "compiler_version = \"1.0\"\n"
          << "cpp_std = 20\n"
          << "configuration = \"Debug\"\n"
          << "runtime = \"default\"\n";
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
      << "macos_system_include_dirs = [\"/opt/homebrew/opt/example/include\"]\n"
      << "macos_system_library_dirs = [\"/opt/homebrew/opt/example/lib\"]\n"
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
      contains(generated, "SYSTEM PRIVATE \"/opt/homebrew/opt/example/include\""),
      "build profile adds a platform system include directory"
    );
    expect(
      contains(generated, "target_link_directories(forge_project PRIVATE \"/opt/homebrew/opt/example/lib\")"),
      "build profile adds a platform system library directory"
    );
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

  void test_build_applies_selector_rules()
  {
    TemporaryDirectory directory;
    write_project(directory.path());
    std::ofstream recipe { directory.path() / "forge.recipe.toml", std::ios::app };
    recipe
      << "\n[build.config.debug]\n"
      << "configuration = \"Debug\"\n"
      << "defines = [\"DEBUG_BUILD\"]\n"
      << "\n[build.config.release]\n"
      << "configuration = \"Release\"\n"
      << "defines = [\"RELEASE_BUILD\"]\n"
      << "\n[build.config.-.profile.applaudio]\n"
      << "defines = [\"USE_APPLAUDIO\"]\n";
    recipe.close();
    forge::BuildOptions options;
    options.config = "release";
    options.profile = "applaudio";
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
      "build succeeds with selector rules"
    );
    const auto generated = read_file(directory.path() / ".forge/generated/CMakeLists.txt");
    expect(contains(generated, "\"RELEASE_BUILD\""), "config selector applies release settings");
    expect(!contains(generated, "\"DEBUG_BUILD\""), "config selector excludes debug settings");
    expect(contains(generated, "\"USE_APPLAUDIO\""), "wildcard config combines with profile selector");
    expect(
      commands.size() == 2 && commands[1][4] == "Release",
      "config selector selects the CMake configuration"
    );
    expect(error.str().empty(), "successful selector build does not write an error");
  }

  void test_effective_build_selection_centralizes_legacy_and_selector_resolution()
  {
    forge::Recipe selector_recipe;
    selector_recipe.default_style = "github-package";
    selector_recipe.build_rules.push_back(
      { { { "config", "release" }, { "profile", "cinema" } }, { "Release", 23, {}, {}, {}, {}, {}, {}, {}, { "CINEMA" } } }
    );
    forge::EffectiveBuildSelection selector_selection;
    std::ostringstream selector_error;
    const forge::BuildSelectionRequest selector_request {
      .profile = "cinema",
      .platform = "macos-arm64",
      .selector_configuration = "release",
      .select_target = false
    };

    expect(
      forge::resolve_effective_build_selection(
        selector_recipe,
        selector_request,
        selector_selection,
        selector_error
      ),
      "effective selection resolves selector profiles"
    );
    expect(!selector_selection.legacy_profile, "selector profile is not treated as a legacy profile");
    expect(
      selector_selection.selectors.style == "github-package"
        && selector_selection.selectors.profile == "cinema"
        && selector_selection.configuration == "Release"
        && selector_recipe.cpp_standard == 23,
      "effective selection applies one consistent selector configuration"
    );
    expect(selector_error.str().empty(), "effective selector resolution does not write an error");

    forge::Recipe legacy_recipe;
    legacy_recipe.build_profiles["workflow-release"].configuration = "Release";
    legacy_recipe.build_rules.push_back(
      { { { "config", "debug" } }, { "Debug", 0, {}, {}, {}, {}, {}, {}, {}, { "SHOULD_NOT_APPLY" } } }
    );
    forge::EffectiveBuildSelection legacy_selection;
    std::ostringstream legacy_error;
    const forge::BuildSelectionRequest legacy_request {
      .profile = "workflow-release",
      .platform = "macos-arm64",
      .select_target = false
    };

    expect(
      forge::resolve_effective_build_selection(
        legacy_recipe,
        legacy_request,
        legacy_selection,
        legacy_error
      ),
      "effective selection resolves legacy profiles"
    );
    expect(
      legacy_selection.legacy_profile
        && *legacy_selection.legacy_profile == "workflow-release"
        && legacy_selection.selectors.profile.empty()
        && legacy_selection.configuration == "Release"
        && legacy_recipe.compile_definitions.empty(),
      "legacy profiles bypass selector rules in the central resolver"
    );
    expect(legacy_error.str().empty(), "effective legacy resolution does not write an error");
  }

  void test_build_applies_default_selector_profile()
  {
    TemporaryDirectory directory;
    write_project(directory.path());
    std::ofstream recipe { directory.path() / "forge.recipe.toml", std::ios::app };
    recipe
      << "\n[defaults]\n"
      << "style = \"github-package\"\n"
      << "profile = \"applaudio\"\n"
      << "\n[build.profile.applaudio]\n"
      << "defines = [\"USE_APPLAUDIO\"]\n"
      << "\n[build.profile.openal]\n"
      << "defines = [\"USE_OPENAL\"]\n";
    recipe.close();
    const forge::ProcessRunner runner =
      [](const std::vector<std::string>&,
         const std::filesystem::path&,
         std::ostream&)
      {
        return 0;
      };
    std::ostringstream default_output;
    std::ostringstream default_error;

    expect(
      forge::build_project(
        directory.path(),
        forge::BuildOptions {},
        runner,
        default_output,
        default_error
      ) == 0,
      "build succeeds with a default selector profile"
    );
    const auto default_generated = read_file(directory.path() / ".forge/generated/CMakeLists.txt");
    expect(
      contains(default_generated, "\"USE_APPLAUDIO\"")
        && !contains(default_generated, "\"USE_OPENAL\""),
      "default selector profile applies when --profile is omitted"
    );
    expect(default_error.str().empty(), "default selector profile does not write an error");

    forge::BuildOptions explicit_options;
    explicit_options.profile = "openal";
    std::ostringstream explicit_output;
    std::ostringstream explicit_error;
    expect(
      forge::build_project(
        directory.path(),
        explicit_options,
        runner,
        explicit_output,
        explicit_error
      ) == 0,
      "build succeeds with an explicit selector profile"
    );
    const auto explicit_generated = read_file(directory.path() / ".forge/generated/CMakeLists.txt");
    expect(
      contains(explicit_generated, "\"USE_OPENAL\"")
        && !contains(explicit_generated, "\"USE_APPLAUDIO\""),
      "explicit selector profile overrides the recipe default"
    );
    expect(explicit_error.str().empty(), "explicit selector profile does not write an error");
  }

  void test_build_rejects_selector_that_omits_required_dependency()
  {
    TemporaryDirectory directory;
    write_project(directory.path());
    std::ofstream { directory.path() / "main.cpp" }
      << "#include <8Beat/AudioSourceHandler.h>\nint main() {}\n";
    std::ofstream recipe { directory.path() / "forge.recipe.toml", std::ios::app };
    recipe
      << "\n[dependencies.style.github-package.profile.applaudio]\n"
      << "8Beat = { github = \"example/8Beat\", version = \"1.0.0\" }\n";
    recipe.close();
    forge::BuildOptions options;
    options.style = "github-package";
    options.profile = "openal";
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
      "build rejects a selector combination that omits a required dependency"
    );
    expect(invocations == 0, "missing selected dependency fails before external tools run");
    expect(
      contains(error.str(), "selected dependency rules omit '8Beat'")
        && contains(error.str(), "profile 'openal'"),
      "missing selected dependency explains the selector mismatch"
    );
  }

  void test_dependency_style_validates_rule_shape()
  {
    TemporaryDirectory directory;
    write_project(directory.path());
    std::ofstream recipe { directory.path() / "forge.recipe.toml", std::ios::app };
    recipe
      << "\n[dependencies.style.local-source]\n"
      << "Core = { github = \"razterizer/Core\", version = \"1.5.0+build.8\" }\n";
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
      "dependency style rejects a mismatched declaration"
    );
    expect(invocations == 0, "invalid dependency style does not invoke external tools");
    expect(contains(error.str(), "invalid recipe value"), "style mismatch identifies the recipe line");
  }

  void test_selector_recipe_keeps_legacy_workflow_profile()
  {
    TemporaryDirectory directory;
    write_project(directory.path());
    std::ofstream recipe { directory.path() / "forge.recipe.toml", std::ios::app };
    recipe
      << "\n[build.config.debug]\n"
      << "configuration = \"Debug\"\n"
      << "defines = [\"DEBUG_BUILD\"]\n"
      << "\n[profile.workflow-release.build]\n"
      << "configuration = \"Release\"\n"
      << "defines = [\"WORKFLOW_RELEASE\"]\n";
    recipe.close();
    forge::BuildOptions options;
    options.profile = "workflow-release";
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
      "selector recipe accepts the legacy workflow-release profile"
    );
    const auto generated = read_file(directory.path() / ".forge/generated/CMakeLists.txt");
    expect(contains(generated, "\"WORKFLOW_RELEASE\""), "legacy workflow settings are applied");
    expect(!contains(generated, "\"DEBUG_BUILD\""), "selector rules do not leak into legacy workflow builds");
    expect(commands.size() == 2 && commands[1][4] == "Release", "legacy workflow stays Release");
    expect(error.str().empty(), "legacy workflow migration build does not write an error");
  }

  void test_build_generates_system_package_hint_diagnostics()
  {
    TemporaryDirectory directory;
    write_project(directory.path());
    std::ofstream recipe { directory.path() / "forge.recipe.toml", std::ios::app };
    recipe
      << "\n[build]\n"
      << "macos_libraries = [\"openal\"]\n"
      << "macos_brew_packages = [\"openal-soft\"]\n"
      << "linux_libraries = [\"openal\"]\n"
      << "linux_apt_packages = [\"libopenal-dev\"]\n";
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
      "build succeeds while generating system package hint diagnostics"
    );
    const auto generated = read_file(directory.path() / ".forge/generated/CMakeLists.txt");
    expect(
      contains(generated, "forge: missing system library 'openal'; install provider package with: brew install openal-soft"),
      "macOS system library diagnostics include the Homebrew provider hint"
    );
    expect(
      contains(generated, "forge: missing system library 'openal'; install provider package with: sudo apt install libopenal-dev"),
      "Linux system library diagnostics include the apt provider hint"
    );
  }

  void test_local_dependency_replaces_selected_system_provider()
  {
    TemporaryDirectory directory;
    const auto application = directory.path() / "application";
    const auto spdlog = directory.path() / "spdlog";
    std::filesystem::create_directories(application / "include/application");
    std::filesystem::create_directories(spdlog / "include/spdlog");
    std::ofstream { application / "include/application/application.h" } << "#pragma once\n";
    std::ofstream { spdlog / "forge.recipe.toml" }
      << "[project]\n"
      << "name = \"spdlog\"\n"
      << "version = \"1.0.0\"\n"
      << "type = \"header_only\"\n"
      << "cpp_std = 20\n\n"
      << "[sources]\n"
      << "paths = []\n"
      << "public_headers = [\"include/spdlog/spdlog.h\"]\n";
    std::ofstream { spdlog / "include/spdlog/spdlog.h" } << "#pragma once\n";
    std::ofstream recipe { application / "forge.recipe.toml" };
    recipe
      << "[project]\n"
      << "name = \"application\"\n"
      << "version = \"1.0.0\"\n"
      << "type = \"header_only\"\n"
      << "cpp_std = 20\n\n"
      << "[sources]\n"
      << "paths = []\n"
      << "public_headers = [\"include/application/application.h\"]\n\n"
      << "[build]\n"
      << "macos_libraries = [\"spdlog\"]\n"
      << "macos_brew_packages = [\"fmt\", \"spdlog\"]\n"
      << "linux_libraries = [\"spdlog\"]\n"
      << "linux_apt_packages = [\"libfmt-dev\", \"libspdlog-dev\"]\n"
      << "windows_libraries = [\"spdlog\"]\n"
      << "\n[dependencies.style.local-source]\n"
      << "spdlog = { path = \"../spdlog\", provides = [\"spdlog\"] }\n";
    recipe.close();
    forge::BuildOptions options;
    options.style = "local-source";
    std::ostringstream output;
    std::ostringstream error;
    std::map<std::filesystem::path, std::filesystem::path> archives;
    const forge::ProcessRunner runner =
      [&archives](const std::vector<std::string>& arguments,
         const std::filesystem::path& working_directory,
         std::ostream&)
      {
        if (arguments.size() > 2 && arguments[1] == "-E" && arguments[2] == "tar")
          return fake_cmake_tar(arguments, working_directory, archives) ? 0 : 2;

        return 0;
      };

    const auto result = forge::build_project(application, options, runner, output, error);
    expect(
      result == 0,
      "local provider dependency builds without its package-manager requirement"
    );
    const auto generated = read_file(application / ".forge/generated/CMakeLists.txt");
    expect(
      !contains(generated, "find_library(FORGE_forge_project_MACOS_LIBRARY_spdlog spdlog)")
        && !contains(generated, "brew --prefix \"spdlog\"")
        && !contains(generated, "brew --prefix \"fmt\""),
      "local spdlog provider suppresses its system libraries and provider packages"
    );
    expect(contains(output.str(), "Resolving dependency spdlog"), "local provider dependency is resolved");
    expect(error.str().empty(), "local provider replacement is clean");
  }

  void test_build_uses_ancestor_vcpkg_manifest_paths()
  {
    TemporaryDirectory directory;
    const auto application = directory.path() / "application";
    write_header_only_project(application);
    std::ofstream { directory.path() / "vcpkg.json" }
      << "{\"name\":\"example\",\"version\":\"1.0.0\",\"dependencies\":[\"stb\"]}\n";
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
      forge::build_project(application, runner, output, error) == 0,
      "build accepts a vcpkg manifest inherited from its project root"
    );
    const auto generated = read_file(application / ".forge/generated/CMakeLists.txt");
#if defined(__APPLE__) && (defined(__aarch64__) || defined(__arm64__))
    constexpr auto triplet = "arm64-osx";
#elif defined(__APPLE__)
    constexpr auto triplet = "x64-osx";
#elif defined(_WIN32)
    constexpr auto triplet = "x64-windows";
#elif defined(__aarch64__) || defined(__arm64__)
    constexpr auto triplet = "arm64-linux";
#else
    constexpr auto triplet = "x64-linux";
#endif
    expect(
      contains(
        generated,
        "vcpkg_installed/" + std::string { triplet } + "/include"
      ),
      "generated CMake exposes the vcpkg manifest include directory"
    );
    expect(error.str().empty(), "vcpkg manifest path setup is clean");
  }

  void test_build_generates_named_target_system_package_hint_diagnostics()
  {
    TemporaryDirectory directory;
    write_multi_target_project(directory.path());
    std::ofstream recipe { directory.path() / "forge.recipe.toml", std::ios::app };
    recipe
      << "\n[target.hello]\n"
      << "macos_brew_packages = [\"openal-soft\"]\n"
      << "linux_apt_packages = [\"libasound2-dev\"]\n";
    recipe.close();
    forge::BuildOptions options;
    options.target = "examples";
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
      "named target build succeeds while generating system package hint diagnostics"
    );
    const auto generated = read_file(directory.path() / ".forge/generated/examples/CMakeLists.txt");
    expect(
      contains(generated, "forge: missing system library 'AudioToolbox'; install provider package with: brew install openal-soft"),
      "named target framework diagnostics include the Homebrew provider hint"
    );
    expect(
      contains(generated, "forge: missing system library 'asound'; install provider package with: sudo apt install libasound2-dev"),
      "named target Linux library diagnostics include the apt provider hint"
    );
  }

  void test_build_skips_dependencies_filtered_to_other_targets()
  {
    TemporaryDirectory directory;
    write_project(directory.path());
    std::ofstream recipe { directory.path() / "forge.recipe.toml", std::ios::app };
    recipe
      << "\n[dependencies]\n"
      << "missing_windows_sdk = { path = \"missing\", targets = [\"windows-x86_64\"] }\n";
    recipe.close();
    forge::BuildOptions options;
    options.update_target = "linux-x86_64";
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
      "build skips dependencies filtered to a different target"
    );
    expect(commands.size() == 2, "filtered dependency does not invoke dependency tooling");
    expect(error.str().empty(), "filtered dependency build does not write an error");
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
    expect(
      contains(generated, "find_library(FORGE_forge_internal_0_FRAMEWORK_AudioToolbox AudioToolbox)")
        && contains(generated, "find_library(FORGE_forge_internal_0_LINUX_LIBRARY_asound asound)")
        && contains(generated, "target_link_libraries(forge_internal_0 INTERFACE ole32)"),
      "internal libraries propagate platform system-link requirements"
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
      [&directory](const std::vector<std::string>& arguments,
         const std::filesystem::path&,
         std::ostream&)
      {
        if (arguments.size() > 1 && arguments[1] == "--build")
        {
          std::filesystem::create_directories(directory.path() / ".forge/build");
#ifdef _WIN32
          std::ofstream { directory.path() / ".forge/build/hello.exe" };
#else
          std::ofstream { directory.path() / ".forge/build/hello" };
#endif
        }

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
    expect(
      read_file(
        directory.path() / ".forge/run/hello/Debug/local-source/default/assets/message.txt"
      ) == "hello\n",
      "build caches runtime assets with the runnable variant"
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

  void test_build_rejects_unsafe_runtime_asset_manifest()
  {
    TemporaryDirectory directory;
    write_project(directory.path());
    std::filesystem::create_directories(directory.path() / ".forge/build/.forge");
    std::ofstream { directory.path() / "victim.txt" } << "keep me\n";
    std::ofstream { directory.path() / ".forge/build/.forge/runtime-assets.txt" }
      << "../../victim.txt\n";
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
      "build rejects an unsafe stale runtime asset manifest"
    );
    expect(invocations == 0, "unsafe runtime asset cleanup invokes no external tools");
    expect(
      std::filesystem::exists(directory.path() / "victim.txt"),
      "unsafe runtime asset cleanup preserves files outside the build directory"
    );
    expect(contains(error.str(), "unsafe path"), "unsafe runtime asset cleanup is explained");
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

  void test_build_stages_dependency_runtime_assets()
  {
    TemporaryDirectory directory;
    const auto dependency = directory.path() / "dependency";
    const auto application = directory.path() / "application";
    std::filesystem::create_directories(dependency / "include/dependency");
    std::filesystem::create_directories(dependency / "assets/fonts");
    std::filesystem::create_directories(application);
    std::ofstream dependency_recipe { dependency / "forge.recipe.toml" };
    dependency_recipe
      << "[project]\n"
      << "name = \"dependency\"\n"
      << "version = \"1.0.0\"\n"
      << "type = \"header_only\"\n"
      << "cpp_std = 20\n\n"
      << "[sources]\n"
      << "paths = []\n"
      << "public_headers = [\"include/dependency/dependency.h\"]\n\n"
      << "[runtime]\n"
      << "files = [{ source = \"assets/fonts\", destination = \"dependency/fonts\" }]\n";
    dependency_recipe.close();
    std::ofstream { dependency / "include/dependency/dependency.h" } << "#pragma once\n";
    std::ofstream { dependency / "assets/fonts/font.txt" } << "font\n";
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
      << "dependency = { path = \"../dependency\" }\n";
    application_recipe.close();
    std::ofstream { application / "main.cpp" } << "int main() {}\n";
    std::ostringstream output;
    std::ostringstream error;
    std::map<std::filesystem::path, std::filesystem::path> archives;

    const forge::ProcessRunner runner =
      [&application, &dependency, &archives](const std::vector<std::string>& arguments,
                                             const std::filesystem::path& working_directory,
                                             std::ostream&)
      {
        if (arguments.size() > 1 && arguments[1] == "--build")
        {
          const auto build_directory = working_directory / ".forge/build";
          std::filesystem::create_directories(build_directory);
          std::ofstream { build_directory / "forge-toolchain.toml" }
            << "compiler = \"ExampleCompiler\"\n"
            << "compiler_version = \"1.0\"\n"
            << "cpp_std = 20\n"
            << "configuration = \"Debug\"\n"
            << "runtime = \"default\"\n";

          if (working_directory == application)
          {
#ifdef _WIN32
            std::ofstream { build_directory / "application.exe" } << "exe\n";
#else
            std::ofstream { build_directory / "application" } << "exe\n";
#endif
          }

          if (working_directory == dependency)
            return 0;
        }

        if (arguments.size() > 2 && arguments[1] == "-E" && arguments[2] == "tar")
          return fake_cmake_tar(arguments, working_directory, archives) ? 0 : 2;

        return 0;
      };

    expect(
      forge::build_project(application, runner, output, error) == 0,
      "build succeeds with dependency runtime assets"
    );
    expect(
      read_file(application / ".forge/build/dependency/fonts/font.txt") == "font\n",
      "build stages dependency runtime assets beside the executable"
    );
    expect(error.str().empty(), "dependency runtime asset build does not write an error");
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

  void test_build_rejects_source_with_embedded_parent_path()
  {
    TemporaryDirectory directory;
    const auto project = directory.path() / "project";
    std::filesystem::create_directories(project / "inside");
    std::ofstream { directory.path() / "outside.cpp" } << "int main() {}\n";
    std::ofstream recipe { project / "forge.recipe.toml" };
    recipe
      << "[project]\n"
      << "name = \"hello\"\n"
      << "version = \"0.1.0\"\n"
      << "type = \"executable\"\n"
      << "cpp_std = 20\n\n"
      << "[sources]\n"
      << "paths = [\"inside/../../outside.cpp\"]\n";
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
      forge::build_project(project, runner, output, error) == 2,
      "build rejects a source path with embedded parent traversal"
    );
    expect(invocations == 0, "embedded source traversal invokes no external tools");
    expect(contains(error.str(), "stay inside the project"), "source traversal is explained");
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

  void test_build_accepts_named_target_dependency()
  {
    TemporaryDirectory directory;
    const auto dependency = directory.path() / "dependency";
    const auto application = directory.path() / "application";
    std::filesystem::create_directories(dependency / "include/targetlib");
    std::filesystem::create_directories(application);
    std::ofstream dependency_recipe { dependency / "forge.recipe.toml" };
    dependency_recipe
      << "[project]\n"
      << "name = \"Package Display Name\"\n"
      << "version = \"1.0.0\"\n\n"
      << "[target.targetlib]\n"
      << "type = \"header_only\"\n"
      << "cpp_std = 20\n"
      << "sources = []\n"
      << "public_headers = [\"include/targetlib/targetlib.h\"]\n";
    dependency_recipe.close();
    std::ofstream { dependency / "include/targetlib/targetlib.h" } << "#pragma once\n";
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
      << "targetlib = { path = \"../dependency\" }\n";
    application_recipe.close();
    std::ofstream { application / "main.cpp" } << "int main() {}\n";
    std::ostringstream output;
    std::ostringstream error;
    std::map<std::filesystem::path, std::filesystem::path> archives;

    const forge::ProcessRunner runner =
      [&application, &archives](const std::vector<std::string>& arguments,
                                const std::filesystem::path& working_directory,
                                std::ostream&)
      {
        if (arguments.size() > 1 && arguments[1] == "--build")
        {
          const auto build_directory = working_directory / ".forge/build";
          std::filesystem::create_directories(build_directory);
          std::ofstream { build_directory / "forge-toolchain.toml" }
            << "compiler = \"ExampleCompiler\"\n"
            << "compiler_version = \"1.0\"\n"
            << "cpp_std = 20\n"
            << "configuration = \"Debug\"\n"
            << "runtime = \"default\"\n";

          if (working_directory == application)
          {
#ifdef _WIN32
            std::ofstream { build_directory / "application.exe" } << "exe\n";
#else
            std::ofstream { build_directory / "application" } << "exe\n";
#endif
          }
        }

        if (arguments.size() > 2 && arguments[1] == "-E" && arguments[2] == "tar")
          return fake_cmake_tar(arguments, working_directory, archives) ? 0 : 2;

        return 0;
      };

    expect(
      forge::build_project(application, runner, output, error) == 0,
      "build accepts a dependency named after a local library target"
    );
    expect(error.str().empty(), "named target dependency build does not write an error");
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
      << "\n[profile.pinned.dependencies]\n"
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
    options.profile = "pinned";
    expect(
      forge::build_project(directory.path(), options, runner, output, error) == 2,
      "profile-selected GitHub dependency update reaches box validation"
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

  void test_named_update_skips_other_unlocked_github_dependencies()
  {
    TemporaryDirectory directory;
    write_project(directory.path());
    std::ofstream recipe { directory.path() / "forge.recipe.toml", std::ios::app };
    recipe
      << "\n[profile.pinned.dependencies]\n"
      << "answer = { github = \"example/answer\", version = \"1.2.3+build.6\" }\n"
      << "other = { github = \"example/other\", version = \"2.0.0+build.1\" }\n";
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
    options.dependencies_only = true;
    options.update_dependencies = true;
    options.update_dependency = "answer";
    options.profile = "pinned";
    expect(
      forge::build_project(directory.path(), options, runner, output, error) == 2,
      "named profile update reaches requested dependency box validation"
    );
    expect(commands.size() == 2, "named profile update downloads only the requested dependency");
    expect(
      !contains(error.str(), "other"),
      "named profile update does not require unrelated GitHub dependencies"
    );
  }

  void test_update_resolves_github_dependency_for_requested_target()
  {
    TemporaryDirectory directory;
    write_project(directory.path());
    std::ofstream recipe { directory.path() / "forge.recipe.toml", std::ios::app };
    recipe
      << "\n[profile.pinned.dependencies]\n"
      << "answer = { github = \"example/answer\", version = \"1.2.3+build.6\" }\n";
    recipe.close();
    std::vector<std::vector<std::string>> commands;
    std::ostringstream output;
    std::ostringstream error;
    const std::string checksum =
      "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855";
    const std::string target = "windows-x86_64";
    const auto asset = "answer-1.2.3+build.6-" + target + ".cbox";
    const std::string release_url =
      "https://github.com/example/answer/releases/download/release-1.2.3.6/";

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
    options.update_target = target;
    options.profile = "pinned";
    expect(
      forge::build_project(directory.path(), options, runner, output, error) == 2,
      "requested-target GitHub dependency update reaches box validation"
    );
    expect(commands.size() >= 2, "requested-target update downloads checksum before box");

    if (commands.size() >= 2)
    {
      expect(
        commands[0][1] == "-DURL=" + release_url + asset + ".sha256",
        "requested-target update resolves the checksum asset URL"
      );
      expect(
        commands[1][1] == "-DURL=" + release_url + asset,
        "requested-target update resolves the box asset URL"
      );
    }
  }

  void test_update_resolves_github_dependency_variant()
  {
    TemporaryDirectory directory;
    write_project(directory.path());
    std::ofstream recipe { directory.path() / "forge.recipe.toml", std::ios::app };
    recipe
      << "\n[profile.pinned.dependencies]\n"
      << "answer = { github = \"example/answer\", version = \"1.2.3+build.6\", "
         "variant = \"applaudio\" }\n";
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

    const auto asset = "answer-1.2.3+build.6-applaudio-" + target + ".cbox";
    const std::string release_url =
      "https://github.com/example/answer/releases/download/release-1.2.3.6/";
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
    options.dependencies_only = true;
    options.update_dependencies = true;
    options.profile = "pinned";
    expect(
      forge::build_project(directory.path(), options, runner, output, error) == 2,
      "variant GitHub dependency update reaches box validation"
    );
    expect(commands.size() == 2, "variant update downloads checksum and box once");

    if (commands.size() == 2)
    {
      expect(
        commands[0][1] == "-DURL=" + release_url + asset + ".sha256",
        "variant update resolves the variant checksum asset URL"
      );
      expect(
        commands[1][1] == "-DURL=" + release_url + asset,
        "variant update resolves the variant box asset URL"
      );
    }

    expect(
      contains(output.str(), "Resolving GitHub dependency answer variant applaudio"),
      "variant update reports the selected cbox variant"
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

    std::ostringstream profile_output;
    std::ostringstream profile_error;
    forge::BuildOptions profile_options;
    profile_options.profile = "pinned";
    {
      std::ofstream profile_recipe { directory.path() / "forge.recipe.toml", std::ios::app };
      profile_recipe
        << "\n[profile.pinned.dependencies]\n"
        << "answer = { github = \"example/answer\", version = \"1.2.3\" }\n";
    }
    expect(
      forge::build_project(directory.path(), profile_options, runner, profile_output, profile_error) == 2,
      "profile build rejects an unlocked GitHub dependency"
    );
    expect(
      contains(profile_error.str(), "run forge update answer --profile=pinned"),
      "unlocked profile dependency explains how to resolve it with the selected profile"
    );

    const std::string checksum =
      "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855";
    std::ofstream lock { directory.path() / "forge.lock.toml" };
    lock
      << "format = 1\n\n"
      << "[[dependency]]\n"
      << "name = \"answer\"\n"
      << "github = \"example/answer\"\n"
      << "version = \"1.2.3\"\n"
      << "target = \"any\"\n"
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
    expect(
      contains(output.str(), "Using locked dependency answer for any"),
      "build reuses a portable locked dependency on the current target"
    );

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

  void test_build_uses_local_dependency_before_github_fallback()
  {
    TemporaryDirectory directory;
    const auto dependency = directory.path() / "answer";
    const auto application = directory.path() / "app";
    std::filesystem::create_directories(dependency / "include/answer");
    std::filesystem::create_directories(application);
    std::ofstream dependency_recipe { dependency / "forge.recipe.toml" };
    dependency_recipe
      << "[project]\n"
      << "name = \"answer\"\n"
      << "version = \"0.1.0\"\n"
      << "type = \"header_only\"\n"
      << "cpp_std = 20\n\n"
      << "[sources]\n"
      << "paths = []\n"
      << "public_headers = [\"include/answer/answer.h\"]\n";
    dependency_recipe.close();
    std::ofstream { dependency / "include/answer/answer.h" } << "#pragma once\n";
    std::ofstream application_recipe { application / "forge.recipe.toml" };
    application_recipe
      << "[project]\n"
      << "name = \"app\"\n"
      << "version = \"1.0.0\"\n"
      << "type = \"executable\"\n"
      << "cpp_std = 20\n\n"
      << "[sources]\n"
      << "paths = [\"main.cpp\"]\n\n"
      << "[dependencies]\n"
      << "answer = { path = \"../answer\", github = \"example/answer\", version = \"1.2.3\" }\n";
    application_recipe.close();
    std::ofstream { application / "main.cpp" } << "int main() { return 0; }\n";
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
      "build reaches local dependency packaging before its GitHub fallback"
    );
    expect(invocations > 0, "local fallback build invokes normal build tools");
    expect(
      contains(output.str(), "Resolving dependency answer"),
      "available local dependency is resolved from its path"
    );
    expect(
      !contains(output.str(), "Using locked dependency answer"),
      "available local dependency does not consume the GitHub lock"
    );

    std::filesystem::remove_all(dependency);
    invocations = 0;
    output.str({});
    error.str({});

    expect(
      forge::build_project(application, runner, output, error) == 2,
      "build falls back to GitHub locking when the local dependency is missing"
    );
    expect(invocations == 0, "missing local dependency without a lock does not invoke external tools");
    expect(
      contains(error.str(), "run forge update answer"),
      "missing local dependency explains how to lock the GitHub fallback"
    );

    const std::string checksum =
      "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855";
    std::ofstream lock { application / "forge.lock.toml" };
    lock
      << "format = 2\n\n"
      << "[[dependency]]\n"
      << "name = \"answer\"\n"
      << "github = \"example/answer\"\n"
      << "package = \"answer\"\n"
      << "version = \"1.2.3\"\n"
      << "target = \"any\"\n"
      << "url = \"https://example.invalid/answer.cbox\"\n"
      << "sha256 = \"" << checksum << "\"\n";
    lock.close();
    std::filesystem::create_directories(application / ".forge/cache/downloads");
    std::ofstream { application / ".forge/cache/downloads" / (checksum + ".cbox") };
    invocations = 0;
    output.str({});
    error.str({});

    expect(
      forge::build_project(application, runner, output, error) == 2,
      "missing local dependency uses its locked GitHub fallback"
    );
    expect(invocations == 0, "locked GitHub fallback skips downloads when cached");
    expect(
      contains(output.str(), "Using locked dependency answer for any"),
      "missing local dependency reports the selected locked fallback"
    );
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

  void test_build_validates_locked_github_variant_identity()
  {
    TemporaryDirectory directory;
    write_project(directory.path());
    std::ofstream recipe { directory.path() / "forge.recipe.toml", std::ios::app };
    recipe
      << "\n[dependencies]\n"
      << "answer = { github = \"example/answer\", version = \"1.2.3\", "
         "variant = \"applaudio\" }\n";
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
      << "github = \"example/answer\"\n"
      << "variant = \"openal\"\n"
      << "version = \"1.2.3\"\n"
      << "target = \"" << target << "\"\n"
      << "url = \"https://example.invalid/answer.cbox\"\n"
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
      "build rejects a lock selecting a different GitHub cbox variant"
    );
    expect(invocations == 0, "GitHub variant lock conflict does not access the network");
    expect(
      contains(error.str(), "run forge update answer"),
      "GitHub variant lock conflict explains how to resolve it"
    );
  }

  void test_build_uses_locked_github_dependency_variant()
  {
    TemporaryDirectory directory;
    write_project(directory.path());
    std::ofstream recipe { directory.path() / "forge.recipe.toml", std::ios::app };
    recipe
      << "\n[dependencies]\n"
      << "answer = { github = \"example/answer\", version = \"1.2.3\", "
         "variant = \"applaudio\" }\n";
    recipe.close();

    const std::string checksum =
      "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855";
    std::ofstream lock { directory.path() / "forge.lock.toml" };
    lock
      << "format = 2\n\n"
      << "[[dependency]]\n"
      << "name = \"answer\"\n"
      << "github = \"example/answer\"\n"
      << "variant = \"applaudio\"\n"
      << "version = \"1.2.3\"\n"
      << "target = \"any\"\n"
      << "url = \"https://example.invalid/answer-applaudio.cbox\"\n"
      << "sha256 = \"" << checksum << "\"\n";
    lock.close();
    std::filesystem::create_directories(directory.path() / ".forge/cache/downloads");
    std::ofstream { directory.path() / ".forge/cache/downloads" / (checksum + ".cbox") };
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
      "locked GitHub variant dependency reaches box validation"
    );
    expect(invocations == 0, "locked GitHub variant dependency skips downloads");
    expect(
      contains(output.str(), "Using locked dependency answer variant applaudio for any"),
      "build reports the selected locked cbox variant"
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
  test_recipe_accepts_toml_comments_multiline_arrays_and_escapes();
  test_build_generates_cmake_and_commands();
  test_build_generates_c_cmake();
  test_build_generates_static_library();
  test_build_generates_dynamic_library();
  test_build_accepts_legacy_shared_library_alias();
  test_build_validates_header_only_project();
  test_build_stops_after_configuration_failure();
  test_build_generates_recipe_and_cli_definitions();
  test_build_generates_named_target_definitions();
  test_build_rejects_invalid_recipe_definition();
  test_build_applies_build_profile();
  test_build_applies_selector_rules();
  test_effective_build_selection_centralizes_legacy_and_selector_resolution();
  test_build_applies_default_selector_profile();
  test_build_rejects_selector_that_omits_required_dependency();
  test_dependency_style_validates_rule_shape();
  test_selector_recipe_keeps_legacy_workflow_profile();
  test_build_generates_system_package_hint_diagnostics();
  test_local_dependency_replaces_selected_system_provider();
  test_build_uses_ancestor_vcpkg_manifest_paths();
  test_build_generates_named_target_system_package_hint_diagnostics();
  test_build_skips_dependencies_filtered_to_other_targets();
  test_build_selects_named_target();
  test_build_requires_target_for_multi_target_recipe();
  test_build_rejects_missing_internal_target();
  test_build_rejects_internal_target_cycle();
  test_build_stages_runtime_assets();
  test_build_rejects_unsafe_runtime_asset_manifest();
  test_build_rejects_runtime_asset_collision();
  test_build_stages_dependency_runtime_assets();
  test_build_rejects_missing_source_without_running_process();
  test_build_rejects_source_with_embedded_parent_path();
  test_build_rejects_dependency_name_mismatch();
  test_build_accepts_named_target_dependency();
  test_build_rejects_dependency_cycle();
  test_update_resolves_github_dependency();
  test_named_update_skips_other_unlocked_github_dependencies();
  test_update_resolves_github_dependency_for_requested_target();
  test_update_resolves_github_dependency_variant();
  test_update_resolves_github_component_dependency();
  test_build_requires_and_uses_locked_github_dependency();
  test_build_uses_local_dependency_before_github_fallback();
  test_build_validates_locked_github_component_identity();
  test_build_validates_locked_github_variant_identity();
  test_build_uses_locked_github_dependency_variant();
  test_build_rejects_incomplete_github_dependency();

  return failures == 0 ? 0 : 1;
}
