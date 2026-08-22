#!/usr/bin/env bash
set -euo pipefail

usage() {
  cat <<'EOF'
usage: scripts/validate-showcases.sh [--forge <path>] [--keep] [--run]

Clone and validate every no-tinkering showcase in an isolated temporary
sandbox. The script downloads pinned Raylib and Meshoptimizer sources from
GitHub.

  --forge <path>  Forge executable (default: this checkout's build/dev/forge).
  --keep          Preserve the temporary sandbox after validation and print its
                  path for inspection.
  --run           Launch each validated demo after it builds. Close each
                  graphical demo to continue to the next one.

The current suite validates Raylib 6.0's textures_tiled_drawing and
audio_sound_loading demos, plus Meshoptimizer 1.2's static-library cbox. A
successful build verifies adoption and runtime resource staging; --run is the
manual graphics and audio verification step.
EOF
}

script_directory="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
forge="$script_directory/../build/dev/forge"
raylib_revision="dbc56a87da87d973a9c5baa4e7438a9d20121d28"
meshoptimizer_revision="9d9890c73011d75920af614485296d1e03e95448"
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

validate_meshoptimizer() {
  local project="$sandbox/meshoptimizer"

  git clone --depth 1 --branch v1.2 https://github.com/zeux/meshoptimizer.git "$project"
  [[ "$(git -C "$project" rev-parse HEAD)" == "$meshoptimizer_revision" ]] || {
    echo "showcase validation: Meshoptimizer v1.2 did not resolve to its validated revision" >&2
    exit 1
  }
  adopt_project "$project" meshoptimizer
  (
    cd "$project"
    run_logged meshoptimizer-build "$forge" build
    echo "Built Meshoptimizer"
    run_logged meshoptimizer-box "$forge" box create
    echo "Created Meshoptimizer cbox"
  )

  if ! find "$project/.forge/boxes" -type f -name 'meshoptimizer-*.cbox' -print -quit | grep -q .; then
    echo "showcase validation: Meshoptimizer cbox was not created" >&2
    exit 1
  fi
}

echo "[1/2] Raylib runtime-resource showcase"
validate_raylib
echo "[2/2] Meshoptimizer static-library showcase"
validate_meshoptimizer
echo "Validated all registered no-tinkering showcase demos."
