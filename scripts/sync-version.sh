#!/usr/bin/env bash
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$root"

if [[ $# -gt 1 ]]; then
  echo "usage: scripts/sync-version.sh [major|minor|patch]" >&2
  exit 2
fi

if [[ $# -eq 1 ]]; then
  if [[ "$1" != "major" && "$1" != "minor" && "$1" != "patch" ]]; then
    echo "usage: scripts/sync-version.sh [major|minor|patch]" >&2
    exit 2
  fi

  forge="${FORGE:-./build/dev/forge}"
  "$forge" bump "$1"
fi

read -r version build_number < <(
  awk '
    $0 ~ /^\[project\]/ { section = "project"; next }
    $0 ~ /^\[build\]/ { section = "build"; next }
    $0 ~ /^\[/ { section = ""; next }
    section == "project" && $1 == "version" {
      value = $3
      gsub(/"/, "", value)
      version = value
    }
    section == "build" && $1 == "number" {
      build = $3
    }
    END {
      if (version == "" || build == "") {
        exit 1
      }
      print version, build
    }
  ' forge.recipe.toml
) || {
  echo "scripts/sync-version.sh: could not read project.version and build.number from forge.recipe.toml" >&2
  exit 1
}

qualified_version="${version}+build.${build_number}"

sync_file() {
  local file="$1"
  local expression="$2"

  perl -0pi -e "$expression" "$file"
}

sync_file CMakeLists.txt "s/(project\\(\\s*forge\\s*VERSION\\s*)[0-9]+\\.[0-9]+\\.[0-9]+/\${1}${version}/s"
sync_file src/cli.h "s/(inline constexpr std::string_view version = \")[^\"]+(\")/\${1}${qualified_version}\${2}/"
sync_file tests/cli_test.cpp "s/(constexpr std::string_view expected_version = \")[^\"]+(\")/\${1}${qualified_version}\${2}/"

grep -q "VERSION ${version}" CMakeLists.txt
grep -q "version = \"${qualified_version}\"" src/cli.h
grep -q "expected_version = \"${qualified_version}\"" tests/cli_test.cpp

echo "Synced Forge version ${qualified_version}"
