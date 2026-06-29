#pragma once

#include <filesystem>
#include <iosfwd>
#include <optional>
#include <string>
#include <string_view>

namespace forge::cli
{
  void print_help(std::ostream& output);
  bool print_command_help(std::string_view command, std::ostream& output);
  bool is_command(std::string_view candidate);

  std::optional<std::string_view> option_value(std::string_view argument,
                                               std::string_view prefix);

  bool set_once(std::optional<std::string>& target,
                std::string_view value);
  bool set_once(std::optional<std::filesystem::path>& target,
                std::string_view value);

  bool read_required_option(std::string_view value,
                            std::string_view empty_message,
                            std::ostream& error);
  bool read_profile_option(std::string_view argument,
                           std::optional<std::string>& profile,
                           std::ostream& error);

  void print_new_usage(std::ostream& error);
  void print_adopt_usage(std::ostream& error);
  void print_update_usage(std::ostream& error);
  void print_upgrade_usage(std::ostream& error);
  void print_build_usage(std::ostream& error);
  void print_workflow_feature_usage(std::string_view operation,
                                    std::ostream& error);
  void print_prepare_release_usage(std::ostream& error);
  void print_release_git_usage(std::ostream& error);
}
