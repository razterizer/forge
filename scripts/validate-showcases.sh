#!/usr/bin/env bash
set -euo pipefail

usage() {
  cat <<'EOF'
usage: scripts/validate-showcases.sh [--forge <path>] [--keep] [--run]

Clone and validate every no-tinkering showcase in an isolated temporary
sandbox. The script downloads the pinned Raylib source from GitHub.

  --forge <path>  Forge executable (default: this checkout's build/dev/forge).
  --keep          Preserve the temporary sandbox after validation and print its
                  path for inspection.
  --run           Launch each validated demo after it builds. Close each
                  graphical demo to continue to the next one.

The current suite validates Raylib 6.0's textures_tiled_drawing and
audio_sound_loading demos. A successful build verifies adoption and runtime
resource staging; --run is the manual graphics and audio verification step.
EOF
}

script_directory="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
forge="$script_directory/../build/dev/forge"
raylib_revision="dbc56a87da87d973a9c5baa4e7438a9d20121d28"
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

validate_raylib() {
  local project="$sandbox/raylib"

  git clone --depth 1 --branch 6.0 https://github.com/raysan5/raylib.git "$project"
  [[ "$(git -C "$project" rev-parse HEAD)" == "$raylib_revision" ]] || {
    echo "showcase validation: Raylib 6.0 did not resolve to its validated revision" >&2
    exit 1
  }
  (
    cd "$project"
    if ! "$forge" adopt > "$sandbox/raylib-adopt.log" 2>&1; then
      cat "$sandbox/raylib-adopt.log" >&2
      exit 1
    fi
  )
  echo "Adopted Raylib (full log: $sandbox/raylib-adopt.log)"

  (
    cd "$project/examples"

    "$forge" build textures_tiled_drawing
    require_staged_file "$project/examples" patterns.png

    "$forge" build audio_sound_loading
    require_staged_file "$project/examples" sound.wav

    if [[ "$run" == true ]]; then
      "$forge" run textures_tiled_drawing
      "$forge" run audio_sound_loading
    fi
  )
}

echo "[1/1] Raylib runtime-resource showcase"
validate_raylib
echo "Validated all registered no-tinkering showcase demos."
