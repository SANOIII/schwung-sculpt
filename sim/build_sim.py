#!/usr/bin/env python3
"""Build the browser simulator: sim/template.html + build/sculpt.wasm + src/ui.js -> dist/sculpt-sim.html

The wasm is built from the same src/dsp/sculpt.c with -DSCULPT_WEB:
  clang --target=wasm32-wasi --sysroot=$WASI_SYSROOT -O3 -ffast-math -DSCULPT_WEB \
        -nostartfiles -Wl,--no-entry -Wl,--initial-memory=134217728 -Wl,--max-memory=268435456 \
        -Isrc/dsp src/dsp/sculpt.c -o build/sculpt.wasm -lm
(get a sysroot from https://github.com/WebAssembly/wasi-sdk/releases)
"""
import base64, os, re, sys
root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
p = lambda *a: os.path.join(root, *a)
tpl = open(p("sim", "template.html"), encoding="utf-8").read()
wasm = base64.b64encode(open(p("build", "sculpt.wasm"), "rb").read()).decode()
font = open(p("sim", "font.json"), encoding="utf-8").read()
ui = open(p("src", "ui.js"), encoding="utf-8").read()
ui, n = re.subn(r"import\s*\{([^}]*)\}\s*from\s*\n?\s*'/data/UserData/schwung/shared/input_filter\.mjs';",
                r"const {\1} = globalThis.__shim;", ui)
assert n == 1, "ui.js import line not found"
assert "</script" not in ui
out = tpl.replace("__WASM_B64__", wasm).replace("__FONT_JSON__", font).replace("__UI_JS__", ui)
os.makedirs(p("dist"), exist_ok=True)
open(p("dist", "sculpt-sim.html"), "w", encoding="utf-8").write(out)
print("dist/sculpt-sim.html", len(out) // 1024, "KB")
