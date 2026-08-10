#!/usr/bin/env bash
set -euo pipefail

repository="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
root=""
resume=false
forwarded=()

while (($#)); do
  case "$1" in
    --output)
      [[ $# -ge 2 ]] || { echo "run_all.sh: --output requires a directory" >&2; exit 2; }
      root="$2"
      resume=false
      shift 2
      ;;
    --resume)
      [[ $# -ge 2 ]] || { echo "run_all.sh: --resume requires a directory" >&2; exit 2; }
      root="$2"
      resume=true
      shift 2
      ;;
    *)
      forwarded+=("$1")
      shift
      ;;
  esac
done

[[ -n "$root" ]] || { echo "run_all.sh: pass --output DIR or --resume DIR" >&2; exit 2; }

cd "$repository"
for experiment in e2e figure3 figure6 figure7 figure8 figure14; do
  destination=(--output "$root/$experiment")
  if [[ "$resume" == true && -f "$root/$experiment/manifest.json" ]]; then
    destination=(--resume "$root/$experiment")
  fi
  "./artifact-eval/run_${experiment}.sh" "${forwarded[@]}" "${destination[@]}"
done
