#!/usr/bin/env sh
#
# Strict Linux release and sanitizer qualification. Build products stay
# below the selected build root; no repository-wide searches are performed.
set -eu

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
repo_dir=$(CDPATH= cd -- "$script_dir/.." && pwd)
build_root=${1:-"$repo_dir/build/qualification-posix"}
repeat_count=${WEPOLL_EX_REPEAT:-5}
jobs=${WEPOLL_EX_JOBS:-4}
generator=${CMAKE_GENERATOR:-Unix Makefiles}
strict_flags=${WEPOLL_EX_POSIX_C_FLAGS:--O3 -Wall -Wextra -Wpedantic -Werror}
run_sanitizers=${WEPOLL_EX_RUN_SANITIZERS:-1}
run_benchmarks=${WEPOLL_EX_RUN_BENCHMARKS:-0}

case $(uname -s) in
    Linux) ;;
    *)
        echo "qualify-posix.sh requires Linux epoll" >&2
        exit 2
        ;;
esac
case $repeat_count in
    ''|*[!0-9]*|0)
        echo "WEPOLL_EX_REPEAT must be a positive integer" >&2
        exit 2
        ;;
esac
case $jobs in
    ''|*[!0-9]*|0)
        echo "WEPOLL_EX_JOBS must be a positive integer" >&2
        exit 2
        ;;
esac

cmake -E make_directory "$build_root"

release_dir=$build_root/release
cmake -S "$repo_dir" -B "$release_dir" -G "$generator" \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_C_FLAGS="$strict_flags" \
    -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
    -DWEPOLL_EX_BUILD_SHARED=ON \
    -DWEPOLL_EX_BUILD_STATIC=ON \
    -DWEPOLL_EX_BUILD_TESTS=ON \
    -DWEPOLL_EX_BUILD_BENCH=ON
cmake --build "$release_dir" --parallel "$jobs"
ctest --test-dir "$release_dir" --output-on-failure
"$script_dir/repeat-ctest.sh" "$release_dir" "$repeat_count" 'wepoll_ex_(api|pool_mpsc)$'

if [ "$run_sanitizers" = 1 ]; then
    sanitizer_dir=$build_root/asan-ubsan
    sanitizer_flags="-O1 -g -Wall -Wextra -Wpedantic -Werror -fno-omit-frame-pointer -fsanitize=address,undefined"
    cmake -S "$repo_dir" -B "$sanitizer_dir" -G "$generator" \
        -DCMAKE_BUILD_TYPE=Debug \
        -DCMAKE_C_FLAGS="$sanitizer_flags" \
        -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=address,undefined" \
        -DCMAKE_SHARED_LINKER_FLAGS="-fsanitize=address,undefined" \
        -DWEPOLL_EX_BUILD_SHARED=OFF \
        -DWEPOLL_EX_BUILD_STATIC=ON \
        -DWEPOLL_EX_BUILD_TESTS=ON \
        -DWEPOLL_EX_BUILD_BENCH=OFF
    cmake --build "$sanitizer_dir" --parallel "$jobs"
    # Some environments (notably WSL with unrelated LD_PRELOAD hooks) put
    # libraries ahead of libasan.  Prepend the compiler's ASan runtime so
    # the initial library list is valid without disabling those hooks.
    asan_runtime=$(${CC:-cc} -print-file-name=libasan.so 2>/dev/null || true)
    sanitizer_preload=
    case $asan_runtime in
        ''|libasan.so) ;;
        /*)
            case :${LD_PRELOAD:-}: in
                *:"$asan_runtime":*) sanitizer_preload=${LD_PRELOAD:-} ;;
                *)
                    if [ -n "${LD_PRELOAD:-}" ]; then
                        sanitizer_preload="$asan_runtime:$LD_PRELOAD"
                    else
                        sanitizer_preload=$asan_runtime
                    fi
                    ;;
            esac
            ;;
    esac
    ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 \
    UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
    LD_PRELOAD="$sanitizer_preload" \
        ctest --test-dir "$sanitizer_dir" --output-on-failure
fi

if [ "$run_benchmarks" = 1 ]; then
    "$release_dir/bench/bench_latency" "${WEPOLL_EX_LATENCY_ITERATIONS:-200000}"
    "$release_dir/bench/bench_wait_scaling" "${WEPOLL_EX_SCALING_FDS:-1024}" "${WEPOLL_EX_SCALING_ITERATIONS:-20000}"
fi
