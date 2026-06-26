#pragma once

#include <chrono>
#include <filesystem>
#include <string>
#include <string_view>
#include <system_error>

class TemporaryDirectory
{
public:
  explicit TemporaryDirectory(std::string_view prefix = "forge-test")
  {
    const auto suffix = std::chrono::steady_clock::now().time_since_epoch().count();
    path_ = std::filesystem::temp_directory_path()
      / (std::string { prefix } + "-" + std::to_string(suffix));
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
