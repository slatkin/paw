# Agent checks for this repository. Run `just` to list the recipes.
# Build folders live under bld/, which git ignores.

set shell := ["bash", "-euo", "pipefail", "-c"]

# This clang-tidy enables no checks by default. These cover null, memory, and errno bugs with little noise.
tidy_checks := "clang-analyzer-core.*,clang-analyzer-unix.*"

default:
    @just --list

# Configure the debug build on first use, then build.
build:
    [ -f bld/build.ninja ] || meson setup bld --buildtype=debug
    ninja -C bld

# Run the meson test suite.
test: build
    meson test -C bld --print-errorlogs

# Build and test with AddressSanitizer and UndefinedBehaviorSanitizer.
asan:
    [ -f bld/asan/build.ninja ] || meson setup bld/asan --buildtype=debug -Db_sanitize=address,undefined
    ninja -C bld/asan
    meson test -C bld/asan --print-errorlogs

# Run the test suite under valgrind (needs valgrind installed).
memcheck: build
    meson test -C bld --print-errorlogs --wrapper='valgrind --error-exitcode=1 --leak-check=full'

# Run clang-tidy on the named files, or on changed C files when none are named.
tidy *files: build
    #!/usr/bin/env bash
    set -euo pipefail
    targets=({{files}})
    if [ "${#targets[@]}" -eq 0 ]; then
        mapfile -t targets < <(git ls-files -m -o --exclude-standard -- '*.c')
    fi
    if [ "${#targets[@]}" -eq 0 ]; then
        echo "no C files to check"
        exit 0
    fi
    clang-tidy -p bld --quiet --checks='{{tidy_checks}}' "${targets[@]}"

# Check spelling with the repository codespell configuration.
spell:
    codespell

# Run every check in order.
check: build test asan tidy spell
    @echo "all checks passed"
