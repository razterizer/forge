#include "box.h"
#include "fprocess.h"
#include "sha256.h"
#include "test_support.h"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <optional>
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

  void write_toolchain(const std::filesystem::path& directory)
  {
    std::ofstream { directory / ".forge/build/forge-toolchain.toml" }
      << "compiler = \"ExampleCompiler\"\n"
      << "compiler_version = \"1.0\"\n"
      << "cpp_std = 20\n"
      << "configuration = \"Debug\"\n"
      << "runtime = \"default\"\n";
  }

  std::filesystem::path write_test_box(const std::filesystem::path& directory)
  {
    const auto staging = directory / "archive-staging";
    const auto box = directory / "hello.cbox";
    std::filesystem::create_directories(staging / "bin");
    std::ofstream { staging / "cbox.toml" };
    std::ofstream { staging / "bin/hello" };
    const std::vector<std::string> arguments {
      "cmake",
      "-E",
      "tar",
      "cf",
      box.string(),
      "--format=zip",
      "cbox.toml",
      "bin"
    };
    std::ostringstream error;
    forge::run_process(arguments, staging, error);
    return box;
  }

  void write_project(const std::filesystem::path& directory,
                     bool include_build_number = false)
  {
    std::ofstream recipe { directory / "forge.recipe.toml" };
    recipe
      << "[project]\n"
      << "name = \"hello\"\n"
      << "version = \"0.1.0\"\n"
      << "type = \"executable\"\n"
      << "cpp_std = 20\n";

    if (include_build_number)
    {
      recipe
        << "\n[build]\n"
        << "number = 6\n";
    }

    recipe
      << "\n"
      << "[sources]\n"
      << "paths = [\"main.cpp\"]\n";

    std::ofstream source { directory / "main.cpp" };
    source << "int main() {}\n";
  }

  void test_create_box_stages_manifest_and_executable()
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
    std::vector<std::vector<std::string>> commands;
    std::ostringstream output;
    std::ostringstream error;

    const forge::ProcessRunner runner =
      [&commands, &directory](const std::vector<std::string>& command,
                             const std::filesystem::path&,
                             std::ostream&)
      {
        commands.push_back(command);

        if (command.size() > 1 && command[1] == "--build")
        {
          std::filesystem::create_directories(directory.path() / ".forge/build");
          write_toolchain(directory.path());
#ifdef _WIN32
          std::ofstream executable { directory.path() / ".forge/build/hello.exe" };
#else
          std::ofstream executable { directory.path() / ".forge/build/hello" };
#endif
        }

        return 0;
      };

    expect(
      forge::create_box(directory.path(), runner, output, error) == 0,
      "box create succeeds when build and archive commands succeed"
    );
    expect(commands.size() == 3, "box create configures, builds, and archives");
    expect(commands[2].size() > 6 && commands[2][2] == "tar", "box create uses CMake archive support");

    const auto staging_root = directory.path() / ".forge/boxes/staging";
    std::filesystem::path manifest;

    for (const auto& entry : std::filesystem::directory_iterator { staging_root })
      manifest = entry.path() / "cbox.toml";

    expect(std::filesystem::exists(manifest), "box create stages a manifest");
    expect(!contains(read_file(manifest), "build ="), "box manifest omits an unspecified build number");
    expect(contains(read_file(manifest), "sha256 = \""), "box manifest includes an artifact checksum");
    expect(
      contains(read_file(manifest), "path = \"runtime-assets/assets/message.txt\""),
      "box manifest declares runtime assets"
    );
    expect(
      std::filesystem::exists(manifest.parent_path() / "runtime-assets/assets/message.txt"),
      "box stages runtime assets"
    );
    expect(
      std::filesystem::exists(manifest.parent_path() / "runtime-assets/Blocks.txt"),
      "box stages mapped runtime assets at their requested destination"
    );
    expect(contains(output.str(), "Created"), "box create reports its archive");
    expect(error.str().empty(), "successful box create does not write an error");
  }

  void test_create_box_includes_build_number()
  {
    TemporaryDirectory directory;
    write_project(directory.path(), true);
    std::vector<std::vector<std::string>> commands;
    std::ostringstream output;
    std::ostringstream error;

    const forge::ProcessRunner runner =
      [&commands, &directory](const std::vector<std::string>& command,
                             const std::filesystem::path&,
                             std::ostream&)
      {
        commands.push_back(command);

        if (command.size() > 1 && command[1] == "--build")
        {
          std::filesystem::create_directories(directory.path() / ".forge/build");
          write_toolchain(directory.path());
#ifdef _WIN32
          std::ofstream executable { directory.path() / ".forge/build/hello.exe" };
#else
          std::ofstream executable { directory.path() / ".forge/build/hello" };
#endif
        }

        return 0;
      };

    expect(
      forge::create_box(directory.path(), runner, output, error) == 0,
      "box create succeeds with a build number"
    );
    expect(
      commands.size() == 3 && contains(commands[2][4], "hello-0.1.0+build.6-"),
      "box archive name includes build metadata"
    );

    std::filesystem::path manifest;

    for (const auto& entry : std::filesystem::directory_iterator {
      directory.path() / ".forge/boxes/staging"
    })
    {
      manifest = entry.path() / "cbox.toml";
    }

    expect(contains(read_file(manifest), "build = 6"), "box manifest includes the build number");
  }

  void test_create_box_variant_uses_profile_suffix()
  {
    TemporaryDirectory directory;
    write_project(directory.path());
    std::ofstream recipe { directory.path() / "forge.recipe.toml", std::ios::app };
    recipe
      << "\n[box]\n"
      << "variants = [{ profile = \"applaudio-release\", suffix = \"applaudio\" }]\n"
      << "\n[profile.applaudio-release.build]\n"
      << "configuration = \"Release\"\n"
      << "defines = [\"USE_APPLAUDIO\"]\n";
    recipe.close();
    std::vector<std::vector<std::string>> commands;
    std::ostringstream output;
    std::ostringstream error;

    const forge::ProcessRunner runner =
      [&commands, &directory](const std::vector<std::string>& command,
                              const std::filesystem::path&,
                              std::ostream&)
      {
        commands.push_back(command);

        if (command.size() > 1 && command[1] == "--build")
        {
          std::filesystem::create_directories(directory.path() / ".forge/build");
          write_toolchain(directory.path());
#ifdef _WIN32
          std::ofstream executable { directory.path() / ".forge/build/hello.exe" };
#else
          std::ofstream executable { directory.path() / ".forge/build/hello" };
#endif
        }

        return 0;
      };

    expect(
      forge::create_box(
        directory.path(),
        std::nullopt,
        std::optional<std::string> { "applaudio-release" },
        runner,
        output,
        error
      ) == 0,
      "box create succeeds with a configured cbox variant profile"
    );
    expect(
      commands.size() == 3 && contains(commands[2][4], "hello-0.1.0-applaudio-"),
      "variant box archive name includes the configured suffix"
    );
    expect(
      contains(output.str(), "Creating box hello-0.1.0-applaudio-"),
      "variant box create reports the suffixed box name"
    );
    expect(error.str().empty(), "successful variant box create does not write an error");
  }

  void test_create_header_only_box_stages_runtime_assets()
  {
    TemporaryDirectory directory;
    std::filesystem::create_directories(directory.path() / "include/hello");
    std::filesystem::create_directories(directory.path() / "data/fonts");
    std::ofstream recipe { directory.path() / "forge.recipe.toml" };
    recipe
      << "[project]\n"
      << "name = \"hello\"\n"
      << "version = \"0.1.0\"\n\n"
      << "[target.hello]\n"
      << "type = \"header_only\"\n"
      << "cpp_std = 20\n"
      << "sources = []\n"
      << "public_headers = [\"include/hello/hello.h\"]\n"
      << "runtime_files = [{ source = \"data/fonts\", destination = \"hello/fonts\" }]\n";
    recipe.close();
    std::ofstream { directory.path() / "include/hello/hello.h" } << "#pragma once\n";
    std::ofstream { directory.path() / "data/fonts/font.txt" } << "font\n";
    std::vector<std::vector<std::string>> commands;
    std::ostringstream output;
    std::ostringstream error;

    const forge::ProcessRunner runner =
      [&commands, &directory](const std::vector<std::string>& command,
                              const std::filesystem::path& working_directory,
                              std::ostream& process_error)
      {
        commands.push_back(command);

        if (command.size() > 1 && command[1] == "--build")
        {
          std::filesystem::create_directories(directory.path() / ".forge/build/hello");
          std::ofstream { directory.path() / ".forge/build/hello/forge-toolchain.toml" }
            << "compiler = \"ExampleCompiler\"\n"
            << "compiler_version = \"1.0\"\n"
            << "cpp_std = 20\n"
            << "configuration = \"Debug\"\n"
            << "runtime = \"default\"\n";
          return 0;
        }

        if (command.size() > 2 && command[1] == "-E" && command[2] == "tar")
          return forge::run_process(command, working_directory, process_error);

        return 0;
      };

    expect(
      forge::create_box(directory.path(), std::optional<std::string> { "hello" }, runner, output, error) == 0,
      "header-only box create succeeds with runtime assets"
    );

    const auto staging_root = directory.path() / ".forge/boxes/staging";
    std::filesystem::path manifest;

    for (const auto& entry : std::filesystem::directory_iterator { staging_root })
      manifest = entry.path() / "cbox.toml";

    expect(
      contains(read_file(manifest), "path = \"runtime-assets/hello/fonts/font.txt\""),
      "header-only box manifest declares exported runtime assets"
    );
    expect(
      std::filesystem::exists(manifest.parent_path() / "runtime-assets/hello/fonts/font.txt"),
      "header-only box stages exported runtime assets"
    );
    expect(error.str().empty(), "header-only box with runtime assets does not write an error");
  }

