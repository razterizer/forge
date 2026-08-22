#!/usr/bin/env bash
set -euo pipefail

usage() {
  cat <<'EOF'
usage: scripts/validate-showcases.sh [--forge <path>] [--keep] [--run]

Clone and validate every no-tinkering showcase in an isolated temporary
sandbox. The script downloads pinned Raylib, Meshoptimizer, fmt, and spdlog
sources from GitHub.

  --forge <path>  Forge executable (default: this checkout's build/dev/forge).
  --keep          Preserve the temporary sandbox after validation and print its
                  path for inspection.
  --run           Launch each validated demo after it builds. Close each
                  graphical demo to continue to the next one.

The current suite validates Raylib 6.0's textures_tiled_drawing and
audio_sound_loading demos, plus static-library cboxes for Meshoptimizer 1.2,
fmt 12.2.0, and spdlog 1.17.0. A successful build verifies adoption and
runtime resource staging; --run is the manual graphics and audio verification
step.
EOF
}

script_directory="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
forge="$script_directory/../build/dev/forge"
raylib_revision="dbc56a87da87d973a9c5baa4e7438a9d20121d28"
meshoptimizer_revision="9d9890c73011d75920af614485296d1e03e95448"
fmt_revision="1be298e1bd68957e4cd352e1f676f00e07dcfb57"
spdlog_revision="79524ddd08a4ec981b7fea76afd08ee05f83755d"
keep=false
run=false

while [[ $# -gt 0 ]]; do
  case "$1" in
    --forge)
      [[ $# -ge 2 ]] || { usage >&2; exit 2; }
      forge="$2"
      shift 2
      ;;
    --keep)
      keep=true
      shift
      ;;
    --run)
      run=true
      shift
      ;;
    --help|-h)
      usage
      exit 0
      ;;
    *)
      usage >&2
      exit 2
      ;;
  esac
done

if [[ "$forge" != /* ]]; then
  forge="$(cd "$(dirname "$forge")" && pwd)/$(basename "$forge")"
fi

[[ -x "$forge" ]] || {
  echo "showcase validation: Forge executable is not runnable: $forge" >&2
  echo "Build Forge first with: cmake --preset dev && cmake --build --preset dev" >&2
  exit 2
}

sandbox="$(mktemp -d "${TMPDIR:-/tmp}/forge-showcases.XXXXXX")"

cleanup() {
  if [[ "$keep" == true ]]; then
    echo "Showcase sandbox preserved at $sandbox"
  else
    rm -rf -- "$sandbox"
  fi
}
trap cleanup EXIT

require_staged_file() {
  local project="$1"
  local filename="$2"

  if ! find "$project/.forge" -type f -path "*/resources/$filename" -print -quit | grep -q .; then
    echo "showcase validation: runtime resource was not staged: $filename" >&2
    exit 1
  fi
}

adopt_project() {
  local project="$1"
  local name="$2"
  local log="$sandbox/$name-adopt.log"

  (
    cd "$project"
    if ! "$forge" adopt > "$log" 2>&1; then
      cat "$log" >&2
      exit 1
    fi
  )
  echo "Adopted $name (full log: $log)"
}

run_logged() {
  local name="$1"
  shift
  local log="$sandbox/$name.log"

  if ! "$@" > "$log" 2>&1; then
    cat "$log" >&2
    exit 1
  fi
}

section() {
  printf '\n========== %s ==========\n' "$1"
}

validate_raylib() {
  local project="$sandbox/raylib"

  git clone --depth 1 --branch 6.0 https://github.com/raysan5/raylib.git "$project"
  [[ "$(git -C "$project" rev-parse HEAD)" == "$raylib_revision" ]] || {
    echo "showcase validation: Raylib 6.0 did not resolve to its validated revision" >&2
    exit 1
  }
  adopt_project "$project" raylib

  (
    cd "$project/examples"

    run_logged raylib-textures-build "$forge" build textures_tiled_drawing
    echo "Built Raylib textures_tiled_drawing"
    require_staged_file "$project/examples" patterns.png

    run_logged raylib-audio-build "$forge" build audio_sound_loading
    echo "Built Raylib audio_sound_loading"
    require_staged_file "$project/examples" sound.wav

    if [[ "$run" == true ]]; then
      "$forge" run textures_tiled_drawing
      "$forge" run audio_sound_loading
    fi
  )
}

validate_static_library() {
  local name="$1"
  local display_name="$2"
  local url="$3"
  local tag="$4"
  local revision="$5"
  local project="$sandbox/$name"

  git clone --depth 1 --branch "$tag" "$url" "$project"
  [[ "$(git -C "$project" rev-parse HEAD)" == "$revision" ]] || {
    echo "showcase validation: $display_name $tag did not resolve to its validated revision" >&2
    exit 1
  }
  adopt_project "$project" "$name"
  (
    cd "$project"
    run_logged "$name-build" "$forge" build
    echo "Built $display_name"
    run_logged "$name-box" "$forge" box create
    echo "Created $display_name cbox"
  )

  if ! find "$project/.forge/boxes" -type f -name '*.cbox' -print -quit | grep -q .; then
    echo "showcase validation: $display_name cbox was not created" >&2
    exit 1
  fi
}

section "[1/4] Raylib 6.0 runtime-resource showcase"
validate_raylib
section "[2/4] Meshoptimizer 1.2 static-library cbox"
validate_static_library meshoptimizer Meshoptimizer https://github.com/zeux/meshoptimizer.git v1.2 "$meshoptimizer_revision"
section "[3/4] fmt 12.2.0 static-library cbox"
validate_static_library fmt fmt https://github.com/fmtlib/fmt.git 12.2.0 "$fmt_revision"
section "[4/4] spdlog 1.17.0 static-library cbox"
validate_static_library spdlog spdlog https://github.com/gabime/spdlog.git v1.17.0 "$spdlog_revision"
section "Validated all registered no-tinkering showcase demos"
