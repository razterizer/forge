#!/usr/bin/env bash
set -euo pipefail

usage() {
  cat <<'EOF'
usage: scripts/validate-showcases.sh --root <checkout-directory> [--forge <path>] [--clean] [--run]

Validate every no-tinkering showcase registered by this script.

  --root <directory>  Directory containing the showcase checkouts (required).
  --forge <path>      Forge executable (default: this checkout's build/dev/forge).
  --clean             Remove untracked and ignored files from each checkout
                      before adoption. Required for a fresh-state validation.
  --run               Launch each validated demo after it builds. Close each
                      graphical demo to continue to the next one.

The current suite validates Raylib's textures_tiled_drawing and
audio_module_playing demos. A successful build verifies adoption and runtime
resource staging; --run is the manual graphics and audio verification step.
EOF
}

script_directory="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
root=""
forge="$script_directory/../build/dev/forge"
clean=false
run=false

while [[ $# -gt 0 ]]; do
  case "$1" in
    --root)
      [[ $# -ge 2 ]] || { usage >&2; exit 2; }
      root="$2"
      shift 2
      ;;
    --forge)
      [[ $# -ge 2 ]] || { usage >&2; exit 2; }
      forge="$2"
      shift 2
      ;;
    --clean)
      clean=true
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

[[ -n "$root" ]] || { usage >&2; exit 2; }
[[ -d "$root" ]] || { echo "showcase validation: root does not exist: $root" >&2; exit 2; }
if [[ "$forge" != /* ]]; then
  forge="$(cd "$(dirname "$forge")" && pwd)/$(basename "$forge")"
fi
[[ -x "$forge" ]] || { echo "showcase validation: Forge executable is not runnable: $forge" >&2; exit 2; }

prepare_checkout() {
  local project="$1"

  [[ -d "$project/.git" ]] || {
    echo "showcase validation: expected a Git checkout: $project" >&2
    exit 2
  }

  if ! git -C "$project" diff --quiet || ! git -C "$project" diff --cached --quiet; then
    echo "showcase validation: tracked changes prevent a clean validation: $project" >&2
    exit 2
  fi

  if [[ "$clean" != true ]]; then
    echo "showcase validation: --clean is required to remove prior Forge state in $project" >&2
    exit 2
  fi

  git -C "$project" clean -fdx
}

require_staged_file() {
  local project="$1"
  local filename="$2"

  if ! find "$project/.forge" -type f -path "*/resources/$filename" -print -quit | grep -q .; then
    echo "showcase validation: runtime resource was not staged: $filename" >&2
    exit 1
  fi
}

validate_raylib() {
  local project="$root/raylib"

  prepare_checkout "$project"
  (
    cd "$project"
    "$forge" adopt

    "$forge" build textures_tiled_drawing
    require_staged_file "$project" patterns.png

    "$forge" build audio_module_playing
    require_staged_file "$project" sound.wav

    if [[ "$run" == true ]]; then
      "$forge" run textures_tiled_drawing
      "$forge" run audio_module_playing
    fi
  )
}

echo "[1/1] Raylib runtime-resource showcase"
validate_raylib
echo "Validated all registered no-tinkering showcase demos."
