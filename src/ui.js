/*
 * Sculpt — overtake UI for the four-track sculpting sampler.
 *
 * Control map (see help.json / README for the full table)
 *   Track buttons ........ select track      Shift+Track: play/stop track
 *                                            Mute+Track: mute   Delete+Track: clear
 *                                            Copy+Track: copy selected track's settings there
 *   Steps 1-8 ............ Material Granular Filter Color Space Mod Mix Scenes
 *   Steps 9-12 ........... mute tracks 1-4
 *   Steps 13-16 (hold) ... Stutter, Tape stop, Reverse, Freeze
 *   Left / Right ......... sub-page (Material 2, Granular 2, Space 2, Mod 1-4, Mix 2)
 *   Up / Down ............ previous / next track
 *   Knobs 1-8 ............ parameters   (Shift = fine, Delete+touch = reset)
 *   Jog .................. Material: browse samples, click = load
 *   Pads ................. one row per track (top = T1): cue slices / play voices
 *                          Scenes page: tap = recall, Shift = store, Delete = clear
 *   Play ................. start / stop all tracks
 *   Rec .................. record into selected track (first pass sets loop length)
 *   Capture .............. save session (settings + new recordings)
 *   Menu ................. help
 *   Back ................. leave Sculpt running in the background (Shift+Back quits)
 */

import { setLED, setButtonLED, decodeDelta, invalidateLedCache } from
    '/data/UserData/schwung/shared/input_filter.mjs';

/* ------------------------------------------------------------------ */
/* Hardware constants                                                  */
/* ------------------------------------------------------------------ */

const CC_JOG_CLICK = 3, CC_JOG = 14;
const CC_SHIFT = 49, CC_MENU = 50, CC_BACK = 51, CC_CAPTURE = 52;
const CC_DOWN = 54, CC_UP = 55, CC_UNDO = 56, CC_LOOP = 58, CC_COPY = 60;
const CC_LEFT = 62, CC_RIGHT = 63, CC_PLAY = 85, CC_REC = 86, CC_MUTE = 88;
const CC_SAMPLE = 118, CC_DELETE = 119;
const CC_KNOB1 = 71;
const TRACK_CC = [43, 42, 41, 40];            /* track 1..4 */

const DEV_NAMES = ["MATERIAL", "GRANULAR", "FILTER", "COLOR", "SPACE", "MOD", "MIX", "SCENES"];
const DEV_SHORT = ["MAT", "GRN", "FLT", "COL", "SPC", "MOD", "MIX", "SCN"];
const PAGE_MIX = 6, PAGE_SCENE = 7, PAGE_MOD = 5;

/* palette (see shared/constants.mjs) */
const BLACK = 0, WHITE = 120, DARKGREY = 124, GREY = 118;
const TRACK_COL = [
    { hi: 16, mid: 95, lo: 96 },    /* azure   */
    { hi: 3,  mid: 69, lo: 70 },    /* orange  */
    { hi: 11, mid: 85, lo: 86 },    /* green   */
    { hi: 26, mid: 115, lo: 116 },  /* magenta */
];
const RED = 1, RED_DIM = 65, RED_DARK = 66, GREEN = 11, GREEN_DIM = 85;
const W_OFF = 0, W_DIM = 0x10, W_MED = 0x40, W_BRIGHT = 0x7c;

const FMT = { PCT: 0, BIP: 1, SEMI: 2, FREQ: 3, TIME: 4, ENUM: 5, CENT: 6, RATE: 7, TARGET: 8, DTIME: 9, DENS: 10, BITS: 11,
              MORPH: 12, LFO: 13, SLOPE: 14, DECAY: 15 };
const PAGE_FLT = 2;

/* ------------------------------------------------------------------ */
/* State                                                               */
/* ------------------------------------------------------------------ */

let meta = null;             /* {params, globals, targets, ddiv, mdiv, gdiv} */
let pidx = {};               /* key -> index */
let gv = [];                 /* global values */
let tv = [[], [], [], []];   /* track values */
let subCount = [];           /* sub pages per device page */

let track = 0, page = 0, sub = [0, 0, 0, 0, 0, 0, 0, 0];
let shift = false, delHeld = false, copyHeld = false, muteHeld = false;
let knobAcc = [0, 0, 0, 0, 0, 0, 0, 0];
let focus = null;            /* {slot, until} */
let toast = null;            /* {text, until} */
let browse = null;           /* {idx, count, names:{}, until} */
let help = null;             /* {line} */
let heldPads = new Set();
let punchHeld = [false, false, false, false];

let status = { ready: 0, bpm: 120, scenes: 0, cur: -1, punch: 0, nfiles: 0, namever: -1, morphing: 0,
               tracks: [0, 1, 2, 3].map(() => ({ play: 0, rec: 0, len: 0, pos: 0, voices: 0, peak: 0, wave: -1, dirty: 0 })) };
let waves = ["", "", "", ""], waveVer = [-2, -2, -2, -2], names = ["", "", "", ""];
let modVals = [0, 0, 0, 0];

let queue = new Map();       /* dedup key -> command line */
let seq = 0;
let lastValuesFetch = 0, valuesStale = true;
let lastAutosave = Date.now(), changedSinceSave = false;
let tickCount = 0;
let ledWant = new Map(), ledSent = new Map();

