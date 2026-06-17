#!/usr/bin/env bash
set -uo pipefail

usage() {
  cat <<'USAGE'
Usage: scripts/run-clang-tidy.sh [--gate] [build-dir]

Runs clang-tidy over qiftop-owned translation units from compile_commands.json.
Default build dir is ./build. Phase 0 is report-only: findings do not fail the
command unless --gate is supplied for a future stricter CI lane.
USAGE
}

gate=0
build_dir="build"

while [ "$#" -gt 0 ]; do
  case "$1" in
    --gate)
      gate=1
      shift
      ;;
    -h|--help)
      usage
      exit 0
      ;;
    --*)
      echo "error: unknown option: $1" >&2
      usage >&2
      exit 2
      ;;
    *)
      build_dir="$1"
      shift
      ;;
  esac
done

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
case "$build_dir" in
  /*) build_path="$build_dir" ;;
  *) build_path="$repo_root/$build_dir" ;;
esac
compile_db="$build_path/compile_commands.json"

if [ ! -f "$compile_db" ]; then
  cat >&2 <<EOF_HINT
error: $compile_db does not exist.

Configure first, for example:
  cmake -S . -B ${build_dir} -G Ninja -DQIFTOP_AUTO_PACKAGE=OFF

clang-tidy needs compile_commands.json so it can see the same include paths and
Qt defines as the real build.
EOF_HINT
  exit 2
fi

clang_tidy="${CLANG_TIDY:-clang-tidy}"
if ! command -v "$clang_tidy" >/dev/null 2>&1; then
  echo "error: clang-tidy not found (set CLANG_TIDY=/path/to/clang-tidy)." >&2
  exit 2
fi

source_regex="$(
  python3 - "$repo_root" <<'PY'
import pathlib
import re
import sys

root = pathlib.Path(sys.argv[1]).resolve().as_posix()
print(r'^(?!.*(_autogen|moc_|mocs_compilation|qrc_))' + re.escape(root) + r'/(src|bench)/.*\.(c|cc|cpp|cxx|c\+\+)$')
PY
)"
status=0

cd "$repo_root"

if command -v run-clang-tidy >/dev/null 2>&1 && run-clang-tidy -h 2>&1 | grep -q -- '-source-filter'; then
  cmd=(run-clang-tidy -p "$build_path" -clang-tidy-binary "$clang_tidy" -source-filter "$source_regex" -hide-progress)
  if [ "$gate" -eq 1 ]; then
    cmd+=(-warnings-as-errors '*')
  fi
  echo "==> ${cmd[*]}"
  "${cmd[@]}" || status=$?
else
  mapfile -t files < <(python3 - "$repo_root" "$compile_db" <<'PY'
import json
import pathlib
import sys

repo = pathlib.Path(sys.argv[1]).resolve()
compile_db = pathlib.Path(sys.argv[2])
seen = set()
with compile_db.open(encoding='utf-8') as f:
    for entry in json.load(f):
        raw = pathlib.Path(entry['file'])
        path = raw if raw.is_absolute() else pathlib.Path(entry.get('directory', repo)) / raw
        path = path.resolve()
        text = path.as_posix()
        try:
            rel = path.relative_to(repo)
        except ValueError:
            continue
        if rel.parts and rel.parts[0] in {'src', 'bench'} and not any(
            token in text for token in ('_autogen', 'moc_', 'mocs_compilation', 'qrc_')
        ) and path.suffix in {'.c', '.cc', '.cpp', '.cxx', '.c++'} and text not in seen:
            seen.add(text)
            print(text)
PY
  )
  if [ "${#files[@]}" -eq 0 ]; then
    echo "No qiftop source files matched in $compile_db" >&2
    exit 0
  fi
  echo "==> run-clang-tidy unavailable or too old for source filtering; falling back to a serial clang-tidy loop (${#files[@]} files)"
  for file in "${files[@]}"; do
    echo "==> $file"
    if [ "$gate" -eq 1 ]; then
      "$clang_tidy" -p "$build_path" --warnings-as-errors='*' "$file" || status=$?
    else
      "$clang_tidy" -p "$build_path" "$file" || status=$?
    fi
  done
fi

if [ "$gate" -eq 1 ]; then
  exit "$status"
fi

if [ "$status" -ne 0 ]; then
  echo "clang-tidy exited with status $status; keeping Phase 0 report-only (exit 0)." >&2
fi
exit 0
