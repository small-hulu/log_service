#!/usr/bin/env bash
set -euo pipefail

project=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
if [[ $# -gt 0 ]]; then
  echo "Usage: bash build.sh (optional environment: BUILD_TYPE=Debug JOBS=2)" >&2
  exit 2
fi

cmake -S "$project" -B "$project/build" \
  -DCMAKE_BUILD_TYPE="${BUILD_TYPE:-Debug}" \
  -DCMAKE_PREFIX_PATH="$project/deps/zenoh"
cmake --build "$project/build" --parallel "${JOBS:-2}"
ctest --test-dir "$project/build" --output-on-failure
echo "Build and tests passed. Run: bash run.sh restart"
