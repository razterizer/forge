#include "cli.h"
#include "process.h"

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

  void test_init_discovers_existing_sources()
  {
    TemporaryDirectory directory;
    constexpr std::array arguments { std::string_view { "init" } };
    std::ostringstream output;
    std::ostringstream error;

    write_file(directory.path() / "main.cpp", "int main() {}\n");
    write_file(directory.path() / "source/game.cc", "");
    write_file(directory.path() / "source/render.cxx", "");
    write_file(directory.path() / "source/readme.txt", "");

    expect(
      forge::cli::run(arguments, directory.path(), output, error) == 0,
      "init succeeds"
    );
    expect(
      std::filesystem::exists(directory.path() / "forge.recipe.toml"),
      "init creates a recipe"
    );
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
      "init recipe declares its schema"
    );
    expect(
      std::filesystem::exists(directory.path() / ".github/workflows/release-linux.yml"),
      "init creates GitHub release workflows"
    );
    expect(
      std::filesystem::exists(directory.path() / "RELEASE_NOTES.md"),
      "init creates release notes"
    );
    expect(contains(output.str(), "Found 3 C++ source files"), "init reports discovered sources");
    expect(error.str().empty(), "init does not write an error");
  }

  void test_init_ignores_generated_directories()
  {
    TemporaryDirectory directory;
    constexpr std::array arguments { std::string_view { "init" } };
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

    expect(contains(recipe, "app.cpp"), "init discovers project sources");
    expect(!contains(recipe, "generated.cpp"), "init ignores generated directories");
  }

  void test_init_empty_project()
  {
    TemporaryDirectory directory;
    constexpr std::array arguments { std::string_view { "init" } };
    std::ostringstream output;
    std::ostringstream error;

    expect(
      forge::cli::run(arguments, directory.path(), output, error) == 0,
      "init accepts an empty project"
    );
    expect(
      contains(read_file(directory.path() / "forge.recipe.toml"), "paths = []"),
      "empty project has an empty source list"
    );
    expect(!std::filesystem::exists(directory.path() / "src"), "init does not create source files");
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

  void test_init_preserves_existing_release_support()
  {
    TemporaryDirectory directory;
    constexpr std::array arguments { std::string_view { "init" } };
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
      "init preserves existing release notes"
    );
    expect(
      read_file(directory.path() / ".github/workflows/release-linux.yml")
        == "name: custom release\n",
      "init preserves existing GitHub workflows"
    );
    expect(
      std::filesystem::exists(directory.path() / ".github/workflows/release-windows.yml"),
      "init creates missing GitHub workflows"
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
      contains(read_file(directory.path() / "hello/.github/workflows/release-windows.yml"), "hello-*.zip"),
      "new workflows use the project name"
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
    constexpr std::array init_arguments { std::string_view { "init" } };
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

    const std::array inspect_arguments {
      std::string_view { "box" },
      std::string_view { "inspect" },
      std::string_view { box_path_string }
    };
    std::ostringstream inspect_output;
    std::ostringstream inspect_error;
    expect(
      forge::cli::run(inspect_arguments, project_directory, inspect_output, inspect_error) == 0,
      "box inspect succeeds"
    );
    expect(contains(inspect_output.str(), "format = 1"), "box inspect prints the manifest");
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
    expect(contains(inspect_output.str(), "type = \"header_only\""), "manifest identifies header-only package");
    expect(contains(inspect_output.str(), "kind = \"public_header\""), "manifest declares a public header");
    expect(!contains(inspect_output.str(), "static_library"), "header-only box does not declare a library");

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
  }

  void test_run_and_release_with_shared_dependency()
  {
#ifndef _WIN32
    TemporaryDirectory directory;
    const auto answer = directory.path() / "answer";
    const auto shared_library = directory.path() / "greeting";
    const auto application = directory.path() / "app";
    write_file(
      answer / "forge.recipe.toml",
      "[project]\n"
      "name = \"answer\"\n"
      "version = \"1.0.0\"\n"
      "type = \"shared_library\"\n"
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
      shared_library / "forge.recipe.toml",
      "[project]\n"
      "name = \"greeting\"\n"
      "version = \"1.0.0\"\n"
      "type = \"shared_library\"\n"
      "cpp_std = 20\n\n"
      "[sources]\n"
      "paths = [\"src/greeting.cpp\"]\n"
      "public_headers = [\"include/greeting/greeting.h\"]\n\n"
      "[dependencies]\n"
      "answer = { path = \"../answer\" }\n"
    );
    write_file(shared_library / "include/greeting/greeting.h", "int greeting();\n");
    write_file(
      shared_library / "src/greeting.cpp",
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
      "run succeeds with a shared-library dependency"
    );
#ifdef __APPLE__
    constexpr std::string_view runtime_filename = "libgreeting.dylib";
    constexpr std::string_view transitive_runtime_filename = "libanswer.dylib";
#else
    constexpr std::string_view runtime_filename = "libgreeting.so";
    constexpr std::string_view transitive_runtime_filename = "libanswer.so";
#endif
    expect(
      std::filesystem::exists(application / ".forge/build/runtime" / runtime_filename),
      "build stages the shared-library runtime"
    );
    expect(
      std::filesystem::exists(
        application / ".forge/build/runtime" / transitive_runtime_filename
      ),
      "build stages the transitive shared-library runtime"
    );
    expect(
      std::filesystem::exists(application / ".forge/deps/greeting/runtime" / runtime_filename),
      "build installs the shared-library box"
    );
    constexpr std::array release_arguments { std::string_view { "release" } };
    std::ostringstream release_output;
    std::ostringstream release_error;

    expect(
      forge::cli::run(release_arguments, application, release_output, release_error) == 0,
      "release succeeds with a shared-library dependency"
    );
    expect(
      std::filesystem::exists(
        application / ".forge/release/app-1.0.0/runtime" / runtime_filename
      ),
      "release stages the shared-library runtime"
    );
    expect(
      std::filesystem::exists(
        application / ".forge/release/app-1.0.0/runtime" / transitive_runtime_filename
      ),
      "release stages the transitive shared-library runtime"
    );
    const auto released_executable = application / ".forge/release/app-1.0.0/app";
    const std::vector<std::string> released_arguments { released_executable.string() };
    std::ostringstream released_error;
    expect(
      forge::run_process(
        released_arguments,
        released_executable.parent_path(),
        released_error
      ) == 0,
      "released executable loads its shared-library runtime"
    );
    expect(run_error.str().empty(), "shared-library run does not write an error");
    expect(release_error.str().empty(), "shared-library release does not write an error");
    expect(released_error.str().empty(), "released executable does not write an error");
#endif
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
  test_init_discovers_existing_sources();
  test_init_ignores_generated_directories();
  test_init_empty_project();
  test_init_refuses_to_overwrite();
  test_init_preserves_existing_release_support();
  test_new();
  test_new_refuses_existing_path();
  test_new_requires_simple_name();
  test_new_requires_name();
  test_build_new_project();
  test_build_rejects_empty_project();
  test_run_new_project();
  test_release_new_project();
  test_release_rejects_empty_tag_format();
  test_release_github_rejects_empty_tag_format();
  test_box_round_trip();
  test_static_library_box_round_trip();
  test_header_only_box_round_trip();
  test_run_with_local_dependencies();
  test_run_and_release_with_shared_dependency();
  test_unknown_command();

  return failures == 0 ? 0 : 1;
}
