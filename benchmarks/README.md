# libwtf branch benchmark

`compare_branches.sh` builds the committed `main` tree and the current working tree in
Release mode, then runs the same loopback WebTransport workload against both libraries.
Connection and stream setup are excluded from the timed regions.

```sh
./benchmarks/compare_branches.sh --iterations 10000 --runs 5
```

The summary reports aggregate operations per second and MiB per second across all runs
for copied stream and datagram payloads. Raw samples are saved as CSV files in the output
directory printed by the script. Use `--output DIR` to select a persistent location.

To build only the benchmark executable against the current tree:

```sh
cmake -S . -B build -DWTF_BUILD_BENCHMARKS=ON -DCMAKE_BUILD_TYPE=Release
cmake --build build --target wtf_benchmark
./build/benchmarks/wtf_benchmark --cert-dir certs
```
