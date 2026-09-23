/* Runs src/ui.js headless: stubbed display + DSP bridge fed from snapshots
 * captured by render_test. Renders screens to build/screens/*.pbm and checks
 * that key interactions emit the right DSP commands. */
import fs from "fs";
import path from "path";
import { fileURLToPath, pathToFileURL } from "url";
const here = path.dirname(fileURLToPath(import.meta.url));
const root = path.join(here, "..");
const font = JSON.parse(fs.readFileSync(path.join(here, "font5x7.json"), "utf8"));
const fb = new Uint8Array(128 * 64);
const px = (x, y, c) => { if (x >= 0 && x < 128 && y >= 0 && y < 64) fb[y * 128 + x] = c ? 1 : 0; };
globalThis.clear_screen = () => fb.fill(0);
globalThis.set_pixel = px;
globalThis.fill_rect = (x, y, w, h, c) => { for (let j = 0; j < h; j++) for (let i = 0; i < w; i++) px(x + i, y + j, c); };
globalThis.draw_rect = (x, y, w, h, c) => { for (let i = 0; i < w; i++) { px(x + i, y, c); px(x + i, y + h - 1, c); } for (let j = 0; j < h; j++) { px(x, y + j, c); px(x + w - 1, y + j, c); } };
globalThis.text_width = (s) => String(s).length * 6;
globalThis.print = (x, y, s, c) => { let cx = x; for (const ch of String(s)) { const g = font[ch]; if (g) g.forEach((row, j) => { for (let i = 0; i < 5; i++) if (row[i] === "#") px(cx + i, y + j, c); }); cx += 6; } };
globalThis.__leds = [];
const snap = (k) => { try { return fs.readFileSync(path.join(root, "build", "snap_" + k + ".txt"), "utf8"); } catch { return null; } };
const sent = [];
globalThis.host_module_get_param = (k) => {
  if (k === "meta") return fs.readFileSync(path.join(root, "build", "meta.json"), "utf8");
  if (k.startsWith("file:")) return "drums/kit " + k.slice(5) + ".wav";
  if (k.startsWith("wave")) return snap("wave0");
  if (k.startsWith("modv")) return snap("modv0");
  if (k.startsWith("bank")) return snap("bank0");
  return snap(k);
};
globalThis.host_module_set_param = (k, v) => { sent.push(v); return true; };

let src = fs.readFileSync(path.join(root, "src", "ui.js"), "utf8")
  .replace("/data/UserData/schwung/shared/input_filter.mjs", pathToFileURL(path.join(here, "stubs", "input_filter.mjs")).href);
const tmp = path.join(root, "build", "ui_under_test.mjs");
fs.writeFileSync(tmp, src);
await import(pathToFileURL(tmp).href);

fs.mkdirSync(path.join(root, "build", "screens"), { recursive: true });
const shot = (name) => {
  let s = "P1\n128 64\n";
  for (let y = 0; y < 64; y++) s += Array.from(fb.slice(y * 128, y * 128 + 128)).join(" ") + "\n";
  fs.writeFileSync(path.join(root, "build", "screens", name + ".pbm"), s);
};
const midi = (a, b, c) => globalThis.onMidiMessageInternal([a, b, c]);
const cc = (n, v) => midi(0xB0, n, v);
const note = (n, v) => midi(v ? 0x90 : 0x80, n, v);
let fails = 0;
const expect = (c, what) => { console.log(`  [${c ? "ok" : "FAIL"}] ${what}`); if (!c) fails++; };
const lastSent = () => sent.join("\n");
const META = JSON.parse(fs.readFileSync(path.join(root, "build", "meta.json"), "utf8"));
const PI = (k) => META.params.find((p) => p.k === k).i;

globalThis.init();
for (let i = 0; i < 3; i++) globalThis.tick();
shot("01_material");
expect(sent.some((s) => s.includes("scenepads 0")), "init sends scenepads 0");

note(1, 127); cc(72, 3); globalThis.tick(); shot("02_material_knob_touch");
expect(/p 0 1 0\.0/.test(lastSent()), "knob 2 turns track 1 Start");
note(1, 0);

cc(63, 127); globalThis.tick(); shot("03_material_page2");
note(17, 127); note(17, 0); globalThis.tick(); shot("04_granular");
cc(71, 5); globalThis.tick();
expect(new RegExp(`p 0 ${PI("gmix")} `).test(lastSent()), "knob 1 on granular page sets gmix");
note(18, 127); globalThis.tick(); shot("05_filter");
cc(71, 120); globalThis.tick();
expect(new RegExp(`p 0 ${PI("fcut")} `).test(lastSent()), "filter knob 1 = bank cutoff");
cc(63, 127); globalThis.tick(); shot("05b_filter_waves"); cc(62, 127); globalThis.tick();
cc(72, 125); globalThis.tick(); shot("06_filter_cutoff_focus");
note(19, 127); globalThis.tick(); shot("07_color");
note(20, 127); globalThis.tick(); shot("08_space");
note(21, 127); globalThis.tick(); shot("09_mod1");
cc(74, 1); cc(74, 1); globalThis.tick(); shot("10_mod1_target");
expect(new RegExp(`p 0 ${PI("m1tgt")} `).test(lastSent()), "mod target knob sets m1tgt");
note(22, 127); globalThis.tick(); shot("11_mix");
cc(71, 10); globalThis.tick();
expect(new RegExp(`p 0 ${PI("level")} `).test(lastSent()), "mix knob 1 = track 1 level");
cc(75, 10); globalThis.tick();
expect(new RegExp(`p 0 ${PI("djf")} `).test(lastSent()), "mix knob 5 = track 1 DJ filter");
note(23, 127); globalThis.tick(); shot("12_scenes");
expect(/scenepads 1/.test(lastSent()), "scene page tells DSP pads are scenes");
cc(49, 127); note(92, 100); cc(49, 0); globalThis.tick();
expect(/sstore 0/.test(lastSent()), "shift + top-left pad stores scene 1");
note(92, 0);
note(16, 127); globalThis.tick();
cc(14, 1); globalThis.tick(); shot("13_browse");
cc(14, 2); cc(3, 127); globalThis.tick();
expect(/load 0 \d/.test(lastSent()), "jog click loads the highlighted file into T1");
cc(42, 127); globalThis.tick(); shot("14_track2");
cc(86, 127); globalThis.tick();
expect(/rec 1/.test(lastSent()), "Rec records into selected track (T2)");
cc(49, 127); cc(43, 127); cc(49, 0); globalThis.tick();
expect(/play 0 /.test(lastSent()), "Shift+Track1 toggles T1 play");
note(28, 127); globalThis.tick(); expect(/punch 0 1/.test(lastSent()), "step 13 = stutter on"); note(28, 0); globalThis.tick();
expect(/punch 0 0/.test(lastSent()), "stutter released");
cc(50, 127); globalThis.tick(); shot("15_help"); cc(50, 127);
cc(52, 127); globalThis.tick(); shot("16_saved_toast");
expect(/save/.test(lastSent()), "Capture saves");
expect(globalThis.__leds.length > 0, "LEDs were driven");
const perTick = []; for (let i = 0; i < 5; i++) { globalThis.__leds = []; globalThis.tick(); perTick.push(globalThis.__leds.length); }
expect(perTick.every((n) => n <= 12), "LED sends rate-limited (<=12/tick): " + perTick.join(","));
globalThis.onUnload();
console.log(fails ? `\n${fails} UI FAILURES` : "\nUI ALL OK");
process.exit(fails ? 1 : 0);
