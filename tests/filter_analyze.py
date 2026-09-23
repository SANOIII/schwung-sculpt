"""Checks the 48-band bank's responses rendered by filter_probe; writes build/fb/responses.png."""
import wave, numpy as np, sys
import matplotlib; matplotlib.use("Agg"); import matplotlib.pyplot as plt
from scipy import signal
def load(n):
    w = wave.open(f"build/fb/{n}.wav"); a = np.frombuffer(w.readframes(w.getnframes()), dtype=np.int16).reshape(-1, 2) / 32768
    return a
def psd(a):
    f, p = signal.welch(a[:, 0], 44100, nperseg=8192); return f, p
fails = 0
def expect(c, msg):
    global fails; print(("  [ok] " if c else "  [FAIL] ") + msg); fails += 0 if c else 1
f, dry = psd(load("dry"))
def rel(name):
    _, p = psd(load(name)); return 10 * np.log10(p / dry)
def band(r, lo, hi): m = (f >= lo) & (f < hi); return r[m].mean()
r_open = rel("open")
print("open bank gain (dB) 100-10k: mean %.2f, ripple %.2f" % (band(r_open, 100, 10000), r_open[(f > 100) & (f < 10000)].std()))
expect(abs(band(r_open, 100, 10000)) < 1.5, "open bank is ~unity (within 1.5 dB)")
expect(r_open[(f > 100) & (f < 10000)].std() < 1.5, "open bank ripple < 1.5 dB rms")
lp = rel("lp1k"); hp = rel("hp1k"); bp = rel("bp1k"); lpr = rel("lp1k_res"); lps = rel("lp1k_steep")
print("LP1k: 250Hz %.1f  1k %.1f  4k %.1f  8k %.1f" % (band(lp, 200, 300), band(lp, 900, 1100), band(lp, 3500, 4500), band(lp, 7000, 9000)))
expect(band(lp, 200, 300) > -2 and band(lp, 3500, 4500) < -15, "low-pass passes lows, cuts 2 oct above by >15 dB")
expect(band(hp, 3500, 4500) > -3 and band(hp, 200, 300) < -15, "high-pass passes highs, cuts 2 oct below by >15 dB")
expect(band(bp, 900, 1100) > band(bp, 200, 300) + 8 and band(bp, 900, 1100) > band(bp, 4000, 6000) + 8, "band-pass peaks at the cutoff")
pkr = lambda r: r[(f > 900) & (f < 1150)].max()
expect(pkr(lpr) > pkr(lp) + 6, "resonance narrows the bands into a peak at the cutoff (>6 dB)")
expect(band(lps, 1900, 2100) < band(lp, 1900, 2100) - 10, "steeper slope cuts harder one octave out")
# ringing: T60 of short vs long decay
def t60(name):
    a = load(name)[:, 0]; seg = a[44100:88200]; e = np.convolve(seg**2, np.ones(441)/441, "same")
    pk = e.argmax(); tail = e[pk:]; th = e[pk] * 1e-3
    idx = np.argmax(tail < th); return (idx if idx > 0 else len(tail)) / 44100
tl, ts = t60("ring_minor"), t60("ring_short")
print("ring T60: decay .7 -> %.2fs, decay .3 -> %.2fs" % (tl, ts))
expect(tl > ts * 2 and tl > 0.4, "longer Decay rings longer (metallic/bell tones)")
# scale quantisation: peaks of the minor-scale ring fall on C minor pitch classes
a = load("ring_minor")[:, 0][44100:88200]
F = np.abs(np.fft.rfft(a * np.hanning(len(a)))); fr = np.fft.rfftfreq(len(a), 1 / 44100)
pk, _ = signal.find_peaks(F, height=F.max() * 0.12, distance=20)
pcs = [int(round(69 + 12 * np.log2(fr[i] / 440))) % 12 for i in pk if fr[i] > 60]
minor = {0, 2, 3, 5, 7, 8, 10}
ok = sum(p in minor for p in pcs)
print("minor ring peaks pitch classes:", sorted(set(pcs)), f"({ok}/{len(pcs)} in C minor)")
expect(len(pcs) >= 5 and ok / len(pcs) > 0.9, "Scale=Minor rings on C-minor notes")
a = load("notes_cm")[:, 0][44100:]
F = np.abs(np.fft.rfft(a * np.hanning(len(a)))); fr = np.fft.rfftfreq(len(a), 1 / 44100)
pk, _ = signal.find_peaks(F, height=F.max() * 0.12, distance=20)
pcs = [int(round(69 + 12 * np.log2(fr[i] / 440))) % 12 for i in pk if fr[i] > 60]
print("held C-Eb-G -> peaks:", sorted(set(pcs)))
expect(len(pcs) >= 3 and set(pcs) <= {0, 3, 7}, "Scale=Notes resonates on the held chord (C, Eb, G)")
wn = load("waves_noise"); 
expect(np.sqrt((wn**2).mean()) > 0.002 and np.isfinite(wn).all(), "waves + noise produce a textured signal")
# waves: spectrum moves over time
S = [psd(wn[i*44100:(i+1)*44100])[1] for i in range(5)]
mv = np.mean([np.abs(10*np.log10(S[i+1]/S[i]))[(f>200)&(f<8000)].mean() for i in range(4)])
print("waves spectral movement %.2f dB/s" % mv)
expect(mv > 1.0, "waves animate the spectrum")
fig, ax = plt.subplots(1, 2, figsize=(13, 4.2))
for n, r in [("open", r_open), ("LP 1k", lp), ("BP 1k", bp), ("HP 1k", hp), ("LP 1k + res", lpr), ("LP 1k steep", lps)]:
    ax[0].semilogx(f[1:], signal.savgol_filter(r[1:], 9, 2), label=n)
ax[0].set_xlim(30, 18000); ax[0].set_ylim(-50, 20); ax[0].grid(alpha=.3); ax[0].legend(fontsize=8); ax[0].set_title("48-band bank: multi-mode responses (dB)")
a = load("ring_minor")[:, 0][44100:88200]; F = 20*np.log10(np.abs(np.fft.rfft(a*np.hanning(len(a))))+1e-9); fr = np.fft.rfftfreq(len(a), 1/44100)
ax[1].semilogx(fr[1:], F[1:] - F.max()); ax[1].set_xlim(60, 8000); ax[1].set_ylim(-60, 2); ax[1].grid(alpha=.3); ax[1].set_title("Resonator: impulse, Decay .7, Scale Minor")
plt.tight_layout(); plt.savefig("build/fb/responses.png", dpi=80)
print("\nFILTER ALL OK" if not fails else f"\n{fails} FILTER FAILURES"); sys.exit(1 if fails else 0)