#ifdef _WIN32
  void test_create_windows_dynamic_library_box()
  {
    TemporaryDirectory directory;
    std::filesystem::create_directories(directory.path() / "src");
    std::filesystem::create_directories(directory.path() / "include/hello");
    std::ofstream { directory.path() / "forge.recipe.toml" }
      << "[project]\n"
      << "name = \"hello\"\n"
      << "version = \"0.1.0\"\n"
      << "type = \"dynamic_library\"\n"
      << "cpp_std = 20\n\n"
      << "[sources]\n"
      << "paths = [\"src/hello.cpp\"]\n"
      << "public_headers = [\"include/hello/hello.h\"]\n";
    std::ofstream { directory.path() / "src/hello.cpp" } << "int hello() { return 42; }\n";
    std::ofstream { directory.path() / "include/hello/hello.h" } << "int hello();\n";
    std::ostringstream output;
    std::ostringstream error;

    const forge::ProcessRunner runner =
      [&directory](const std::vector<std::string>& command,
                   const std::filesystem::path&,
                   std::ostream&)
      {
        if (command.size() > 1 && command[1] == "--build")
        {
          std::filesystem::create_directories(directory.path() / ".forge/build");
          write_toolchain(directory.path());
          std::ofstream { directory.path() / ".forge/build/hello.dll" };
          std::ofstream { directory.path() / ".forge/build/hello.lib" };
        }

        return 0;
      };

    expect(
      forge::create_box(directory.path(), runner, output, error) == 0,
      "Windows dynamic-library box creation succeeds"
    );

    std::filesystem::path staging;

    for (const auto& entry : std::filesystem::directory_iterator {
      directory.path() / ".forge/boxes/staging"
    })
    {
      staging = entry.path();
    }

    const auto manifest = read_file(staging / "cbox.toml");
    expect(contains(manifest, "path = \"runtime/hello.dll\""), "box declares the Windows DLL");
    expect(contains(manifest, "kind = \"dynamic_library\""), "box identifies the Windows DLL");
    expect(contains(manifest, "path = \"lib/hello.lib\""), "box declares the import library");
    expect(contains(manifest, "kind = \"import_library\""), "box identifies the import library");
    expect(std::filesystem::exists(staging / "runtime/hello.dll"), "box stages the Windows DLL");
    expect(std::filesystem::exists(staging / "lib/hello.lib"), "box stages the import library");
  }
