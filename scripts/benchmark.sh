#!/usr/bin/env bash
set -euo pipefail
if [[ $# -lt 1 ]]; then
  echo "Usage: scripts/benchmark.sh NEW_RESULT_DIRECTORY [tinydb_bench options]" >&2
  exit 2
fi
repo_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
result_dir=$1
shift
mkdir -- "$result_dir" # Refuse existing results; never remove user files.
result_dir=$(cd -- "$result_dir" && pwd)
build_dir=${TINYDB_BENCH_BUILD_DIR:-"$repo_dir/build-release"}
cmake -S "$repo_dir" -B "$build_dir" -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=OFF -DTINYDB_SANITIZERS=OFF > "$result_dir/build.log" 2>&1
cmake --build "$build_dir" --parallel 2 >> "$result_dir/build.log" 2>&1
{
  date -u '+UTC: %Y-%m-%dT%H:%M:%SZ'
  uname -a
  compiler=$(sed -n 's/^CMAKE_CXX_COMPILER:FILEPATH=//p' "$build_dir/CMakeCache.txt")
  "$compiler" --version
  sed -n '/^CMAKE_BUILD_TYPE:/p; /^CMAKE_CXX_FLAGS[^:]*:/p; /^TINYDB_SANITIZERS:/p' "$build_dir/CMakeCache.txt"
  cmake --version
  lscpu
  df -T -- "$result_dir"
  cat /proc/meminfo
  printf 'Command: tinydb_bench --dir RUN_DIRECTORY'
  printf ' %q' "$@"
  printf '\nThree fresh-process runs; no OS cache drop; Release, sanitizers OFF.\n'
} > "$result_dir/environment.txt"
for run in 1 2 3; do
  "$build_dir/tinydb_bench" --dir "$result_dir/run-$run" "$@" > "$result_dir/run-$run.csv"
done
python3 "$repo_dir/scripts/summarize_benchmarks.py" "$result_dir" > "$result_dir/summary.md"
cat "$result_dir/summary.md"
