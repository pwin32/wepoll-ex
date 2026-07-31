#!/usr/bin/env sh
#
# MinGW qualification across linkage forms and socket-lifetime policies.
set -eu

case $(uname -s) in
    MINGW*|MSYS*) ;;
    *)
        echo "run with /path/to/msys64/usr/bin/bash.exe and /mingw64/bin first in PATH" >&2
        exit 2
        ;;
esac

PATH=/mingw64/bin:/usr/bin:$PATH
export PATH

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
repo_dir=$(CDPATH= cd -- "$script_dir/.." && pwd)
build_root=${1:-"$repo_dir/build/qualification-mingw"}
repeat_count=${WEPOLL_EX_REPEAT:-3}
jobs=${WEPOLL_EX_JOBS:-4}
variants=${WEPOLL_EX_MINGW_VARIANTS:-combined,static,shared,strict,strict-shared,synchronized,synchronized-shared}
strict_flags=${WEPOLL_EX_MINGW_C_FLAGS:--O2 -Wall -Wextra -Wpedantic -Werror}
run_benchmark=${WEPOLL_EX_RUN_WINDOWS_BENCH:-0}

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

variant_enabled()
{
    case ",$variants," in
        *,"$1",*) return 0 ;;
        *) return 1 ;;
    esac
}

configure_and_test()
{
    name=$1
    shared=$2
    static=$3
    lifetime_mode=$4
    build_dir=$build_root/$name

    cmake -S "$repo_dir" -B "$build_dir" -G "MinGW Makefiles" \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_C_FLAGS="$strict_flags" \
        -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
        -DWEPOLL_EX_SOCKET_LIFETIME_MODE="$lifetime_mode" \
        -DWEPOLL_EX_BUILD_SHARED="$shared" \
        -DWEPOLL_EX_BUILD_STATIC="$static" \
        -DWEPOLL_EX_BUILD_TESTS=ON \
        -DWEPOLL_EX_BUILD_BENCH=ON \
        -DWEPOLL_EX_FORCE_EPOLL_PWAIT2_FALLBACK=OFF
    cmake --build "$build_dir" --parallel "$jobs"
    ctest --test-dir "$build_dir" --output-on-failure
    "$script_dir/repeat-ctest.sh" "$build_dir" "$repeat_count" \
        'wepoll_ex_windows_(api|stress|backpressure|compat_concurrent_ctl|pwait2_(conversion|generation_deadline|readiness_wins|close|fallback)|pipe_.*|waitable_(zero_.*|timer_et|queued_rearm)|state_(waitable_(zero_(callback|ready)|queued_rearm)|udp_readless_(error|park|rollback)|large_wait)|fault_waitable_(ready_node_alloc|zero_disarm)|socket_events_(aliases|oob_(lt|et|oneshot|mod|inline_(lt|et))|udp_readless_data|udp_error_v6_(zero|err|out_et|readless_(et|oneshot)))|events_(mapping|status))$'
}

if variant_enabled combined; then
    configure_and_test combined ON ON best-effort
fi
if variant_enabled static; then
    configure_and_test static OFF ON best-effort
fi
if variant_enabled shared; then
    configure_and_test shared ON OFF best-effort
fi
if variant_enabled strict; then
    configure_and_test strict ON ON strict
fi
if variant_enabled strict-shared; then
    configure_and_test strict-shared ON OFF strict
fi
if variant_enabled synchronized; then
    configure_and_test synchronized ON ON synchronized
fi
if variant_enabled synchronized-shared; then
    configure_and_test synchronized-shared ON OFF synchronized
fi

if [ "$run_benchmark" = 1 ]; then
    benchmark_dir=$build_root/combined
    if [ ! -x "$benchmark_dir/bench/bench_windows.exe" ]; then
        echo "combined bench_windows.exe is not available" >&2
        exit 1
    fi
    if [ "${WEPOLL_EX_WINDOWS_BENCH_PRODUCTION:-0}" = 1 ]; then
        PATH="$benchmark_dir:$PATH" \
            "$benchmark_dir/bench/bench_windows.exe" --production
    else
        PATH="$benchmark_dir:$PATH" \
            "$benchmark_dir/bench/bench_windows.exe"
    fi
fi
