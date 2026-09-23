#!/usr/bin/env bash
# Host-side tests: native build of the DSP, offline render + checks, headless UI run.
set -e
cd "$(dirname "$0")/.."
mkdir -p build
gcc -std=gnu11 -O3 -ffast-math -Wall -Wno-format-truncation -shared -fPIC src/dsp/sculpt.c -o build/sculpt_host.so -lm -lpthread
gcc -O2 tests/render_test.c -o build/render_test -ldl -lm
./build/render_test build/sculpt_host.so build/render_test.wav
gcc -O2 tests/filter_probe.c -o build/filter_probe -ldl -lm
./build/filter_probe build/sculpt_host.so
gcc -O2 tests/filter_thd.c -o build/filter_thd -ldl -lm 2>/dev/null
./build/filter_thd build/sculpt_host.so
if python3 -c "import scipy, matplotlib" 2>/dev/null; then python3 tests/filter_analyze.py; else echo "(scipy/matplotlib missing: skipping filter response checks)"; fi
if command -v node >/dev/null; then node tests/ui_harness.mjs; else echo "(node not found: skipping UI harness)"; fi