/* ------------------------------------------------------------------ */
/* DSP bridge                                                          */
/* ------------------------------------------------------------------ */

function dspGet(key) {
    if (typeof host_module_get_param !== "function") return null;
    const v = host_module_get_param(key);
    return (v === null || v === undefined) ? null : String(v);
}

function send(line, dedupKey) {
    queue.set(dedupKey || ("#" + (seq++)), line);
}

function flush() {
    if (queue.size === 0) return true;
    if (typeof host_module_set_param !== "function") { queue.clear(); return true; }
    const text = Array.from(queue.values()).join("\n");
    const ok = host_module_set_param("cmd", text);
    if (ok !== false) { queue.clear(); return true; }
    return false;   /* channel busy: retry next tick */
}

function flushBlocking() {
    if (queue.size === 0) return;
    const text = Array.from(queue.values()).join("\n");
    if (typeof host_module_set_param_blocking === "function") host_module_set_param_blocking("cmd", text, 800);
    else if (typeof host_module_set_param === "function") host_module_set_param("cmd", text);
    queue.clear();
}

function loadMeta() {
    const m = dspGet("meta");
    if (!m) return false;
    try { meta = JSON.parse(m); } catch (e) { return false; }
    pidx = {};
    meta.params.forEach((p) => { pidx[p.k] = p.i; });
    meta.ddivL = meta.ddiv.split("|"); meta.mdivL = meta.mdiv.split("|"); meta.gdivL = meta.gdiv.split("|");
    for (let d = 0; d < 6; d++) {
        let n = 1;
        meta.params.forEach((p) => { if (p.d === d && p.pg + 1 > n) n = p.pg + 1; });
        subCount[d] = n;
    }
    subCount[PAGE_MIX] = 2; subCount[PAGE_SCENE] = 1;
    return true;
}

function fetchValues() {
    if (queue.size > 0) return;
    const v = dspGet("values");
    if (!v) return;
    const parts = v.split(";");
    if (parts.length < 5) return;
    gv = parts[0].split(",").map(Number);
    for (let t = 0; t < 4; t++) tv[t] = parts[t + 1].split(",").map(Number);
    lastValuesFetch = Date.now();
    valuesStale = false;
}

function pollStatus() {
    const s = dspGet("ui");
    if (!s) return;
    const lines = s.split("\n");
    const g = lines[0].split(" ").map(Number);
    status.ready = g[0]; status.bpm = g[1]; status.scenes = g[2] >>> 0; status.cur = g[3];
    status.punch = g[4]; status.nfiles = g[5];
    const prevMorph = status.morphing; status.morphing = g[7];
    if (prevMorph && !status.morphing) valuesStale = true;
    if (g[6] !== status.namever) {
        status.namever = g[6];
        for (let t = 0; t < 4; t++) names[t] = dspGet("name" + t) || "";
    }
    for (let t = 0; t < 4; t++) {
        const f = (lines[t + 1] || "").split(" ").map(Number);
        const tr = status.tracks[t];
        const wasRec = tr.rec;
        tr.play = f[0]; tr.rec = f[1]; tr.len = f[2]; tr.pos = f[3]; tr.voices = f[4];
        tr.peak = f[5]; tr.wave = f[6]; tr.dirty = f[7];
        if (wasRec === 1 && tr.rec !== 1) valuesStale = true;   /* start/len reset */
        if (tr.wave !== waveVer[t]) {
            const w = dspGet("wave" + t);
            if (w) { waves[t] = w; waveVer[t] = tr.wave; }
        }
    }
}

/* ------------------------------------------------------------------ */
/* Param helpers                                                       */
/* ------------------------------------------------------------------ */

/* A knob slot resolves to {kind:'t', t, i} (track param) or {kind:'g', i}. */
function slotFor(k) {
    if (!meta) return null;
    if (page < PAGE_MIX) {
        const s = sub[page];
        const p = meta.params.find((q) => q.d === page && q.pg === s && q.sl === k);
        return p ? { kind: "t", t: track, i: p.i } : null;
    }
    if (page === PAGE_MIX) {
        if (sub[PAGE_MIX] === 0) return k < 4 ? { kind: "t", t: k, i: pidx.level } : { kind: "t", t: k - 4, i: pidx.djf };
        return k < 4 ? { kind: "t", t: k, i: pidx.pan } : { kind: "g", i: k - 4 };
    }
    if (page === PAGE_SCENE) return k === 0 ? { kind: "g", i: 1 } : null;
    return null;
}

function defOf(sl) { return sl.kind === "t" ? meta.params[sl.i] : meta.globals[sl.i]; }
function valOf(sl) { return sl.kind === "t" ? (tv[sl.t][sl.i] ?? defOf(sl).df) : (gv[sl.i] ?? defOf(sl).df); }

function setVal(sl, v) {
    const d = defOf(sl);
    v = Math.max(d.mn, Math.min(d.mx, v));
    if (sl.kind === "t") { tv[sl.t][sl.i] = v; send(`p ${sl.t} ${sl.i} ${v.toFixed(5)}`, `p${sl.t}_${sl.i}`); }
    else { gv[sl.i] = v; send(`g ${sl.i} ${v.toFixed(5)}`, `g${sl.i}`); }
    changedSinceSave = true;
}

