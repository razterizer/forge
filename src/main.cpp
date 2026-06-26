#include "cli.h"

#include <iostream>
#include <string_view>
#include <vector>

int main(int argc, char* argv[])
{
  std::vector<std::string_view> arguments;
  arguments.reserve(static_cast<std::size_t>(argc > 0 ? argc - 1 : 0));

  for (int index = 1; index < argc; ++index)
    arguments.emplace_back(argv[index]);

  return forge::cli::run(arguments, std::cout, std::cerr);
}
