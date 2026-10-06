# hft

L3 market-making lab. Architecture and plan: [DESIGN.md](DESIGN.md).

## Build

Requires CMake, Ninja, a C++23 compiler and `VCPKG_ROOT` pointing at a vcpkg checkout.

```
cmake --preset debug
cmake --build build/debug
ctest --test-dir build/debug --output-on-failure
```

Presets: `debug`, `release`, `asan`, `ubsan`, `tsan`, `fuzz`, `pgo-gen`, `pgo-use`.

## Data

Day files are listed in `configs/splits.toml`. Download, verify and ingest one day into the
per-symbol store with:

```
scripts/ingest_day.sh S121225-v50.txt.gz
```

`ingest <store-dir> <day.gz>` decompresses in-process, checks the gzip CRC and records the
SHA-256 of the download in `<store-dir>/source.sha256`. `store_cat <store-dir>` merges a store
back into the original stream, and `research/count_itch.c` is an independent message counter
used to cross-check the decoder.
