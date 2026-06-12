#include "cli.h"
#include "sha256.h"

#include <array>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <string_view>

namespace
{

  class TemporaryDirectory
  {
  public:
    TemporaryDirectory()
    {
      const auto suffix = std::chrono::steady_clock::now().time_since_epoch().count();
      path_ = std::filesystem::temp_directory_path()
        / ("forge-aggregate-box-e2e-" + std::to_string(suffix));
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

  void write_file(const std::filesystem::path& path, std::string_view contents)
  {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream file { path };
    file << contents;
  }

  std::string current_target()
  {
#ifdef _WIN32
    const std::string os = "windows";
#elif __APPLE__
    const std::string os = "macos";
#else
    const std::string os = "linux";
#endif

#if defined(__aarch64__) || defined(_M_ARM64)
    return os + "-arm64";
#elif defined(__x86_64__) || defined(_M_X64)
    return os + "-x86_64";
#elif defined(__i386__) || defined(_M_IX86)
    return os + "-x86";
#else
    return os + "-unknown";
#endif
  }

  bool run(const std::filesystem::path& directory,
           std::span<const std::string_view> arguments,
           std::string_view description)
  {
    std::ostringstream output;
    std::ostringstream error;
    const auto result = forge::cli::run(arguments, directory, output, error);

    if (result == 0)
    {
      return true;
    }

    std::cerr << "FAIL: " << description << " (exit " << result << ")\n"
              << output.str() << error.str();
    return false;
  }

  bool has_nested_metadata_cache(const std::filesystem::path& project)
  {
    const auto root = project / ".forge/cache/box-metadata";
    std::error_code error;

    if (!std::filesystem::is_directory(root))
    {
      return false;
    }

    for (const auto& entry : std::filesystem::recursive_directory_iterator { root, error })
    {
      if (error)
      {
        return true;
      }

      if (entry.is_directory() && entry.path().filename() == ".forge")
      {
        return true;
      }
    }

    return false;
  }

  void write_producer(const std::filesystem::path& directory)
  {
    write_file(
      directory / "forge.recipe.toml",
      "[project]\n"
      "name = \"platform-suite\"\n"
      "version = \"0.7.0\"\n"
      "\n"
      "[target.headers]\n"
      "type = \"header_only\"\n"
      "cpp_std = 20\n"
      "sources = []\n"
      "public_headers = [\"include/platform_suite/headers.h\"]\n"
      "\n"
      "[target.static_math]\n"
      "type = \"static_library\"\n"
      "cpp_std = 20\n"
      "sources = [\"src/static_math.cpp\"]\n"
      "public_headers = [\"include/platform_suite/static_math.h\"]\n"
      "\n"
      "[target.dynamic_math]\n"
      "type = \"dynamic_library\"\n"
      "cpp_std = 20\n"
      "sources = [\"src/dynamic_math.cpp\"]\n"
      "public_headers = [\"include/platform_suite/dynamic_math.h\"]\n"
    );
    write_file(
      directory / "include/platform_suite/headers.h",
      "#pragma once\ninline int header_value() { return 11; }\n"
    );
    write_file(
      directory / "include/platform_suite/static_math.h",
      "#pragma once\nint static_value();\n"
    );
    write_file(
      directory / "include/platform_suite/dynamic_math.h",
      "#pragma once\nint dynamic_value();\n"
    );
    write_file(
      directory / "src/static_math.cpp",
      "#include <platform_suite/static_math.h>\nint static_value() { return 22; }\n"
    );
    write_file(
      directory / "src/dynamic_math.cpp",
      "#include <platform_suite/dynamic_math.h>\nint dynamic_value() { return 33; }\n"
    );
  }

  void write_consumer(const std::filesystem::path& directory, std::string_view dependencies)
  {
    write_file(
      directory / "forge.recipe.toml",
      std::string {
        "[project]\n"
        "name = \"aggregate-consumer\"\n"
        "version = \"0.7.0\"\n"
        "type = \"executable\"\n"
        "cpp_std = 20\n"
        "\n"
        "[sources]\n"
        "paths = [\"main.cpp\"]\n"
        "\n"
        "[dependencies]\n"
      } + std::string { dependencies }
    );
    write_file(
      directory / "main.cpp",
      "#include <platform_suite/headers.h>\n"
      "#include <platform_suite/static_math.h>\n"
      "#include <platform_suite/dynamic_math.h>\n"
      "int main() {\n"
      "  return header_value() + static_value() + dynamic_value() == 66 ? 0 : 1;\n"
      "}\n"
    );
  }

} // namespace

int main()
{
  TemporaryDirectory workspace;
  const auto producer = workspace.path() / "producer";
  write_producer(producer);

  constexpr std::array create_box { std::string_view { "box" }, std::string_view { "create" } };

  if (!run(producer, create_box, "create aggregate format-3 box"))
  {
    return 1;
  }

  if (has_nested_metadata_cache(producer))
  {
    std::cerr << "FAIL: aggregate validation recursively nested the metadata cache\n";
    return 1;
  }

  const auto box_name = "platform-suite-0.7.0-" + current_target() + ".cbox";
  const auto aggregate_box = producer / ".forge/boxes" / box_name;

  if (!std::filesystem::is_regular_file(aggregate_box))
  {
    std::cerr << "FAIL: aggregate box was not created at " << aggregate_box << '\n';
    return 1;
  }

  const auto packages = workspace.path() / "packages";
  std::filesystem::create_directories(packages);
  const auto local_box = packages / box_name;
  std::filesystem::copy_file(aggregate_box, local_box);
  const auto local_consumer = workspace.path() / "local-consumer";
  const auto relative_box = "../packages/" + box_name;
  write_consumer(
    local_consumer,
    "headers = { box = \"" + relative_box + "\", component = \"headers\" }\n"
    "static_math = { box = \"" + relative_box + "\", component = \"static_math\" }\n"
    "dynamic_math = { box = \"" + relative_box + "\", component = \"dynamic_math\" }\n"
  );

  constexpr std::array run_command { std::string_view { "run" } };

  if (!run(local_consumer, run_command, "consume local aggregate components"))
  {
    return 1;
  }

  std::string checksum;
  std::ostringstream checksum_error;

  if (!forge::sha256_file(aggregate_box, checksum, checksum_error))
  {
    std::cerr << "FAIL: checksum aggregate box\n" << checksum_error.str();
    return 1;
  }

  const auto github_consumer = workspace.path() / "github-consumer";
  write_consumer(
    github_consumer,
    "headers = { github = \"example/platform-suite\", package = \"platform-suite\", "
    "component = \"headers\", version = \"0.7.0\" }\n"
    "static_math = { github = \"example/platform-suite\", package = \"platform-suite\", "
    "component = \"static_math\", version = \"0.7.0\" }\n"
    "dynamic_math = { github = \"example/platform-suite\", package = \"platform-suite\", "
    "component = \"dynamic_math\", version = \"0.7.0\" }\n"
  );
  std::filesystem::create_directories(github_consumer / ".forge/cache/downloads");
  std::filesystem::copy_file(
    aggregate_box,
    github_consumer / ".forge/cache/downloads" / (checksum + ".cbox")
  );

  const auto url =
    "https://github.com/example/platform-suite/releases/download/release-0.7.0/" + box_name;
  std::ostringstream lock;
  lock << "format = 2\n";

  for (const auto component : { "headers", "static_math", "dynamic_math" })
  {
    lock
      << "\n[[dependency]]\n"
      << "name = \"" << component << "\"\n"
      << "github = \"example/platform-suite\"\n"
      << "package = \"platform-suite\"\n"
      << "component = \"" << component << "\"\n"
      << "version = \"0.7.0\"\n"
      << "target = \"" << current_target() << "\"\n"
      << "url = \"" << url << "\"\n"
      << "sha256 = \"" << checksum << "\"\n";
  }

  write_file(github_consumer / "forge.lock.toml", lock.str());

  if (!run(github_consumer, run_command, "consume locked GitHub aggregate components"))
  {
    return 1;
  }

  std::cout << "Verified aggregate format-3 publication and component consumption for "
            << current_target() << '\n';
  return 0;
}