#endif

  void test_create_imported_library_box_without_building()
  {
    TemporaryDirectory directory;
    std::filesystem::create_directories(directory.path() / "vendor/include/vendor");
    std::filesystem::create_directories(directory.path() / "vendor/lib");
    std::filesystem::create_directories(directory.path() / "vendor/runtime");
    std::ofstream { directory.path() / "vendor/include/vendor/sdk.h" } << "int sdk();\n";
    std::ofstream { directory.path() / "vendor/lib/sdk.a" };
    std::ofstream { directory.path() / "vendor/runtime/sdk.so" };
    std::ofstream { directory.path() / "forge.recipe.toml" }
      << "[project]\n"
      << "name = \"vendor-sdk\"\n"
      << "version = \"4.2.0\"\n"
      << "type = \"imported_library\"\n\n"
      << "[build]\n"
      << "macos_libraries = [\"openal\"]\n"
      << "macos_brew_packages = [\"openal-soft\"]\n"
      << "linux_libraries = [\"openal\"]\n"
      << "linux_apt_packages = [\"libopenal-dev\"]\n\n"
      << "[import." << current_target() << "]\n"
      << "compiler = \"ExampleCompiler\"\n"
      << "compiler_version = \"1.0\"\n"
      << "cpp_std = 20\n"
      << "configuration = \"Debug\"\n"
      << "runtime = \"default\"\n"
      << "public_headers = [\"vendor/include\"]\n"
      << "static_libraries = [\"vendor/lib/sdk.a\"]\n"
      << "dynamic_libraries = [\"vendor/runtime/sdk.so\"]\n";
    std::vector<std::vector<std::string>> commands;
    std::ostringstream output;
    std::ostringstream error;

    const forge::ProcessRunner runner =
      [&commands](const std::vector<std::string>& command,
                  const std::filesystem::path&,
                  std::ostream&)
      {
        commands.push_back(command);
        return 0;
      };

    expect(
      forge::create_box(directory.path(), runner, output, error) == 0,
      "imported-library box creation succeeds"
    );
    expect(commands.size() == 1, "imported-library box creation skips configure and build");

    std::filesystem::path staging;

    for (const auto& entry : std::filesystem::directory_iterator {
      directory.path() / ".forge/boxes/staging"
    })
    {
      staging = entry.path();
    }

    const auto manifest = read_file(staging / "cbox.toml");
    expect(contains(manifest, "type = \"imported_library\""), "manifest identifies an imported library");
    expect(contains(manifest, "path = \"include/vendor/sdk.h\""), "manifest declares imported headers");
    expect(contains(manifest, "path = \"lib/sdk.a\""), "manifest declares imported static libraries");
    expect(contains(manifest, "path = \"runtime/sdk.so\""), "manifest declares imported runtime libraries");
    expect(
      contains(manifest, "macos_brew_packages = [\"openal-soft\"]"),
      "manifest records macOS package-provider hints"
    );
    expect(
      contains(manifest, "linux_apt_packages = [\"libopenal-dev\"]"),
      "manifest records Linux package-provider hints"
    );
    expect(std::filesystem::exists(staging / "include/vendor/sdk.h"), "box stages imported headers");
    expect(std::filesystem::exists(staging / "lib/sdk.a"), "box stages imported static libraries");
    expect(std::filesystem::exists(staging / "runtime/sdk.so"), "box stages imported runtime libraries");
    expect(error.str().empty(), "imported-library box creation does not write an error");
  }

  void test_inspect_prints_manifest()
  {
    TemporaryDirectory directory;
    const auto box = write_test_box(directory.path());
    std::ostringstream output;
    std::ostringstream error;

    const forge::ProcessRunner runner =
      [](const std::vector<std::string>&,
         const std::filesystem::path& working_directory,
         std::ostream&)
      {
        std::filesystem::create_directories(working_directory / "bin");
        std::ofstream artifact { working_directory / "bin/hello" };
        artifact.close();
        std::ofstream manifest { working_directory / "cbox.toml" };
        manifest
          << "[cbox]\n"
          << "format = 1\n\n"
          << "[package]\n"
          << "name = \"hello\"\n"
          << "version = \"0.1.0\"\n"
          << "type = \"executable\"\n\n"
          << "[target]\n"
          << "os = \"test\"\n"
          << "arch = \"test\"\n\n"
          << "[artifact]\n"
          << "path = \"bin/hello\"\n"
          << "kind = \"executable\"\n"
          << "sha256 = \"e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855\"\n";
        return 0;
      };

    expect(
      forge::inspect_box(box, directory.path(), runner, output, error) == 0,
      "box inspect succeeds"
    );
    expect(
      contains(output.str(), "Package: hello 0.1.0")
      && contains(output.str(), "Target: test-test")
      && contains(output.str(), "Type: executable")
      && contains(output.str(), "Manifest:")
      && contains(output.str(), "format = 1"),
      "box inspect prints a summary and the manifest"
    );
  }

  void test_verify_rejects_checksum_mismatch()
  {
    TemporaryDirectory directory;
    const auto box = write_test_box(directory.path());
    std::ostringstream output;
    std::ostringstream error;

    const forge::ProcessRunner runner =
      [](const std::vector<std::string>&,
         const std::filesystem::path& working_directory,
         std::ostream&)
      {
        std::filesystem::create_directories(working_directory / "bin");
        std::ofstream { working_directory / "bin/hello" } << "changed";
        std::ofstream manifest { working_directory / "cbox.toml" };
        manifest
          << "[cbox]\nformat = 1\n"
          << "[package]\nname = \"hello\"\nversion = \"0.1.0\"\ntype = \"executable\"\n"
          << "[target]\nos = \"test\"\narch = \"test\"\n"
          << "[artifact]\npath = \"bin/hello\"\nkind = \"executable\"\n"
          << "sha256 = \"e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855\"\n";
        return 0;
      };

    expect(
      forge::verify_box(box, directory.path(), runner, output, error) == 2,
      "box verify rejects a checksum mismatch"
    );
    expect(contains(error.str(), "checksum"), "checksum mismatch has a useful error");
  }

  void test_verify_rejects_unexpected_file()
  {
    TemporaryDirectory directory;
    const auto box = write_test_box(directory.path());
    std::ostringstream output;
    std::ostringstream error;

    const forge::ProcessRunner runner =
      [](const std::vector<std::string>&,
         const std::filesystem::path& working_directory,
         std::ostream&)
      {
        std::filesystem::create_directories(working_directory / "bin");
        std::ofstream { working_directory / "bin/hello" };
        std::ofstream { working_directory / "surprise.txt" };
        std::ofstream manifest { working_directory / "cbox.toml" };
        manifest
          << "[cbox]\nformat = 1\n"
          << "[package]\nname = \"hello\"\nversion = \"0.1.0\"\ntype = \"executable\"\n"
          << "[target]\nos = \"test\"\narch = \"test\"\n"
          << "[artifact]\npath = \"bin/hello\"\nkind = \"executable\"\n"
          << "sha256 = \"e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855\"\n";
        return 0;
      };

    expect(
      forge::verify_box(box, directory.path(), runner, output, error) == 2,
      "box verify rejects an unexpected file"
    );
    expect(contains(error.str(), "unexpected file"), "unexpected file has a useful error");
  }

  void test_verify_rejects_unsupported_format()
  {
    TemporaryDirectory directory;
    const auto box = write_test_box(directory.path());
    std::ostringstream output;
    std::ostringstream error;

    const forge::ProcessRunner runner =
      [](const std::vector<std::string>&,
         const std::filesystem::path& working_directory,
         std::ostream&)
      {
        std::ofstream manifest { working_directory / "cbox.toml" };
        manifest
          << "[cbox]\nformat = 4\n"
          << "[package]\nname = \"hello\"\nversion = \"0.1.0\"\ntype = \"executable\"\n"
          << "[target]\nos = \"test\"\narch = \"test\"\n"
          << "[artifact]\npath = \"bin/hello\"\nkind = \"executable\"\n"
          << "sha256 = \"e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855\"\n";
        return 0;
      };

    expect(
      forge::verify_box(box, directory.path(), runner, output, error) == 2,
      "box verify rejects an unsupported format"
    );
    expect(contains(error.str(), "unsupported box format"), "unsupported format has a useful error");
  }

  void test_verify_rejects_unsafe_artifact_path()
  {
    TemporaryDirectory directory;
    const auto box = write_test_box(directory.path());
    std::ostringstream output;
    std::ostringstream error;

    const forge::ProcessRunner runner =
      [](const std::vector<std::string>&,
         const std::filesystem::path& working_directory,
         std::ostream&)
      {
        std::ofstream manifest { working_directory / "cbox.toml" };
        manifest
          << "[cbox]\nformat = 1\n"
          << "[package]\nname = \"hello\"\nversion = \"0.1.0\"\ntype = \"executable\"\n"
          << "[target]\nos = \"test\"\narch = \"test\"\n"
          << "[artifact]\npath = \"../hello\"\nkind = \"executable\"\n"
          << "sha256 = \"e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855\"\n";
        return 0;
      };

    expect(
      forge::verify_box(box, directory.path(), runner, output, error) == 2,
      "box verify rejects an unsafe artifact path"
    );
    expect(contains(error.str(), "invalid package or artifact"), "unsafe path has a useful error");
  }

  void test_verify_rejects_unsafe_archive_entry()
  {
    TemporaryDirectory directory;
    const auto staging = directory.path() / "archive-staging";
    const auto box = directory.path() / "unsafe.cbox";
    std::filesystem::create_directories(staging / "bin");
    std::ofstream { staging / "cbox.toml" };
    std::ofstream { staging / "bin/hello" };
    std::ofstream { directory.path() / "escape.txt" };
    const std::vector<std::string> arguments {
      "cmake",
      "-E",
      "tar",
      "cf",
      box.string(),
      "--format=zip",
      "cbox.toml",
      "bin",
      "../escape.txt"
    };
    std::ostringstream archive_error;
    forge::run_process(arguments, staging, archive_error);
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
      forge::verify_box(box, directory.path(), runner, output, error) == 2,
      "box verify rejects an unsafe archive entry"
    );
    expect(invocations == 0, "unsafe archive entry is rejected before extraction");
    expect(contains(error.str(), "unsafe archive entry"), "unsafe archive entry has a useful error");
  }

  void test_sha256_known_value()
  {
    TemporaryDirectory directory;
    const auto path = directory.path() / "value.txt";
    std::ofstream { path } << "abc";
    std::string checksum;
    std::ostringstream error;

    expect(forge::sha256_file(path, checksum, error), "SHA-256 reads a file");
    expect(
      checksum == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
      "SHA-256 matches its known value"
    );
  }

  void test_extract_refuses_existing_destination()
  {
    TemporaryDirectory directory;
    const auto box = write_test_box(directory.path());
    std::filesystem::create_directory(directory.path() / "hello");
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
      forge::extract_box(box, directory.path(), runner, output, error) == 2,
      "box extract refuses an existing destination"
    );
    expect(invocations == 0, "box extract does not invoke tools after validation failure");
  }

} // namespace

int main()
{
  test_create_box_stages_manifest_and_executable();
  test_create_box_includes_build_number();
  test_create_box_variant_uses_profile_suffix();
  test_create_header_only_box_stages_runtime_assets();
#ifdef _WIN32
  test_create_windows_dynamic_library_box();
#endif
  test_create_imported_library_box_without_building();
  test_inspect_prints_manifest();
  test_verify_rejects_checksum_mismatch();
  test_verify_rejects_unexpected_file();
  test_verify_rejects_unsupported_format();
  test_verify_rejects_unsafe_artifact_path();
  test_verify_rejects_unsafe_archive_entry();
  test_sha256_known_value();
  test_extract_refuses_existing_destination();

  return failures == 0 ? 0 : 1;
}
