# sysinfo

Command line tool that prints the specifications of the machine it runs on:
operating system, CPU, memory, storage, graphics, network, sensors, user
environment and build toolchain. Text, JSON or `key=value` output.

Linux (Ubuntu 24.04 LTS and newer) is the primary platform. Only `/proc`, `/sys`
and a few system calls are used: no root, no external programs, no network.
macOS and Windows get a reduced set of sections (best effort).

## Build

Needs a C++20 compiler (GCC 11+/Clang 14+) and CMake 3.20+.

```sh
sudo apt install build-essential cmake
cmake -S . -B build
cmake --build build -j
ctest --test-dir build        # optional
./build/sysinfo
```

## Use

```sh
sysinfo                        # everything, human readable
sysinfo -s cpu,memory          # selected sections (see: sysinfo --list)
sysinfo -f json -o hw.json     # JSON to a file
sysinfo -r -v > report.txt     # verbose, hostname/user/MAC/IP removed
sysinfo --help
```

## Manual

`doc/sysinfo-manual.tex` (PDF: `cmake --build build --target manual`, needs
`latexmk`; a prebuilt `sysinfo-manual.pdf` is shipped next to it).

## Files

| File | Purpose |
|---|---|
| `main.cpp` | command line tool: options, section selection, text/JSON/kv output |
| `sysinfo.h`, `sysinfo.cpp` | collector library (`sysinfo::collectAll()`, `sysinfo::collect()`) |
| `build_info.h.in` | template for the generated build type / flags header |
| `CMakeLists.txt` | build, tests, install, `manual` target |
| `doc/` | LaTeX user manual and example outputs |

The library can be reused: `add_subdirectory(sysinfo)` and link `sysinfo::sysinfo`.