function expMap(x, lo, hi) { return lo * Math.pow(hi / lo, x); }
function fmtHz(hz) { return hz < 1000 ? Math.round(hz) + "Hz" : (hz / 1000).toFixed(hz < 10000 ? 1 : 0) + "k"; }
function fmtMs(ms) { return ms < 1000 ? Math.round(ms) + "ms" : (ms / 1000).toFixed(ms < 10000 ? 2 : 1) + "s"; }

function trackVal(t, key) { const i = pidx[key]; return tv[t][i] ?? meta.params[i].df; }

function targetName(v) {
    v = Math.round(v);
    if (v <= 0 || !meta || v > meta.targets.length) return "None";
    const p = meta.params[meta.targets[v - 1]];
    return DEV_SHORT[p.d] + " " + p.n;
}

function formatVal(sl, v) {
    const d = defOf(sl);
    const t = sl.kind === "t" ? sl.t : 0;
    switch (d.f) {
        case FMT.PCT: return Math.round(v * 100) + "%";
        case FMT.BIP: { const r = Math.round(v * 100); return (r > 0 ? "+" : "") + r; }
        case FMT.SEMI: { const r = Math.round(v); return (r > 0 ? "+" : "") + r + "st"; }
        case FMT.FREQ: return fmtHz(expMap(v, d.lo, d.hi));
        case FMT.TIME: return (d.k === "gmorph" && v <= 0.001) ? "Off" : fmtMs(expMap(v, d.lo, d.hi));
        case FMT.ENUM: { const l = d.e.split("|"); return l[Math.max(0, Math.min(l.length - 1, Math.round(v)))]; }
        case FMT.CENT: { const r = Math.round(v); return (r > 0 ? "+" : "") + r + "c"; }
        case FMT.RATE: {
            const m = Math.floor((d.i - pidx.m1rate) / 8);
            if (tv[t][pidx.m1sync + m * 8] >= 0.5) return meta.mdivL[Math.min(9, Math.floor(v * 9.999))];
            const hz = expMap(v, d.lo, d.hi);
            return hz < 1 ? hz.toFixed(2) + "Hz" : hz.toFixed(1) + "Hz";
        }
        case FMT.TARGET: return targetName(v);
        case FMT.DTIME:
            if (trackVal(t, "dsync") >= 0.5) return meta.ddivL[Math.min(11, Math.floor(v * 11.999))];
            return fmtMs(expMap(v, d.lo, d.hi));
        case FMT.DENS:
            if (trackVal(t, "gsync") >= 0.5) return meta.gdivL[Math.min(6, Math.floor(v * 6.999))];
            return expMap(v, d.lo, d.hi).toFixed(1) + "/s";
        case FMT.BITS: return v <= 0.001 ? "Off" : (16 - v * 14).toFixed(1) + "b";
        case FMT.MORPH:
            if (v < 0.03) return "LP";
            if (v > 0.47 && v < 0.53) return "BP";
            if (v > 0.97) return "HP";
            return v < 0.5 ? "LP>BP" : "BP>HP";
        case FMT.LFO: { const hz = expMap(v, d.lo, d.hi); return (hz < 1 ? hz.toFixed(2) : hz.toFixed(1)) + "Hz"; }
        case FMT.SLOPE: return Math.round((1 + v * 7) * 6) + "dB";
        case FMT.DECAY: return v <= 0.001 ? "Off" : fmtMs(expMap(v, d.lo, d.hi));
    }
    return String(v);
}

function labelFor(sl) {
    const d = defOf(sl);
    if (page === PAGE_MIX && sl.kind === "t") return d.s + (sl.t + 1);
    return d.s;
}

function longLabel(sl) {
    const d = defOf(sl);
    if (page === PAGE_MIX && sl.kind === "t") return "T" + (sl.t + 1) + " " + d.n;
    return d.n;
}

/* ------------------------------------------------------------------ */
/* Input                                                               */
/* ------------------------------------------------------------------ */

function turnKnob(k, raw) {
    const sl = slotFor(k);
    if (!sl) return;
    const d = defOf(sl);
    const delta = decodeDelta(raw);
    if (delta === 0) return;
    focus = { slot: k, until: Date.now() + 1500 };
    if (d.st || d.f === FMT.ENUM || d.f === FMT.TARGET) {
        knobAcc[k] += delta;
        const need = d.f === FMT.ENUM ? 3 : 1;
        if (Math.abs(knobAcc[k]) < need) return;
        const steps = Math.trunc(knobAcc[k] / need);
        knobAcc[k] -= steps * need;
        setVal(sl, Math.round(valOf(sl)) + steps);
        return;
    }
    const range = d.mx - d.mn;
    const stepSize = range / (shift ? 1000 : 150);
    setVal(sl, valOf(sl) + delta * stepSize);
}

function selectTrack(t) {
    track = Math.max(0, Math.min(3, t));
    toastMsg("Track " + (track + 1) + "  " + (names[track] || ""));
}

function toastMsg(text, ms) { toast = { text, until: Date.now() + (ms || 1200) }; }

