#include "init.h"

#include "github.h"
#include "recipe.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <iterator>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace forge
{
  namespace
  {

    std::string escape_toml_string(std::string_view value)
    {
      std::string escaped;
      escaped.reserve(value.size());

      for (const char character : value)
      {
        if (character == '\\' || character == '"')
        {
          escaped += '\\';
        }

        escaped += character;
      }

      return escaped;
    }

    bool is_ignored_directory(const std::filesystem::path& path)
    {
      const auto name = path.filename().string();

      return
        name == ".git"
        || name == ".forge"
        || name == "build"
        || name == "out"
        || name.starts_with("cmake-build-");
    }

    bool is_cpp_source(const std::filesystem::path& path)
    {
      const auto extension = path.extension().string();
      return extension == ".cpp" || extension == ".cc" || extension == ".cxx";
    }

    bool is_cpp_header(const std::filesystem::path& path)
    {
      const auto extension = path.extension().string();
      return extension == ".h"
        || extension == ".hpp"
        || extension == ".hh"
        || extension == ".hxx";
    }

    bool contains_main_function(const std::filesystem::path& path)
    {
      std::ifstream file { path };
      const std::string contents {
        std::istreambuf_iterator<char> { file },
        std::istreambuf_iterator<char> {}
      };
      bool line_comment = false;
      bool block_comment = false;
      char quote = '\0';

      for (std::size_t index = 0; index < contents.size(); ++index)
      {
        const auto character = contents[index];
        const auto next = index + 1 < contents.size() ? contents[index + 1] : '\0';

        if (line_comment)
        {
          line_comment = character != '\n';
          continue;
        }

        if (block_comment)
        {
          if (character == '*' && next == '/')
          {
            block_comment = false;
            ++index;
          }

          continue;
        }

        if (quote != '\0')
        {
          if (character == '\\')
          {
            ++index;
          }
          else if (character == quote)
          {
            quote = '\0';
          }

          continue;
        }

        if (character == '/' && next == '/')
        {
          line_comment = true;
          ++index;
          continue;
        }

        if (character == '/' && next == '*')
        {
          block_comment = true;
          ++index;
          continue;
        }

        if (character == '"' || character == '\'')
        {
          quote = character;
          continue;
        }

        if (contents.compare(index, std::string_view { "main" }.size(), "main") != 0
            || (index != 0
                && (std::isalnum(static_cast<unsigned char>(contents[index - 1]))
                    || contents[index - 1] == '_')))
        {
          continue;
        }

        auto position = index + std::string_view { "main" }.size();

        while (position < contents.size()
               && std::isspace(static_cast<unsigned char>(contents[position])))
        {
          ++position;
        }

        if (position < contents.size() && contents[position] == '(')
        {
          return true;
        }
      }

      return false;
    }

    bool discover_sources(const std::filesystem::path& project_directory,
                          std::vector<std::string>& sources,
                          std::vector<std::string>& public_headers,
                          std::vector<std::string>& headers,
                          std::vector<std::string>& entry_points,
                          std::ostream& error)
    {
      std::error_code filesystem_error;
      std::filesystem::recursive_directory_iterator iterator {
        project_directory,
        std::filesystem::directory_options::skip_permission_denied,
        filesystem_error
      };
      const std::filesystem::recursive_directory_iterator end;

      if (filesystem_error)
      {
        error << "forge: could not inspect '" << project_directory.string() << "'\n";
        return false;
      }

      while (iterator != end)
      {
        const auto& entry = *iterator;

        if (entry.is_directory(filesystem_error) && is_ignored_directory(entry.path()))
        {
          iterator.disable_recursion_pending();
        }
        else if (!filesystem_error && entry.is_regular_file(filesystem_error) && is_cpp_source(entry.path()))
        {
          const auto relative = entry.path().lexically_relative(project_directory).generic_string();
          sources.push_back(relative);

          if (contains_main_function(entry.path()))
          {
            entry_points.push_back(relative);
          }
        }
        else if (!filesystem_error
                 && entry.is_regular_file(filesystem_error)
                 && is_cpp_header(entry.path()))
        {
          const auto relative = entry.path().lexically_relative(project_directory);
          headers.push_back(relative.generic_string());

          if (relative.begin()->string() == "include")
          {
            public_headers.push_back(relative.generic_string());
          }
        }

        if (filesystem_error)
        {
          error << "forge: could not inspect '" << entry.path().string() << "'\n";
          return false;
        }

        iterator.increment(filesystem_error);

        if (filesystem_error)
        {
          error << "forge: could not inspect '" << project_directory.string() << "'\n";
          return false;
        }
      }

      std::ranges::sort(sources);
      std::ranges::sort(public_headers);
      std::ranges::sort(headers);
      std::ranges::sort(entry_points);
      return true;
    }

    std::vector<std::string> included_headers(const std::filesystem::path& path)
    {
      std::ifstream file { path };
      std::vector<std::string> includes;
      std::string line;

      while (std::getline(file, line))
      {
        auto content = std::string_view { line };
        const auto first = content.find_first_not_of(" \t");

        if (first == std::string_view::npos || content[first] != '#')
        {
          continue;
        }

        content.remove_prefix(first + 1);
        const auto directive = content.find_first_not_of(" \t");

        if (directive == std::string_view::npos)
        {
          continue;
        }

        content.remove_prefix(directive);

        if (!content.starts_with("include"))
        {
          continue;
        }

        content.remove_prefix(std::string_view { "include" }.size());
        const auto delimiter = content.find_first_not_of(" \t");

        if (delimiter == std::string_view::npos
            || (content[delimiter] != '<' && content[delimiter] != '"'))
        {
          continue;
        }

        const auto closing = content[delimiter] == '<' ? '>' : '"';
        const auto end = content.find(closing, delimiter + 1);

        if (end != std::string_view::npos)
        {
          auto include = std::string { content.substr(delimiter + 1, end - delimiter - 1) };
          std::replace(include.begin(), include.end(), '\\', '/');
          includes.push_back(std::move(include));
        }
      }

      return includes;
    }

    bool looks_like_dependency_include(std::string_view include)
    {
      static const std::set<std::string_view> system_headers {
        "assert.h", "complex.h", "ctype.h", "errno.h", "fenv.h", "float.h",
        "inttypes.h", "limits.h", "locale.h", "math.h", "process.h", "setjmp.h",
        "signal.h", "stdarg.h", "stdbool.h", "stddef.h", "stdint.h", "stdio.h",
        "stdlib.h", "string.h", "time.h", "uchar.h", "unistd.h", "wchar.h",
        "wctype.h", "windows.h",
        "algorithm", "any", "array", "atomic", "barrier", "bit", "bitset",
        "cassert", "ccomplex", "cctype", "cerrno", "cfenv", "cfloat", "charconv",
        "chrono", "cinttypes", "ciso646", "climits", "clocale", "cmath",
        "codecvt", "compare", "complex", "concepts", "condition_variable",
        "coroutine", "csetjmp", "csignal", "cstdarg", "cstdbool", "cstddef",
        "cstdint", "cstdio", "cstdlib", "cstring", "ctgmath", "ctime", "cuchar",
        "cwchar", "cwctype", "deque",
        "exception", "execution", "filesystem", "format", "forward_list",
        "fstream", "functional", "future", "initializer_list", "iomanip",
        "ios", "iosfwd", "iostream", "istream", "iterator", "latch", "limits",
        "list", "map", "memory", "memory_resource", "mutex", "new", "numbers",
        "numeric", "optional", "ostream", "queue", "random", "ranges", "ratio",
        "regex", "scoped_allocator", "semaphore", "set", "shared_mutex",
        "source_location", "span", "sstream", "stack", "stdexcept", "stop_token",
        "streambuf", "string", "string_view", "syncstream", "system_error",
        "thread", "tuple", "type_traits", "typeindex", "typeinfo",
        "unordered_map", "unordered_set", "utility", "valarray", "variant",
        "vector", "version"
      };

      return
        !system_headers.contains(include)
        && !include.starts_with("sys/")
        && !include.starts_with("linux/")
        && !include.starts_with("machine/")
        && !include.starts_with("arpa/")
        && !include.starts_with("netinet/");
    }

    std::optional<std::string> resolve_local_header(
      const std::string& including_file,
      const std::string& include,
      const std::vector<std::string>& headers);

    std::map<std::string, std::string> unresolved_includes(
      const std::filesystem::path& project_directory,
      const std::vector<std::string>& sources,
      const std::vector<std::string>& headers)
    {
      std::map<std::string, std::string> unresolved;
      std::vector<std::string> scanned_files = sources;
      scanned_files.insert(scanned_files.end(), headers.begin(), headers.end());

      for (const auto& scanned_file : scanned_files)
      {
        for (const auto& include : included_headers(project_directory / scanned_file))
        {
          if (!resolve_local_header(scanned_file, include, headers)
              && looks_like_dependency_include(include))
          {
            unresolved.try_emplace(include, scanned_file);
          }
        }
      }

      return unresolved;
    }

    struct SiblingDependency
    {
      std::string name;
      std::string path;
    };

    bool provides_include(const Recipe& recipe, std::string_view include)
    {
      if (!recipe.targets.empty()
          || (recipe.type != "static_library"
              && recipe.type != "dynamic_library"
              && recipe.type != "header_only"
              && recipe.type != "imported_library"))
      {
        return false;
      }

      for (const auto& header : recipe.public_headers)
      {
        const auto generic = header.generic_string();

        if (generic.starts_with("include/") && generic.substr(8) == include)
        {
          return true;
        }
      }

      for (const auto& profile : recipe.imports)
      {
        for (const auto& header : profile.public_headers)
        {
          const auto generic = header.generic_string();

          if (generic == include || generic.ends_with('/' + std::string { include }))
          {
            return true;
          }
        }
      }

      return false;
    }

    std::vector<SiblingDependency> infer_sibling_dependencies(
      const std::filesystem::path& project_directory,
      std::map<std::string, std::string>& unresolved)
    {
      const auto parent = project_directory.parent_path();
      std::error_code filesystem_error;
      std::map<std::string, std::vector<SiblingDependency>> matches;

      for (const auto& entry : std::filesystem::directory_iterator { parent, filesystem_error })
      {
        if (filesystem_error)
        {
          break;
        }

        if (!entry.is_directory(filesystem_error)
            || std::filesystem::equivalent(entry.path(), project_directory, filesystem_error)
            || !std::filesystem::is_regular_file(
              entry.path() / "forge.recipe.toml",
              filesystem_error
            ))
        {
          filesystem_error.clear();
          continue;
        }

        Recipe sibling;
        std::ostringstream ignored_error;

        if (!read_recipe(entry.path() / "forge.recipe.toml", sibling, ignored_error))
        {
          continue;
        }

        const auto relative =
          std::filesystem::relative(entry.path(), project_directory, filesystem_error);

        if (filesystem_error)
        {
          filesystem_error.clear();
          continue;
        }

        for (const auto& [include, source] : unresolved)
        {
          if (provides_include(sibling, include))
          {
            matches[include].push_back(SiblingDependency {
              sibling.name,
              relative.generic_string()
            });
          }
        }
      }

      std::map<std::string, std::vector<std::pair<std::string, SiblingDependency>>> by_name;

      for (const auto& [include, candidates] : matches)
      {
        if (candidates.size() == 1)
        {
          by_name[candidates.front().name].emplace_back(include, candidates.front());
        }
      }

      std::vector<SiblingDependency> result;

      for (const auto& [name, candidates] : by_name)
      {
        const auto path = candidates.front().second.path;
        const auto same_project = std::ranges::all_of(
          candidates,
          [&path](const auto& candidate)
          {
            return candidate.second.path == path;
          }
        );

        if (!same_project)
        {
          continue;
        }

        result.push_back(candidates.front().second);

        for (const auto& [include, dependency] : candidates)
        {
          unresolved.erase(include);
        }
      }

      return result;
    }

    std::vector<std::string> infer_include_directories(
      const std::filesystem::path& project_directory,
      const std::vector<std::string>& sources,
      const std::vector<std::string>& headers)
    {
      std::set<std::string> include_directories;
      std::vector<std::string> scanned_files = sources;
      scanned_files.insert(scanned_files.end(), headers.begin(), headers.end());

      for (const auto& scanned_file : scanned_files)
      {
        for (const auto& include : included_headers(project_directory / scanned_file))
        {
          std::set<std::string> matching_roots;

          for (const auto& header : headers)
          {
            if (header == include)
            {
              matching_roots.insert(".");
              continue;
            }

            const auto suffix = '/' + include;

            if (header.size() > suffix.size() && header.ends_with(suffix))
            {
              matching_roots.insert(header.substr(0, header.size() - suffix.size()));
            }
          }

          if (matching_roots.size() == 1 && *matching_roots.begin() != "include")
          {
            include_directories.insert(*matching_roots.begin());
          }
        }
      }

      return { include_directories.begin(), include_directories.end() };
    }

    std::optional<std::string> resolve_local_header(
      const std::string& including_file,
      const std::string& include,
      const std::vector<std::string>& headers)
    {
      std::set<std::string> matches;
      const auto relative =
        (std::filesystem::path { including_file }.parent_path() / include)
          .lexically_normal()
          .generic_string();

      if (std::binary_search(headers.begin(), headers.end(), relative))
      {
        matches.insert(relative);
      }

      if (std::binary_search(headers.begin(), headers.end(), include))
      {
        matches.insert(include);
      }

      const auto suffix = '/' + include;

      for (const auto& header : headers)
      {
        if (header.size() > suffix.size() && header.ends_with(suffix))
        {
          matches.insert(header);
        }
      }

      if (matches.size() == 1)
      {
        return *matches.begin();
      }

      return std::nullopt;
    }

    std::set<std::string> reachable_local_headers(
      const std::filesystem::path& project_directory,
      const std::string& source,
      const std::vector<std::string>& headers)
    {
      std::set<std::string> reachable;
      std::vector<std::string> pending { source };

      while (!pending.empty())
      {
        auto file = std::move(pending.back());
        pending.pop_back();

        for (const auto& include : included_headers(project_directory / file))
        {
          const auto resolved = resolve_local_header(file, include, headers);

          if (resolved && reachable.insert(*resolved).second)
          {
            pending.push_back(*resolved);
          }
        }
      }

      return reachable;
    }

    std::vector<std::vector<std::string>> infer_target_sources(
      const std::filesystem::path& project_directory,
      const std::vector<std::string>& sources,
      const std::vector<std::string>& headers,
      const std::vector<std::string>& entry_points)
    {
      std::map<std::string, std::set<std::string>> reachable;

      for (const auto& source : sources)
      {
        reachable[source] = reachable_local_headers(project_directory, source, headers);
      }

      std::vector<std::vector<std::string>> target_sources(entry_points.size());

      for (std::size_t target_index = 0; target_index < entry_points.size(); ++target_index)
      {
        target_sources[target_index].push_back(entry_points[target_index]);
      }

      for (const auto& source : sources)
      {
        if (std::binary_search(entry_points.begin(), entry_points.end(), source))
        {
          continue;
        }

        std::vector<std::size_t> owners;

        for (std::size_t target_index = 0; target_index < entry_points.size(); ++target_index)
        {
          const auto& target_headers = reachable.at(entry_points[target_index]);
          const auto& source_headers = reachable.at(source);

          if (std::ranges::any_of(
            source_headers,
            [&target_headers](const std::string& header)
            {
              return target_headers.contains(header);
            }
          ))
          {
            owners.push_back(target_index);
          }
        }

        if (owners.empty())
        {
          for (std::size_t target_index = 0; target_index < entry_points.size(); ++target_index)
          {
            target_sources[target_index].push_back(source);
          }
        }
        else
        {
          for (const auto owner : owners)
          {
            target_sources[owner].push_back(source);
          }
        }
      }

      for (auto& target : target_sources)
      {
        std::ranges::sort(target);
      }

      return target_sources;
    }

    std::string format_sources(const std::vector<std::string>& sources)
    {
      if (sources.empty())
      {
        return "[]";
      }

      std::string formatted = "[";

      for (std::size_t index = 0; index < sources.size(); ++index)
      {
        if (index != 0)
        {
          formatted += ", ";
        }

        formatted += '"' + escape_toml_string(sources[index]) + '"';
      }

      formatted += ']';
      return formatted;
    }

    std::string target_name(const std::filesystem::path& source, std::size_t index)
    {
      auto name = source.stem().string();

      for (char& character : name)
      {
        if (!std::isalnum(static_cast<unsigned char>(character))
            && character != '-'
            && character != '_')
        {
          character = '-';
        }
      }

      return name.empty() ? "executable-" + std::to_string(index + 1) : name;
    }

    bool write_file(const std::filesystem::path& path,
                    std::string_view contents,
                    std::ostream& error)
    {
      std::ofstream file { path };

      if (!file)
      {
        error << "forge: could not create '" << path.string() << "'\n";
        return false;
      }

      file << contents;

      if (!file)
      {
        error << "forge: could not write '" << path.string() << "'\n";
        return false;
      }

      return true;
    }

  } // namespace

  int adopt_project(const std::filesystem::path& project_directory,
                    std::ostream& output,
                    std::ostream& error)
  {
    const auto recipe_path = project_directory / "forge.recipe.toml";

    std::error_code filesystem_error;

    if (std::filesystem::exists(recipe_path, filesystem_error))
    {
      error << "forge: '" << recipe_path.string() << "' already exists\n";
      return 2;
    }

    if (filesystem_error)
    {
      error << "forge: could not inspect '" << recipe_path.string() << "'\n";
      return 2;
    }

    std::vector<std::string> sources;
    std::vector<std::string> public_headers;
    std::vector<std::string> headers;
    std::vector<std::string> entry_points;

    if (!discover_sources(project_directory, sources, public_headers, headers, entry_points, error))
    {
      return 2;
    }

    const auto include_directories = infer_include_directories(project_directory, sources, headers);
    auto unresolved = unresolved_includes(project_directory, sources, headers);
    const auto sibling_dependencies = infer_sibling_dependencies(project_directory, unresolved);
    const auto project_name = project_directory.filename().string();
    const auto escaped_project_name = escape_toml_string(project_name);
    const auto formatted_sources = format_sources(sources);
    const auto formatted_headers = format_sources(public_headers);
    const auto formatted_include_directories = format_sources(include_directories);
    std::string recipe =
      "#:schema " + std::string { recipe_schema_url } + "\n"
      "\n"
      "[project]\n"
      "name = \"" + escaped_project_name + "\"\n"
      "version = \"0.1.0\"\n";

    if (entry_points.size() > 1)
    {
      std::set<std::string> target_names;
      const auto inferred_target_sources =
        infer_target_sources(project_directory, sources, headers, entry_points);

      for (std::size_t index = 0; index < entry_points.size(); ++index)
      {
        auto name = target_name(entry_points[index], index);

        if (!target_names.insert(name).second)
        {
          name += '-' + std::to_string(index + 1);
          target_names.insert(name);
        }

        recipe
          += "\n[target." + name + "]\n"
          "type = \"executable\"\n"
          "cpp_std = 20\n"
          "sources = " + format_sources(inferred_target_sources[index]) + "\n";

        if (!public_headers.empty())
        {
          recipe += "public_headers = " + formatted_headers + "\n";
        }

        if (!include_directories.empty())
        {
          recipe += "include_dirs = " + formatted_include_directories + "\n";
        }
      }
    }
    else
    {
      const auto type =
        sources.empty() && !public_headers.empty()
          ? "header_only"
          : entry_points.empty() && !sources.empty() && !public_headers.empty()
          ? "static_library"
          : "executable";
      recipe
        += "type = \"" + std::string { type } + "\"\n"
        "cpp_std = 20\n"
        "\n"
        "[sources]\n"
        "paths = " + formatted_sources + "\n";

      if (!public_headers.empty())
      {
        recipe += "public_headers = " + formatted_headers + "\n";
      }

      if (!include_directories.empty())
      {
        recipe += "include_dirs = " + formatted_include_directories + "\n";
      }
    }

    if (!sibling_dependencies.empty())
    {
      recipe += "\n[dependencies]\n";

      for (const auto& dependency : sibling_dependencies)
      {
        recipe += dependency.name + " = { path = \""
          + escape_toml_string(dependency.path) + "\" }\n";
      }
    }

    if (!write_file(recipe_path, recipe, error))
    {
      return 2;
    }

    if (!generate_github_release_support(project_directory, error))
    {
      return 2;
    }

    output
      << "Adopted project '" << project_name << "'\n"
      << "Created " << recipe_path.string() << '\n'
      << "Found " << sources.size() << " C++ source file";

    if (sources.size() != 1)
    {
      output << 's';
    }

    output << '\n'
           << "Found " << entry_points.size() << " main() entry point";

    if (entry_points.size() != 1)
    {
      output << 's';
    }

    output << '\n';

    if (!include_directories.empty())
    {
      output << "Inferred " << include_directories.size() << " local include director";
      output << (include_directories.size() == 1 ? "y\n" : "ies\n");
    }

    if (!unresolved.empty())
    {
      output << "Found " << unresolved.size() << " unresolved dependency include";
      output << (unresolved.size() == 1 ? ":\n" : "s:\n");

      for (const auto& [include, source] : unresolved)
      {
        output << "  <" << include << "> from " << source << '\n';
      }
    }

    if (!sibling_dependencies.empty())
    {
      output << "Inferred " << sibling_dependencies.size() << " sibling project dependenc";
      output << (sibling_dependencies.size() == 1 ? "y:\n" : "ies:\n");

      for (const auto& dependency : sibling_dependencies)
      {
        output << "  " << dependency.name << " = " << dependency.path << '\n';
      }
    }

    if (entry_points.empty() && !sources.empty() && public_headers.empty())
    {
      output << "Could not infer a library interface; generated an executable recipe\n";
    }

    return 0;
  }

  int init_project(const std::filesystem::path& project_directory,
                   std::ostream& output,
                   std::ostream& error)
  {
    return adopt_project(project_directory, output, error);
  }

} // namespace forge
