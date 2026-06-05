#include "cli.h"
#include "build.h"
#include "init.h"
#include "new.h"

#include <array>
#include <filesystem>
#include <ostream>

namespace forge::cli
{
  namespace
  {

    constexpr std::array commands {
      std::string_view { "init" },
      std::string_view { "new" },
      std::string_view { "build" },
      std::string_view { "run" },
      std::string_view { "release" },
    };

    void print_help(std::ostream& output)
    {
      output
        << "Forge - a project workflow system for C++\n\n"
        << "Usage:\n"
        << "  forge <command>\n"
        << "  forge new <name>\n"
        << "  forge --help\n"
        << "  forge --version\n\n"
        << "Commands:\n"
        << "  init      Adopt the current project\n"
        << "  new       Create a new project\n"
        << "  build     Build the current project\n"
        << "  run       Run the current project\n"
        << "  release   Create a release artifact\n";
    }

    bool is_command(std::string_view candidate)
    {
      for (const auto command : commands)
      {
        if (candidate == command)
        {
          return true;
        }
      }

      return false;
    }

  } // namespace

  int run(std::span<const std::string_view> arguments,
          std::ostream& output,
          std::ostream& error)
  {
    std::error_code filesystem_error;
    const auto working_directory = std::filesystem::current_path(filesystem_error);

    if (filesystem_error)
    {
      error << "forge: could not determine the current directory\n";
      return 2;
    }

    return run(arguments, working_directory, output, error);
  }

  int run(std::span<const std::string_view> arguments,
          const std::filesystem::path& working_directory,
          std::ostream& output,
          std::ostream& error)
  {
    if (arguments.empty() || arguments.front() == "--help" || arguments.front() == "-h")
    {
      print_help(output);
      return 0;
    }

    if (arguments.front() == "--version")
    {
      output << "forge " << version << '\n';
      return 0;
    }

    if (!is_command(arguments.front()))
    {
      error << "forge: unknown command '" << arguments.front() << "'\n";
      return 2;
    }

    if (arguments.front() == "new")
    {
      if (arguments.size() != 2)
      {
        error << "forge: usage: forge new <name>\n";
        return 2;
      }

      return new_project(working_directory, arguments[1], output, error);
    }

    if (arguments.size() != 1)
    {
      error << "forge: commands do not accept arguments yet\n";
      return 2;
    }

    if (arguments.front() == "init")
    {
      return init_project(working_directory, output, error);
    }

    if (arguments.front() == "build")
    {
      return build_project(working_directory, output, error);
    }

    error << "forge: '" << arguments.front() << "' is not implemented yet\n";
    return 2;
  }

} // namespace forge::cli