function setPage(p) {
    if (p === page) { sub[p] = (sub[p] + 1) % Math.max(1, subCount[p] || 1); }
    const wasScene = page === PAGE_SCENE;
    page = p;
    browse = null;
    focus = null;
    if ((page === PAGE_SCENE) !== wasScene) send("scenepads " + (page === PAGE_SCENE ? 1 : 0), "scenepads");
}

function onTrackButton(t) {
    if (delHeld) { send("clear " + t); toastMsg("Cleared T" + (t + 1)); return; }
    if (copyHeld) {
        if (t !== track) { send(`copyt ${track} ${t}`); valuesStale = true; toastMsg(`T${track + 1} settings -> T${t + 1}`); }
        return;
    }
    if (muteHeld) { toggleMute(t); return; }
    if (shift) {
        const playing = status.tracks[t].play;
        send(`play ${t} ${playing ? 0 : 1}`);
        return;
    }
    selectTrack(t);
}

function toggleMute(t) {
    const i = pidx.mute;
    const v = tv[t][i] >= 0.5 ? 0 : 1;
    setVal({ kind: "t", t, i }, v);
    toastMsg("T" + (t + 1) + (v ? " muted" : " unmuted"));
}

function onScenePad(n) {
    const used = (status.scenes >>> n) & 1;
    if (delHeld) { send("sclear " + n); toastMsg("Scene " + (n + 1) + " cleared"); return; }
    if (shift) { send("sstore " + n); status.scenes |= (1 << n); toastMsg("Stored scene " + (n + 1)); changedSinceSave = true; return; }
    if (used) { send("srecall " + n); valuesStale = true; toastMsg("Scene " + (n + 1)); }
    else toastMsg("Scene " + (n + 1) + " empty - Shift+pad stores");
}

function openBrowse(delta) {
    if (!browse) {
        if (status.nfiles <= 0) { toastMsg("No WAVs in UserLibrary/Samples"); return; }
        browse = { idx: 0, count: status.nfiles, names: {}, until: 0 };
        delta = 0;
    }
    browse.count = status.nfiles;
    browse.idx = Math.max(0, Math.min(browse.count - 1, browse.idx + delta));
    browse.until = Date.now() + 8000;
}

function browseName(i) {
    if (!browse) return "";
    if (browse.names[i] === undefined) browse.names[i] = dspGet("file:" + i) || "?";
    return browse.names[i];
}

globalThis.onMidiMessageInternal = function (data) {
    if (!data || data.length < 3) return;
    const st = data[0] & 0xF0, d1 = data[1], d2 = data[2];

    if (st === 0x90 || st === 0x80) {
        const on = st === 0x90 && d2 > 0;
        /* knob touch */
        if (d1 < 8) {
            if (on) {
                focus = { slot: d1, until: Date.now() + 100000 };
                if (delHeld) { const sl = slotFor(d1); if (sl) { setVal(sl, defOf(sl).df); toastMsg("Reset " + longLabel(sl)); } }
            } else if (focus && focus.slot === d1) focus.until = Date.now() + 800;
            return;
        }
        if (d1 < 10) return;
        /* step buttons */
        if (d1 >= 16 && d1 <= 31) {
            const s = d1 - 16;
            if (s < 8) { if (on) setPage(s); }
            else if (s < 12) { if (on) toggleMute(s - 8); }
            else {
                const p = s - 12;
                punchHeld[p] = on;
                send(`punch ${p} ${on ? 1 : 0}`, "punch" + p);
            }
            flush();
            return;
        }
        /* pads */
        if (d1 >= 68 && d1 <= 99) {
            const idx = d1 - 68, row = Math.floor(idx / 8), col = idx % 8;
            if (on) heldPads.add(d1); else heldPads.delete(d1);
            if (page === PAGE_SCENE) { if (on) onScenePad((3 - row) * 8 + col); flush(); }
            else if (on && !shift) track = 3 - row;   /* playing a row focuses that track */
            return;
        }
        return;
    }

    if (st !== 0xB0) return;
    const pressed = d2 > 0;

    if (d1 >= CC_KNOB1 && d1 < CC_KNOB1 + 8) { turnKnob(d1 - CC_KNOB1, d2); flush(); return; }

    switch (d1) {
        case CC_SHIFT: shift = pressed; return;
        case CC_DELETE: delHeld = pressed; return;
        case CC_COPY: copyHeld = pressed; return;
        case CC_MUTE: muteHeld = pressed; return;
        case CC_BACK: return;   /* host: suspend (Shift+Back exits) */
    }
    const ti = TRACK_CC.indexOf(d1);
    if (ti >= 0) { if (pressed) onTrackButton(ti); flush(); return; }
    if (!pressed && d1 !== CC_JOG) return;

    switch (d1) {
        case CC_JOG: {
            const delta = decodeDelta(d2);
            if (help) { help.line = Math.max(0, Math.min(HELP.length - 5, help.line + delta)); return; }
            if (page === 0) openBrowse(delta);
            else if (page === PAGE_SCENE) { const sl = { kind: "g", i: 1 }; setVal(sl, valOf(sl) + delta * 0.01); focus = { slot: 0, until: Date.now() + 1200 }; }
            else if (page === PAGE_MOD) { sub[page] = (sub[page] + (delta > 0 ? 1 : 3)) % 4; }
            else { const n = subCount[page] || 1; sub[page] = (sub[page] + n + (delta > 0 ? 1 : -1)) % n; }
            break;
        }
        case CC_JOG_CLICK:
            if (help) { help = null; break; }
            if (browse) {
                send(`load ${track} ${browse.idx}`);
                toastMsg("Loading " + browseName(browse.idx).split("/").pop());
                browse = null; changedSinceSave = true;
            } else if (page === 0) openBrowse(0);
            break;
        case CC_LEFT: case CC_RIGHT: {
            if (browse) { browse = null; break; }
            const n = subCount[page] || 1;
            sub[page] = (sub[page] + n + (d1 === CC_RIGHT ? 1 : -1)) % n;
            focus = null;
            break;
        }
        case CC_UP: selectTrack(track - 1); break;
        case CC_DOWN: selectTrack(track + 1); break;
        case CC_PLAY: {
            const any = status.tracks.some((t) => t.play);
            send("playall " + (any ? 0 : 1));
            break;
        }
        case CC_REC: case CC_SAMPLE: {
            const r = status.tracks[track].rec;
            send("rec " + track);
            toastMsg(r ? "Rec stopped" : (status.tracks[track].len > 0 ? "Overdub T" : "Recording T") + (track + 1));
            changedSinceSave = true;
            break;
        }
        case CC_CAPTURE:
            send("save"); toastMsg("Saving session..."); changedSinceSave = false; lastAutosave = Date.now();
            break;
        case CC_MENU: help = help ? null : { line: 0 }; break;
        case CC_LOOP: {   /* quick toggle: live input through the selected track */
            const sl = { kind: "t", t: track, i: pidx.mon };
            setVal(sl, valOf(sl) >= 0.5 ? 0 : 1);
            toastMsg("Live input T" + (track + 1) + (valOf(sl) >= 0.5 ? " on" : " off"));
            break;
        }
        case CC_UNDO: valuesStale = true; break;
    }
    flush();
};

