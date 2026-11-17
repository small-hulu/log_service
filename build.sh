#!/usr/bin/env bash
set -euo pipefail

project=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
if [[ $# -gt 0 ]]; then
  echo "Usage: bash build.sh (optional environment: BUILD_TYPE=Debug JOBS=2 BUILD_DIR=path)" >&2
  exit 2
fi

build_dir="${BUILD_DIR:-$project/build}"
flatbuffers="$project/deps/flatbuffers"
if [[ ! -x "$flatbuffers/bin/flatc" ]]; then
  echo "Missing FlatBuffers 25.12.19. Run: python3 tools/fetch_flatbuffers.py" >&2
  exit 1
fi
cmake -S "$project" -B "$build_dir" \
  -DCMAKE_BUILD_TYPE="${BUILD_TYPE:-Debug}" \
  -DCMAKE_PREFIX_PATH="$project/deps/zenoh" \
  -DFLATC="$flatbuffers/bin/flatc" \
  -DFLATBUFFERS_INCLUDE_DIR="$flatbuffers/include"
cmake --build "$build_dir" --parallel "${JOBS:-2}"
ctest --test-dir "$build_dir" --output-on-failure
echo "Build and tests passed. Run: bash run.sh restart"
