# hft

L3 market-making lab. Architecture and plan: [DESIGN.md](DESIGN.md).

## Build

Requires CMake, Ninja, a C++23 compiler and `VCPKG_ROOT` pointing at a vcpkg checkout.

```
cmake --preset debug
cmake --build build/debug
ctest --test-dir build/debug --output-on-failure
```

Presets: `debug`, `release`, `asan`, `ubsan`, `tsan`, `pgo-gen`, `pgo-use`.
