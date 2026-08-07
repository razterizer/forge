#include "fprocess.h"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <string>
#include <string_view>
#include <vector>
#include <filesystem>
#include <ostream>

#ifdef _WIN32
#include <process.h>
#include <windows.h>
#else
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace forge
{

#ifdef _WIN32
  namespace
  {

    bool executable_is_available(const char* name)
    {
      std::vector<char> path(32768);
      return SearchPathA(nullptr, name, nullptr, static_cast<DWORD>(path.size()), path.data(), nullptr) != 0;
    }

    std::optional<std::string> read_command_output(const std::string& command)
    {
      auto* pipe = _popen(command.c_str(), "r");

      if (!pipe)
        return std::nullopt;

      std::string output;
      char buffer[4096];

      while (std::fgets(buffer, sizeof(buffer), pipe))
        output += buffer;

      if (_pclose(pipe) != 0)
        return std::nullopt;

      return output;
    }

    std::string trim_line(std::string value)
    {
      const auto end = value.find_first_of("\r\n");

      if (end != std::string::npos)
        value.erase(end);

      return value;
    }

    const char* target_architecture()
    {
#if defined(_M_ARM64)
      return "arm64";
#elif defined(_M_IX86)
      return "x86";
#else
      return "x64";
#endif
    }

    bool initialize_visual_studio_environment(std::ostream& error)
    {
      if (executable_is_available("cl.exe"))
        return true;

      const auto* program_files = std::getenv("ProgramFiles(x86)");

      if (!program_files)
      {
        error << "forge: could not locate Visual Studio Installer (ProgramFiles(x86) is unset)\n";
        return false;
      }

      const auto vswhere = std::filesystem::path(program_files)
        / "Microsoft Visual Studio/Installer/vswhere.exe";

      if (!std::filesystem::is_regular_file(vswhere))
      {
        error << "forge: could not locate vswhere.exe; install Visual Studio Build Tools with the C++ workload\n";
        return false;
      }

      const auto installation_output = read_command_output(
        "\"" + vswhere.string()
        + "\" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64"
          " -property installationPath"
      );
      const auto installation = installation_output
        ? trim_line(*installation_output)
        : std::string {};

      if (installation.empty())
      {
        error << "forge: Visual Studio Build Tools with the C++ workload were not found\n";
        return false;
      }

      const auto developer_command = std::filesystem::path(installation)
        / "Common7/Tools/VsDevCmd.bat";

      if (!std::filesystem::is_regular_file(developer_command))
      {
        error << "forge: could not locate '" << developer_command.string() << "'\n";
        return false;
      }

      const auto environment = read_command_output(
        "cmd.exe /d /s /c \"\"" + developer_command.string()
        + "\" -no_logo -arch=" + target_architecture()
        + " -host_arch=" + target_architecture() + " >nul && set\""
      );

      if (!environment)
      {
        error << "forge: failed to initialize the Visual Studio developer environment\n";
        return false;
      }

      std::string_view remaining = *environment;

      while (!remaining.empty())
      {
        const auto line_end = remaining.find('\n');
        auto line = remaining.substr(0, line_end);

        if (!line.empty() && line.back() == '\r')
          line.remove_suffix(1);

        const auto separator = line.find('=');

        // cmd.exe also emits pseudo variables such as "=C:=C:\\...".
        if (separator != std::string_view::npos && separator != 0)
        {
          const std::string name(line.substr(0, separator));
          const std::string value(line.substr(separator + 1));
          _putenv_s(name.c_str(), value.c_str());
        }

        if (line_end == std::string_view::npos)
          break;

        remaining.remove_prefix(line_end + 1);
      }

      if (!executable_is_available("cl.exe"))
      {
        error << "forge: Visual Studio was found, but cl.exe is unavailable after initialization\n";
        return false;
      }

      return true;
    }

  } // namespace
#endif

  int run_process(const std::vector<std::string>& arguments,
                  const std::filesystem::path& working_directory,
                  std::ostream& error)
  {
    if (arguments.empty())
    {
      error << "forge: cannot run an empty command\n";
      return 2;
    }

    std::vector<char*> native_arguments;
    native_arguments.reserve(arguments.size() + 1);

    for (const auto& argument : arguments)
      native_arguments.push_back(const_cast<char*>(argument.c_str()));

    native_arguments.push_back(nullptr);

#ifdef _WIN32
    const auto configures_cmake = arguments.front() == "cmake"
      && std::find(arguments.begin(), arguments.end(), "-S") != arguments.end();

    if (configures_cmake
        && !initialize_visual_studio_environment(error))
    {
      return 2;
    }

    const auto previous_directory = std::filesystem::current_path();
    std::filesystem::current_path(working_directory);
    const auto result = _spawnvp(_P_WAIT, native_arguments.front(), native_arguments.data());
    std::filesystem::current_path(previous_directory);

    if (result == -1)
    {
      error << "forge: could not run '" << arguments.front() << "'\n";
      return 2;
    }

    return static_cast<int>(result);
#else
    const auto child = fork();

    if (child == -1)
    {
      error << "forge: could not start '" << arguments.front() << "': " << std::strerror(errno) << '\n';
      return 2;
    }

    if (child == 0)
    {
      if (chdir(working_directory.c_str()) != 0)
        _exit(126);

      execvp(native_arguments.front(), native_arguments.data());
      _exit(127);
    }

    int status = 0;

    if (waitpid(child, &status, 0) == -1)
    {
      error << "forge: could not wait for '" << arguments.front() << "'\n";
      return 2;
    }

    if (WIFEXITED(status))
      return WEXITSTATUS(status);

    return 2;
#endif
  }

} // namespace forge
