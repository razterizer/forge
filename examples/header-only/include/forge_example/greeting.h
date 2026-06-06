#pragma once

#include <string_view>

namespace forge_example
{

  inline constexpr std::string_view greeting()
  {
    return "Hello from a header-only Forge project!";
  }

} // namespace forge_example
