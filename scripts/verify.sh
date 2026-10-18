#!/usr/bin/env bash
set -euo pipefail
repo_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
build_dir=${TINYDB_BUILD_DIR:-"$repo_dir/build"}
cmake -S "$repo_dir" -B "$build_dir" -DCMAKE_BUILD_TYPE=Debug "$@"
cmake --build "$build_dir" --parallel 2
ctest --test-dir "$build_dir" --output-on-failure
