#!/usr/bin/env bash
set -euo pipefail

iterations=10000
runs=5
port=45443
output_dir=""

usage() {
    echo "Usage: $0 [--iterations N] [--runs N] [--port PORT] [--output DIR]" >&2
}

while (($# > 0)); do
    case "$1" in
        --iterations)
            iterations=${2:?missing value for --iterations}
            shift 2
            ;;
        --runs)
            runs=${2:?missing value for --runs}
            shift 2
            ;;
        --port)
            port=${2:?missing value for --port}
            shift 2
            ;;
        --output)
            output_dir=${2:?missing value for --output}
            shift 2
            ;;
        --help|-h)
            usage
            exit 0
            ;;
        *)
            usage
            exit 2
            ;;
    esac
done

for value in "$iterations" "$runs" "$port"; do
    if [[ ! "$value" =~ ^[1-9][0-9]*$ ]]; then
        usage
        exit 2
    fi
done

repo_root=$(git rev-parse --show-toplevel)
if [[ -z "$output_dir" ]]; then
    output_dir="/tmp/libwtf-benchmark-results-$(date +%Y%m%d-%H%M%S)"
fi
mkdir -p "$output_dir"

build_root=$(mktemp -d /tmp/libwtf-branch-benchmark.XXXXXX)
cleanup() {
    rm -rf -- "$build_root"
}
trap cleanup EXIT

main_source="$build_root/main-source"
main_build="$build_root/main-build"
refactor_build="$build_root/refactor-build"
cert_dir="$build_root/certs"
mkdir -p "$main_source"

echo "[benchmark] Exporting main..." >&2
git -C "$repo_root" archive main | tar -x -C "$main_source"
mkdir -p "$main_source/msquic"
cp -a "$repo_root/msquic/." "$main_source/msquic/"
rm -f -- "$main_source/msquic/.git"

echo "[benchmark] Generating temporary certificate..." >&2
"$repo_root/tools/certgen.sh" --output "$cert_dir" >/dev/null

configure_library() {
    local source_dir=$1
    local build_dir=$2
    cmake -S "$source_dir" -B "$build_dir" -G Ninja \
        -DCMAKE_BUILD_TYPE=Release \
        -DWTF_BUILD_TESTS=OFF \
        -DWTF_BUILD_SAMPLES=OFF \
        -DWTF_APPLY_BUNDLED_MSQUIC_PATCHES=OFF >/dev/null
    cmake --build "$build_dir" --target wtf -j2 >/dev/null
}

echo "[benchmark] Building main..." >&2
configure_library "$main_source" "$main_build"
echo "[benchmark] Building refactor working tree..." >&2
configure_library "$repo_root" "$refactor_build"

compile_benchmark() {
    local include_dir=$1
    local build_dir=$2
    local output=$3
    "${CC:-cc}" -O3 -DNDEBUG -std=gnu11 -I"$include_dir" \
        "$repo_root/benchmarks/wtf_benchmark.c" \
        -L"$build_dir/output" -lwtf \
        -Wl,-rpath,"$build_dir/output" -o "$output"
}

main_executable="$build_root/wtf_benchmark_main"
refactor_executable="$build_root/wtf_benchmark_refactor"
compile_benchmark "$main_source/include" "$main_build" "$main_executable"
compile_benchmark "$repo_root/include" "$refactor_build" "$refactor_executable"

main_results="$output_dir/main.csv"
refactor_results="$output_dir/refactor.csv"

echo "[benchmark] Running main..." >&2
LD_LIBRARY_PATH="$main_build/output" "$main_executable" \
    --cert-dir "$cert_dir" --label main --port "$port" \
    --iterations "$iterations" --runs "$runs" >"$main_results"

echo "[benchmark] Running refactor..." >&2
LD_LIBRARY_PATH="$refactor_build/output" "$refactor_executable" \
    --cert-dir "$cert_dir" --label refactor --port "$port" \
    --iterations "$iterations" --runs "$runs" >"$refactor_results"

awk -f "$repo_root/benchmarks/compare_results.awk" \
    "$main_results" "$refactor_results" | tee "$output_dir/summary.txt"
echo "[benchmark] Raw results: $output_dir" >&2
