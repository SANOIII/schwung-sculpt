import fs from "fs";
const bytes = fs.readFileSync(new URL("../build/sculpt.wasm", import.meta.url));
const wasi = { fd_close: () => 0, fd_seek: () => 0, fd_write: () => 0 };
const { instance } = await WebAssembly.instantiate(bytes, { wasi_snapshot_preview1: wasi });
const e = instance.exports;
const enc = new TextEncoder(), dec = new TextDecoder();
const mem = () => new Uint8Array(e.memory.buffer);
const putStr = (ptr, s) => { const b = enc.encode(s + "\0"); mem().set(b, ptr); };
const get = (k) => { putStr(e.web_key_ptr(), k); const n = e.web_get_param(); return n < 0 ? null : dec.decode(mem().slice(e.web_res_ptr(), e.web_res_ptr() + n)); };
const cmd = (c) => { putStr(e.web_key_ptr(), "cmd"); putStr(e.web_val_ptr(), c); e.web_set_param(); };
console.log("init", e.web_init());
for (let i = 0; i < 4; i++) e.web_render();
console.log("ui:", get("ui").split("\n")[0]);
// load 2 s of a chord into track 0
const frames = 88200, p = e.web_alloc_track();
const f = new Float32Array(e.memory.buffer, p, frames * 2);
for (let i = 0; i < frames; i++) { const t = i / 44100; const v = 0.3 * Math.sin(2 * Math.PI * 220 * t) * Math.exp(-(t % 0.5) * 6); f[i * 2] = v; f[i * 2 + 1] = v; }
putStr(e.web_key_ptr(), "chord.wav"); e.web_commit_track(0, p, frames); e.web_render();
cmd("playall 1\nk 0 gmix 0.5\nk 0 rmix 0.4\nk 0 gdens 1\nk 0 dmix 0.5\nk 0 fcut 0.5\nk 0 fdecay 0.6\nk 0 fscale 3");
let peak = 0; const t0 = performance.now();
for (let b = 0; b < 3445; b++) { e.web_render(); const o = new Int16Array(e.memory.buffer, e.web_out_ptr(), 256); for (const x of o) peak = Math.max(peak, Math.abs(x)); if (b % 8 === 0) e.web_poll(); }
const ms = performance.now() - t0;
console.log("10 s rendered in", ms.toFixed(0), "ms; peak", peak, "name0", get("name0"), "meta len", get("meta").length);
