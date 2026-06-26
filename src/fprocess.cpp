#include "fprocess.h"

#include <cerrno>
#include <cstring>
#include <vector>
#include <filesystem>
#include <ostream>

#ifdef _WIN32
#include <process.h>
#else
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace forge
{

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
