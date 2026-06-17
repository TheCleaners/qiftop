#!/usr/bin/env bash
set -uo pipefail

# Runs clang-tidy over the qiftop-owned translation units listed in
# compile_commands.json and (optionally) gates on findings.
#
# The gate counts ONLY diagnostics anchored in our own src/ or bench/ tree.
# We deliberately do NOT use clang-tidy's --warnings-as-errors: the static
# analyzer (clang-analyzer-*) loves to anchor a "leak" inside Qt's own headers
# (e.g. NewDeleteLeaks in qobjectdefs.h) for objects we never owned, and which
# checker fires there drifts between LLVM versions. Escalating those to errors
# made the gate a hostage to whatever clang-tidy the runner happens to ship.
# Counting only our-code primaries keeps the gate honest and version-stable.

usage() {
  cat <<'USAGE'
Usage: scripts/run-clang-tidy.sh [--gate] [build-dir]

Runs clang-tidy over qiftop's own translation units (src/ + bench/) using
<build-dir>/compile_commands.json. Default build dir is ./build.

  --gate   fail (exit 1) if any finding is anchored in qiftop source.
           Without it, this is report-only (always exit 0).

Set CLANG_TIDY=clang-tidy-NN to pin a specific clang-tidy binary.
USAGE
}

gate=0
build_dir="build"

while [ "$#" -gt 0 ]; do
  case "$1" in
    --gate) gate=1; shift ;;
    -h|--help) usage; exit 0 ;;
    --*) echo "error: unknown option: $1" >&2; usage >&2; exit 2 ;;
    *) build_dir="$1"; shift ;;
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

clang-tidy needs compile_commands.json so it sees the same include paths and
Qt defines as the real build.
EOF_HINT
  exit 2
fi

clang_tidy="${CLANG_TIDY:-clang-tidy}"
if ! command -v "$clang_tidy" >/dev/null 2>&1; then
  echo "error: clang-tidy not found (set CLANG_TIDY=/path/to/clang-tidy)." >&2
  exit 2
fi

# Explicit file list of our own TUs (passed to run-clang-tidy as positional
# args and used by the serial fallback). compile_commands.json is the source
# of truth for which files actually get built.
mapfile -t files < <(python3 - "$repo_root" "$compile_db" <<'PY'
import json, pathlib, sys
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

cd "$repo_root"

log="$(mktemp)"
trap 'rm -f "$log"' EXIT

# Pass the explicit qiftop file list to run-clang-tidy (its stable positional
# "file regex" interface), rather than -source-filter — the latter's matching
# semantics drift between LLVM versions (and silently analyzed only a handful
# of files in testing). Fall back to a serial loop if run-clang-tidy is absent.
if command -v run-clang-tidy >/dev/null 2>&1; then
  echo "==> run-clang-tidy over ${#files[@]} qiftop TUs (binary: $clang_tidy)"
  run-clang-tidy -p "$build_path" -clang-tidy-binary "$clang_tidy" -quiet \
    "${files[@]}" 2>&1 | tee "$log" || true
else
  echo "==> serial clang-tidy over ${#files[@]} qiftop TUs (binary: $clang_tidy)"
  for file in "${files[@]}"; do
    echo "==> $file"
    "$clang_tidy" -p "$build_path" "$file" 2>&1 | tee -a "$log" || true
  done
fi

# A "finding" is a primary diagnostic whose location is under THIS repo's
# src/ or bench/ tree. Qt/system-header diagnostics are ignored by design.
findings="$(grep -aE "${repo_root}/(src|bench)/[^:]*:[0-9]+:[0-9]+: (warning|error):" "$log" \
            | grep -avE '(_autogen|moc_|mocs_compilation|qrc_)' || true)"
count="$(printf '%s\n' "$findings" | grep -c . || true)"

if [ "$count" -gt 0 ]; then
  echo
  echo "clang-tidy: ${count} finding(s) in qiftop sources:"
  printf '%s\n' "$findings"
fi

if [ "$gate" -eq 1 ]; then
  if [ "$count" -gt 0 ]; then
    echo "clang-tidy gate: failing on ${count} qiftop-owned finding(s)." >&2
    exit 1
  fi
  echo "clang-tidy gate: clean — 0 qiftop-owned findings."
  exit 0
fi

echo "clang-tidy report-only: ${count} qiftop-owned finding(s) (not failing the build)."
exit 0