globalThis.onMidiMessageExternal = function (_data) {
    /* External notes reach the DSP directly (channels 1-4 -> tracks 1-4). */
};

/* ------------------------------------------------------------------ */
/* Drawing                                                             */
/* ------------------------------------------------------------------ */

function tw(s) { return typeof text_width === "function" ? text_width(s) : s.length * 6; }
function clip(s, n) { return s.length > n ? s.slice(0, n) : s; }

function drawHeader() {
    fill_rect(0, 0, 128, 10, 1);
    let title = DEV_NAMES[page];
    if (page === PAGE_MOD) title = "MOD " + (sub[page] + 1) + "/4";
    const n = subCount[page] || 1;
    if (n > 1 && page !== PAGE_MOD) title += " " + (sub[page] + 1) + "/" + n;
    print(2, 1, title, 0);
    /* track tabs */
    for (let t = 0; t < 4; t++) {
        const x = 128 - (4 - t) * 9, tr = status.tracks[t];
        if (t === track) fill_rect(x, 1, 8, 8, 0);
        const c = t === track ? 1 : 0;
        if (tr.rec) { draw_rect(x + 1, 2, 6, 6, c); fill_rect(x + 3, 4, 2, 2, c); }
        else if (tr.play) fill_rect(x + 2, 3, 4, 4, c);
        else draw_rect(x + 2, 3, 4, 4, c);
    }
}

function drawBar(x, y, w, h, v, bip) {
    draw_rect(x, y, w, h, 1);
    if (bip) {
        const mid = x + Math.floor(w / 2);
        const len = Math.round((w / 2 - 1) * Math.max(-1, Math.min(1, v)));
        if (len > 0) fill_rect(mid, y + 1, len, h - 2, 1);
        else if (len < 0) fill_rect(mid + len, y + 1, -len, h - 2, 1);
        fill_rect(mid, y, 1, h, 1);
    } else {
        fill_rect(x + 1, y + 1, Math.round((w - 2) * Math.max(0, Math.min(1, v))), h - 2, 1);
    }
}

function norm(d, v) { return (v - d.mn) / (d.mx - d.mn); }

function drawFocus(y) {
    const sl = slotFor(focus.slot);
    if (!sl) return false;
    const d = defOf(sl), v = valOf(sl);
    const name = longLabel(sl), val = formatVal(sl, v);
    print(2, y, clip(name, 13), 1);
    print(126 - tw(val), y, val, 1);
    if (d.f === FMT.ENUM || d.f === FMT.TARGET) {
        const n = d.f === FMT.ENUM ? d.e.split("|").length : meta.targets.length + 1;
        const w = Math.max(3, Math.floor(124 / n));
        const idx = Math.round(v);
        for (let i = 0; i < n && i * w < 124; i++) {
            if (i === idx) fill_rect(2 + i * w, y + 10, w - 1, 6, 1); else draw_rect(2 + i * w, y + 12, w - 1, 2, 1);
        }
    } else drawBar(2, y + 10, 124, 6, d.mn < 0 ? v / d.mx : norm(d, v), d.mn < 0);
    return true;
}

