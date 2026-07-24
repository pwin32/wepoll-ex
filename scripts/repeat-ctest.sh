#!/usr/bin/env sh
#
# Repeat a configured CTest selection until failure. This helper never
# searches outside the explicitly supplied build directory.
set -eu

if [ "$#" -lt 1 ] || [ "$#" -gt 3 ]; then
    echo "usage: $0 BUILD_DIR [REPEAT_COUNT [TEST_REGEX]]" >&2
    exit 2
fi

build_dir=$1
repeat_count=${2:-5}
test_regex=${3:-}

case $repeat_count in
    ''|*[!0-9]*|0)
        echo "repeat count must be a positive integer" >&2
        exit 2
        ;;
esac

if [ ! -f "$build_dir/CMakeCache.txt" ]; then
    echo "not a configured CMake build directory: $build_dir" >&2
    exit 2
fi

if [ -n "$test_regex" ]; then
    exec ctest --test-dir "$build_dir" --output-on-failure --repeat "until-fail:$repeat_count" -R "$test_regex"
fi

exec ctest --test-dir "$build_dir" --output-on-failure --repeat "until-fail:$repeat_count"
