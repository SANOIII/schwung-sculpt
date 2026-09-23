export const leds = [];
export function setLED(note, color) { globalThis.__leds.push(["n", note, color]); }
export function setButtonLED(cc, color) { globalThis.__leds.push(["c", cc, color]); }
export function invalidateLedCache() {}
export function decodeDelta(v) { if (v === 0) return 0; if (v <= 63) return v; if (v >= 65) return -(128 - v); return 0; }