function drawWave(y, h) {
    const w = waves[track], tr = status.tracks[track];
    const mid = y + Math.floor(h / 2);
    if (!w || tr.len <= 0 && tr.rec !== 1) {
        print(2, y + 2, tr.rec === 1 ? "Recording..." : "Empty - jog to load,", 1);
        if (tr.rec !== 1) print(2, y + 10, "or press Rec", 1);
        if (tr.rec === 1) drawBar(2, y + 11, 124, 5, tr.pos / 1000, false);
        return;
    }
    for (let i = 0; i < 128; i++) {
        const c = w.charCodeAt(i);
        const q = c >= 97 ? c - 97 + 10 : c - 48;
        const a = Math.max(0, Math.round(q / 35 * (h / 2)));
        if (a > 0) fill_rect(i, mid - a, 1, a * 2, 1); else set_pixel(i, mid, 1);
    }
    /* region brackets */
    const s = trackVal(track, "start"), l = trackVal(track, "len");
    const x0 = Math.round(s * 127), x1 = Math.round((s + l * (1 - s)) * 127);
    for (let yy = y; yy < y + h; yy += 2) { if (x0 > 0) set_pixel(x0, yy, 1); if (x1 < 127) set_pixel(x1, yy, 1); }
    fill_rect(0, y, Math.max(0, x0), 1, 1); fill_rect(x1, y, 128 - x1, 1, 1);
    /* playhead */
    if (tr.play && tr.len > 0) {
        const px = Math.round(tr.pos / 1000 * 127);
        for (let yy = y; yy < y + h; yy++) set_pixel(px, yy, (yy & 1) ? 0 : 1);
        fill_rect(px - 1, y, 3, 2, 1);
    }
    const nm = clip(names[track] || "", 20);
    fill_rect(0, y + h - 8, tw(nm) + 3, 8, 0);
    print(1, y + h - 8, nm, 1);
}

function drawGrid(y0) {
    for (let k = 0; k < 8; k++) {
        const sl = slotFor(k);
        const col = k % 4, row = Math.floor(k / 4);
        const x = col * 32, y = y0 + row * 17;
        if (!sl) continue;
        const d = defOf(sl), v = valOf(sl);
        const hl = focus && focus.slot === k && Date.now() < focus.until;
        if (hl) fill_rect(x, y - 1, 31, 17, 1);
        const c = hl ? 0 : 1;
        print(x + 1, y, clip(labelFor(sl), 5), c);
        print(x + 1, y + 8, clip(formatVal(sl, v), 5), c);
        /* modulation marker */
        if (sl.kind === "t" && isModulated(sl.t, sl.i)) fill_rect(x + 28, y + 1, 2, 2, c);
    }
}

function isModulated(t, i) {
    if (!meta) return false;
    const ti = meta.targets.indexOf(i);
    if (ti < 0) return false;
    for (let m = 0; m < 4; m++) {
        const b = pidx.m1type + m * 8;
        if (Math.round(tv[t][b + 3]) === ti + 1 && Math.abs(tv[t][b + 2]) > 0.001) return true;
    }
    return false;
}

function drawMod(y) {
    const m = sub[PAGE_MOD], b = pidx.m1type + m * 8;
    const typ = meta.params[b].e.split("|")[Math.round(tv[track][b] ?? 0)];
    const tgt = targetName(tv[track][b + 3] ?? 0);
    print(2, y, clip(typ + " > " + tgt, 21), 1);
    const v = modVals[m];
    const uni = Math.round(tv[track][b]) >= 6 || tv[track][b + 7] >= 0.5;
    drawBar(2, y + 10, 124, 6, uni ? v : v, !uni);
}

/* the filter bank's 48 band gains as bars, cutoff marked */
let bankCurve = "";
function drawBank(y, h) {
    const c = bankCurve;
    const base = y + h - 1;
    if (!c || c.length < 48) { print(2, y + 4, "48-band bank", 1); return; }
    for (let i = 0; i < 48; i++) {
        const ch = c.charCodeAt(i), q = ch >= 97 ? ch - 97 + 10 : ch - 48;
        const bh = Math.round(q / 35 * (h - 2));
        const x = 2 + Math.round(i * 124 / 48);
        if (bh > 0) fill_rect(x, base - bh + 1, 2, bh, 1); else set_pixel(x, base, 1);
    }
    const cut = trackVal(track, "fcut");
    /* bands span 30 Hz..16 kHz; cutoff in log space over 20 Hz..20 kHz */
    const hz = expMap(cut, 20, 20000);
    const pos = (Math.log2(hz) - Math.log2(30)) / (Math.log2(16000) - Math.log2(30));
    if (pos >= 0 && pos <= 1) { const x = 2 + Math.round(pos * 123); for (let yy = y; yy <= base; yy += 2) set_pixel(x, yy, 1); }
    const sc = meta.params[pidx.fscale].e.split("|")[Math.round(trackVal(track, "fscale"))];
    const label = sc === "Free" ? "" : sc;
    if (label) { fill_rect(126 - tw(label) - 2, y, tw(label) + 3, 8, 0); print(126 - tw(label), y, label, 1); }
}

