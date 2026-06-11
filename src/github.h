#pragma once

#include <filesystem>
#include <iosfwd>
#include <string_view>

namespace forge
{

  enum class GithubWorkflowFeatureOperation
  {
    add,
    update,
    remove
  };

  int list_github_workflow_features(std::ostream& output);

  int status_github_workflow_features(const std::filesystem::path& project_directory,
                                      const std::filesystem::path& workflow_file,
                                      std::ostream& output,
                                      std::ostream& error);

  int change_github_workflow_feature(const std::filesystem::path& project_directory,
                                     GithubWorkflowFeatureOperation operation,
                                     std::string_view feature,
                                     const std::filesystem::path& workflow_file,
                                     bool apply,
                                     std::ostream& output,
                                     std::ostream& error);

  bool generate_github_release_support(const std::filesystem::path& project_directory,
                                       std::ostream& error);

} // namespace forge