function drawMix(y) {
    for (let t = 0; t < 4; t++) {
        const x = 2 + t * 32, tr = status.tracks[t];
        const muted = (tv[t][pidx.mute] ?? 0) >= 0.5;
        print(x, y, (t === track ? ">" : " ") + (t + 1) + (muted ? "M" : ""), 1);
        const h = Math.round(tr.peak / 99 * 12);
        draw_rect(x + 20, y + 2, 6, 14, 1);
        if (h > 0) fill_rect(x + 21, y + 15 - h, 4, h, 1);
    }
}

function drawScenes() {
    const morph = gv[1] ?? 0;
    print(2, 12, "Morph " + formatVal({ kind: "g", i: 1 }, morph), 1);
    print(80, 12, status.cur >= 0 ? "Scene " + (status.cur + 1) : "", 1);
    for (let n = 0; n < 32; n++) {
        const col = n % 8, row = Math.floor(n / 8);
        const x = 2 + col * 16, y = 22 + row * 9;
        const used = (status.scenes >>> n) & 1;
        if (n === status.cur) fill_rect(x, y, 14, 8, 1);
        else if (used) { draw_rect(x, y, 14, 8, 1); fill_rect(x + 3, y + 3, 8, 2, 1); }
        else draw_rect(x + 5, y + 3, 4, 2, 1);
    }
    print(2, 57, "", 1);
}

function drawBrowse() {
    fill_rect(0, 0, 128, 10, 1);
    print(2, 1, "LOAD > T" + (track + 1) + "  " + (browse.idx + 1) + "/" + browse.count, 0);
    const first = Math.max(0, Math.min(browse.count - 5, browse.idx - 2));
    for (let i = 0; i < 5 && first + i < browse.count; i++) {
        const n = first + i, y = 12 + i * 10;
        let s = browseName(n);
        if (tw(s) > 124) s = "~" + s.slice(-20);
        if (n === browse.idx) { fill_rect(0, y - 1, 128, 10, 1); print(2, y, s, 0); }
        else print(2, y, s, 1);
    }
}

const HELP = [
    "SCULPT  4-track",
    "sculpting sampler",
    "",
    "Track: select",
    "Shift+Track: play",
    "Mute+Track: mute",
    "Del+Track: clear",
    "Copy+Track: copy set",
    "Up/Down: track",
    "Steps 1-8: pages",
    " MAT GRN FLT COL SPC",
    " MOD MIX SCENES",
    "Steps 9-12: mutes",
    "Steps 13-16 (hold):",
    " Stutter  TapeStop",
    " Reverse  Freeze",
    "Left/Right: subpage",
    "Filter: 48-band bank",
    " Morph LP>BP>HP",
    " Decay: ringing",
    " Scale: tune bands",
    " Notes: play chords",
    " pg2: waves, noise",
    "Knob: edit param",
    " Shift+turn: fine",
    " Del+touch: reset",
    "Jog: browse samples",
    " (Material page)",
    " click: load",
    "Pads: 1 row/track",
    " Tape: cue slice",
    " Poly: play voices",
    "Play: all start/stop",
    "Rec: record/overdub",
    "Loop: live input",
    "Capture: save",
    "Scenes page pads:",
    " tap: recall",
    " Shift+pad: store",
    " Del+pad: clear",
    "Knob1/jog: morph",
    "Back: run in bg",
    "Shift+Back: quit",
];

function drawHelp() {
    fill_rect(0, 0, 128, 10, 1);
    print(2, 1, "HELP  (jog / Menu)", 0);
    for (let i = 0; i < 5; i++) {
        const l = HELP[help.line + i];
        if (l) print(2, 12 + i * 10, l, 1);
    }
}

function draw() {
    clear_screen();
    if (!meta || !status.ready) {
        print(2, 2, "SCULPT", 1);
        print(2, 24, "Starting engine...", 1);
        return;
    }
    if (help) { drawHelp(); return; }
    if (browse) { drawBrowse(); return; }
    drawHeader();
    const now = Date.now();
    const focusOn = focus && now < focus.until && slotFor(focus.slot);
    if (page === PAGE_SCENE) {
        if (focusOn) { fill_rect(0, 11, 128, 20, 0); drawFocus(12); }
        else drawScenes();
    } else {
        if (focusOn) drawFocus(12);
        else if (page === 0) drawWave(11, 19);
        else if (page === PAGE_MOD) drawMod(12);
        else if (page === PAGE_FLT) drawBank(11, 19);
        else if (page === PAGE_MIX) drawMix(12);
        else {
            /* device overview: sample name + transport */
            const tr = status.tracks[track];
            print(2, 12, clip(names[track] || "", 21), 1);
            drawBar(2, 22, 124, 5, tr.len > 0 ? tr.pos / 1000 : 0, false);
        }
        drawGrid(31);
    }
    if (toast && now < toast.until) {
        const w = Math.min(128, tw(toast.text) + 6);
        fill_rect(0, 54, w, 10, 1);
        print(3, 55, toast.text, 0);
    }
}

/* ------------------------------------------------------------------ */
/* LEDs (diffed and rate-limited: the MIDI out buffer holds ~64 pkts)  */
/* ------------------------------------------------------------------ */

function want(kind, id, color) { ledWant.set(kind + id, { kind, id, color }); }

function computeLeds() {
    const blink = (Math.floor(Date.now() / 250) & 1) === 1;
    /* pads */
    for (let n = 68; n <= 99; n++) {
        const idx = n - 68, row = Math.floor(idx / 8), col = idx % 8, t = 3 - row;
        let c;
        if (page === PAGE_SCENE) {
            const sc = (3 - row) * 8 + col;
            const used = (status.scenes >>> sc) & 1;
            c = sc === status.cur ? WHITE : used ? TRACK_COL[row].mid : DARKGREY;
            if (heldPads.has(n)) c = WHITE;
        } else {
            const tr = status.tracks[t], tc = TRACK_COL[t];
            c = t === track ? tc.mid : tc.lo;
            if (tr.len <= 0 && tr.rec !== 1) c = t === track ? DARKGREY : BLACK;
            if (tr.play && tr.len > 0) {
                const slice = Math.min(7, Math.floor(tr.pos / 1000 * 8));
                const mode = tv[t][pidx.mode] ?? 0;
                if (mode < 0.5 && slice === col) c = tc.hi;
                if (mode >= 0.5 && ((tr.voices >> col) & 1)) c = tc.hi;
            } else if (tr.voices && ((tr.voices >> col) & 1)) c = tc.hi;
            if (tr.rec === 1) c = blink ? RED : RED_DARK;
            if (heldPads.has(n)) c = WHITE;
        }
        want("n", n, c);
    }
    /* steps */
    for (let s = 0; s < 16; s++) {
        let c;
        if (s < 8) c = s === page ? WHITE : GREY;
        else if (s < 12) { const t = s - 8; c = (tv[t][pidx.mute] ?? 0) >= 0.5 ? BLACK : TRACK_COL[t].mid; }
        else c = ((status.punch >> (s - 12)) & 1) || punchHeld[s - 12] ? RED : RED_DARK;
        want("n", 16 + s, c);
    }
    /* track buttons */
    for (let t = 0; t < 4; t++) {
        const tr = status.tracks[t];
        let c = t === track ? TRACK_COL[t].hi : TRACK_COL[t].lo;
        if (tr.rec && blink) c = RED;
        want("c", TRACK_CC[t], c);
    }
    const anyPlay = status.tracks.some((t) => t.play);
    want("c", CC_PLAY, anyPlay ? GREEN : GREEN_DIM);
    want("c", CC_REC, status.tracks[track].rec ? RED : RED_DARK);
    want("c", CC_SAMPLE, status.tracks[track].rec ? RED : RED_DARK);
    const n = subCount[page] || 1;
    want("c", CC_LEFT, n > 1 ? W_MED : W_OFF);
    want("c", CC_RIGHT, n > 1 ? W_MED : W_OFF);
    want("c", CC_UP, track > 0 ? W_DIM : W_OFF);
    want("c", CC_DOWN, track < 3 ? W_DIM : W_OFF);
    want("c", CC_SHIFT, shift ? W_BRIGHT : W_DIM);
    want("c", CC_MENU, help ? W_BRIGHT : W_DIM);
    want("c", CC_CAPTURE, changedSinceSave ? W_MED : W_DIM);
    want("c", CC_DELETE, delHeld ? W_BRIGHT : W_DIM);
    want("c", CC_COPY, copyHeld ? W_BRIGHT : W_DIM);
    want("c", CC_MUTE, muteHeld ? W_BRIGHT : W_DIM);
    want("c", CC_LOOP, (tv[track][pidx.mon] ?? 0) >= 0.5 ? W_BRIGHT : W_DIM);
    want("c", CC_UNDO, W_OFF);
    want("c", CC_BACK, W_DIM);
}

function pushLeds(max) {
    let sent = 0;
    for (const [k, l] of ledWant) {
        if (ledSent.get(k) === l.color) continue;
        if (l.kind === "n") setLED(l.id, l.color); else setButtonLED(l.id, l.color);
        ledSent.set(k, l.color);
        if (++sent >= max) break;
    }
}

function resetLeds() {
    ledSent.clear();
    if (typeof invalidateLedCache === "function") invalidateLedCache();
}

/* ------------------------------------------------------------------ */
/* Lifecycle                                                           */
/* ------------------------------------------------------------------ */

globalThis.init = function () {
    resetLeds();
    loadMeta();
    pollStatus();
    fetchValues();
    send("scenepads 0", "scenepads");
    flush();
};

globalThis.onResume = function () {
    resetLeds();
    valuesStale = true;
};

globalThis.tick = function () {
    tickCount++;
    if (!meta) { if (tickCount % 10 === 0) loadMeta(); draw(); return; }
    flush();
    pollStatus();
    if (!status.ready) { draw(); return; }
    const now = Date.now();
    if (valuesStale || status.morphing || now - lastValuesFetch > 2000) fetchValues();
    if (page === PAGE_FLT) bankCurve = dspGet("bank" + track) || "";
    if (page === PAGE_MOD) {
        const m = dspGet("modv" + track);
        if (m) modVals = m.split(",").map(Number);
    }
    if (browse && now > browse.until) browse = null;
    if (changedSinceSave && now - lastAutosave > 120000) {
        send("save"); lastAutosave = now; changedSinceSave = false;
    }
    computeLeds();
    pushLeds(12);
    draw();
};

globalThis.onUnload = function () {
    send("save");
    flushBlocking();
};
