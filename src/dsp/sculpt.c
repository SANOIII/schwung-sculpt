/*
 * Sculpt — a four-track sculpting sampler for Schwung (Ableton Move).
 *
 * Inspired by the workflow of tape-style sculpting samplers: four parallel
 * stereo tracks, each running a fixed five-device chain
 *
 *     Material -> Granular -> Filter -> Color -> Space -> Mixer
 *
 * plus four modulators per track, 32 morphable scenes, and four
 * punch-in performance effects on the master bus.
 *
 * Runs as an overtake tool: the shim loads this .so as `overtake_dsp` and
 * calls render_block() on the SPI audio callback. EVERY entry point here
 * runs on that realtime thread, so:
 *   - no malloc/free, file I/O, logging or locks in any entry point
 *   - all allocation and disk access happens on our own SCHED_OTHER worker
 *     thread, which hands results to the audio path via atomic pointers.
 *
 * License: MIT
 */
#define _GNU_SOURCE
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <stdatomic.h>
#include <stdarg.h>
#ifndef SCULPT_WEB
#include <pthread.h>
#include <sched.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/stat.h>
#include <time.h>
#endif

#include "plugin_api_v1.h"
#include "sculpt_params.h"

/* ------------------------------------------------------------------ */
/* Constants                                                           */
/* ------------------------------------------------------------------ */

#define SR          44100.0f
#define NT          4
#define NVOICE      8
#define NMOD        4
#define NSCENE      32
#define NSLICE      8
#define TRACK_SECS  30
#define TCAP        ((int)(SR * TRACK_SECS))     /* frames per track buffer */
#define GCAP        131072                       /* grain capture (frames, pow2) */
#define GMASK       (GCAP - 1)
#define MAXGRAIN    20
#define DCAP        131072                       /* delay line (frames, pow2) */
#define DMASK       (DCAP - 1)
#define RVLEN       4096                         /* reverb line cap (pow2) */
#define RVMASK      (RVLEN - 1)
#define PCAP        262144                       /* punch buffer (~6 s) */
#define PMASK       (PCAP - 1)
#define XFADE       256
#define MAXFILES    1024
#define PATHLEN     256
#define WAVEPTS     128
#define BLOCK       128

/* Paths (overridable via env for offline tests; read only on the worker) */
static char DATA_DIR[PATHLEN]    = "/data/UserData/schwung/sculpt";
static char STATE_FILE[PATHLEN]  = "/data/UserData/schwung/sculpt/state.txt";
static char SAMPLE_ROOT[PATHLEN] = "/data/UserData/UserLibrary/Samples";
static char REC_DIR[PATHLEN]     = "/data/UserData/UserLibrary/Samples/Sculpt";

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static const host_api_v1_t *g_host = NULL;

/* Touch every page of a fresh allocation on the worker, so the audio thread
 * never takes a page fault the first time it writes into it (calloc'd memory
 * is mapped lazily by the kernel). */
static void prefault(void *p, size_t bytes) {
    volatile char *c = (volatile char *)p;
    if (!c) return;
    for (size_t i = 0; i < bytes; i += 4096) c[i] = 0;
}


static int find_param(const char *key) {
    for (int i = 0; i < NP; i++) if (!strcmp(PARAMS[i].key, key)) return i;
    return -1;
}


/* ------------------------------------------------------------------ */
/* Small helpers                                                       */
/* ------------------------------------------------------------------ */

static inline float clampf(float x, float a, float b) { return x < a ? a : (x > b ? b : x); }
static inline float lerpf(float a, float b, float t) { return a + (b - a) * t; }
static inline float softclip(float x) {
    if (x > 3.f) return 1.f;
    if (x < -3.f) return -1.f;
    float x2 = x * x;
    return x * (27.f + x2) / (27.f + 9.f * x2);
}
/* Transparent safety limiter: exactly linear below `t`, soft knee up to the
 * ceiling `c`. Unlike softclip() it adds no distortion to normal levels. */
static inline float knee_clip(float x, float t, float c) {
    float ax = fabsf(x);
    if (ax <= t) return x;
    float r = c - t;
    float y = t + r * softclip((ax - t) / r);
    return x < 0 ? -y : y;
}
static inline uint32_t xrand(uint32_t *s) {
    uint32_t x = *s; x ^= x << 13; x ^= x >> 17; x ^= x << 5; return *s = x;
}
static inline float frand(uint32_t *s) { return (xrand(s) >> 8) * (1.0f / 16777216.0f); }   /* 0..1 */
static inline float brand(uint32_t *s) { return frand(s) * 2.f - 1.f; }                      /* -1..1 */
static inline float expmap(float x, float lo, float hi) { return lo * powf(hi / lo, x); }
static inline float db2lin(float db) { return powf(10.f, db * 0.05f); }

/* ------------------------------------------------------------------ */
/* DSP building blocks                                                 */
/* ------------------------------------------------------------------ */

typedef struct { float ic1, ic2; } svf_t;
typedef struct { float a1, a2, a3, k; } svfc_t;

static inline void svf_coef(svfc_t *c, float fc, float res) {
    fc = clampf(fc, 10.f, SR * 0.45f);
    float g = tanf((float)M_PI * fc / SR);
    c->k = 2.f - 1.96f * clampf(res, 0.f, 1.f);
    c->a1 = 1.f / (1.f + g * (g + c->k));
    c->a2 = g * c->a1;
    c->a3 = g * c->a2;
}
/* returns lp, writes bp/hp */
static inline float svf_tick(svf_t *s, const svfc_t *c, float v0, float *bp, float *hp) {
    float v3 = v0 - s->ic2;
    float v1 = c->a1 * s->ic1 + c->a2 * v3;
    float v2 = s->ic2 + c->a2 * s->ic1 + c->a3 * v3;
    s->ic1 = 2.f * v1 - s->ic1;
    s->ic2 = 2.f * v2 - s->ic2;
    *bp = v1;
    *hp = v0 - c->k * v1 - v2;
    return v2;
}

typedef struct { float z; } onepole_t;
static inline float op_lp(onepole_t *o, float x, float a) { o->z += a * (x - o->z); return o->z; }
static inline float op_coef(float fc) { return 1.f - expf(-2.f * (float)M_PI * fc / SR); }

/* ------------------------------------------------------------------ */
/* Structures                                                          */
/* ------------------------------------------------------------------ */

typedef struct {
    int active;
    double pos;
    float rate;           /* signed */
    float env;
    int stage;            /* 0 attack, 1 sustain, 2 release */
    int gate;
    int key;              /* pad (0..7) or midi note + 100 */
    double rs, re;        /* region */
    int pong;
    float vel;
    uint32_t age;
} voice_t;

typedef struct {
    int active;
    float pos;            /* read position in capture buffer (frames) */
    float rate;
    int len, n;
    float inv_len;
    float gl, gr;
    float amp;
} grain_t;

#define NB 48
typedef struct {
    float ic1[2][3][NB], ic2[2][3][NB];    /* 3 cascaded SVF stages/band */
    float a1[2][NB], a2[2][NB], a3[2][NB]; /* SVF coefficients           */
    float kk[2][NB];                       /* per-stage unity-peak gain  */
    float norm[2][NB];                     /* per-band output scaling    */
    float lx[2][NB];                       /* band log2 freq (for gains) */
    float gt[2][NB];                       /* tan(pi f / SR) per band    */
    float qd[2][NB];                       /* Q implied by Decay         */
    float g[2][NB], dg[2][NB];             /* smoothed band gains        */
    unsigned char off[2][NB];              /* above Nyquist / duplicate  */
    float key[8];                          /* params the coefs were built from */
    int built;
    float wph;                             /* waves phase                */
    float nlp;                             /* noise colour state         */
    float agc_in, agc_out, agc_g;          /* resonator auto-gain        */
    int active;
} fbank_t;

typedef struct {
    float phase;
    float value;          /* current output (-1..1 bipolar) */
    float sh, sh_prev, sh_next;
    float env;
    int env_stage;        /* 0 idle 1 attack 2 decay */
    int last_step;
} mod_t;

typedef struct {
    /* ---- material ---- */
    float *buf;           /* interleaved stereo, TCAP frames */
    int len;              /* content length in frames */
    int playing;          /* tape transport */
    double tp;            /* tape playhead */
    int tdir;             /* ping-pong direction */
    float tgain;          /* tape transport gain */
    int xf_n; double xf_pos; float xf_rate;  /* declick crossfade */
    voice_t v[NVOICE];
    uint32_t vage;
    int rec;              /* 0 off, 1 first pass, 2 overdub */
    int rh;               /* first-pass rec head */
    int last_widx;
    int dirty;
    int wave_req;
    int pads_held;        /* bitmask */

    /* ---- granular ---- */
    float *gcap;          /* GCAP stereo */
    int gw;
    float gnext;
    grain_t g[MAXGRAIN];
    float gfb_l, gfb_r;

    /* ---- filter: 48-band resonant bank ---- */
    fbank_t fb;
    float fenv;
    int note_mask;        /* pitch classes of held notes (latched) */
    int notes_held[12];

    /* ---- color ---- */
    onepole_t split[2];
    float hold[2], hphase;
    float cenv, cgain;
    int chold;
    onepole_t noise_lp;

    /* ---- space ---- */
    float *dl;            /* 2 x DCAP */
    int dw;
    float dsm;
    float dlp[2];
    float wow;
    float *rv;            /* 4 x RVLEN */
    int rvw;
    float rvlp[4];
    float *ap;            /* 2 x 1024 allpass */
    int apw;

    /* ---- mixer ---- */
    svf_t dj[2];
    float g_prev_l, g_prev_r;
    float mute_g;

    /* ---- mods ---- */
    mod_t m[NMOD];
    float follow;

    /* ---- params ---- */
    float base[NP];       /* user values */
    float eff[NP];        /* base + modulation, per block */

    /* meters */
    float peak;
    int idle_blocks;
    uint32_t rng;
} track_t;

typedef struct {
    int used;
    float v[NT][NP];
} scene_t;

/* Worker <-> RT job ring (RT produces, worker consumes) */
enum { JOB_LOAD = 1, JOB_CLEAR, JOB_WAVE, JOB_SAVE, JOB_RESCAN, JOB_SAVEWAV };
typedef struct { int type, a, b; } job_t;
#define JOBQ 64

typedef struct {
    /* big memory, owned by worker, published once */
    float *gcap[NT], *dl[NT], *rv[NT], *ap[NT];
    float *punch;         /* 2 x PCAP */
    scene_t *scenes;
} bigmem_t;

typedef struct sculpt {
    char module_dir[PATHLEN];
    track_t t[NT];
    float g[NG];

    /* worker sync */
#ifndef SCULPT_WEB
    pthread_t worker;
#endif
    atomic_int quit;
    atomic_int ready;              /* bigmem published */
    bigmem_t mem;
    job_t jobs[JOBQ];
    atomic_int jhead, jtail;

    _Atomic(float *) pending_buf[NT];
    atomic_int pending_len[NT];
    _Atomic(float *) retired_buf[NT];
    char track_name[NT][64];
    atomic_int name_ver;
    char track_path[NT][PATHLEN];

    /* wave overviews */
    unsigned char wave[NT][WAVEPTS];
    atomic_int wave_ver[NT];

    /* file list (published by worker) */
    char (*files)[PATHLEN];
    atomic_int nfiles;

    /* scenes */
    int cur_scene;
    int morphing;
    float morph_t, morph_step;
    float morph_from[NT][NP];
    float morph_to[NT][NP];

    /* punch FX */
    int punch;                     /* bitmask of held punch fx */
    int punch_mode;                /* active mode 0 none, 1 stutter, 2 stop, 3 reverse */
    int pw;
    double pr;                     /* read head */
    float pspeed;
    int pstart, plen, pk;
    float pmix;                    /* 0 live .. 1 punch */
    int freeze_all;

    /* master */
    float mcomp_env, mcomp_g;
    float dc_x[2], dc_y[2];
    int mcomp_hold;
    float last_master[BLOCK * 2];
    float bpm;
    double beatpos;                /* our own beat clock */
    int mod_targets[MAX_MOD_TARGETS];
    int n_mod_targets;

    uint32_t rng;
    int save_pending;
    int pads_are_scenes;           /* UI is on the scene page */
} sculpt_t;

/* ------------------------------------------------------------------ */
/* Worker thread                                                       */
/* ------------------------------------------------------------------ */

static int job_push(sculpt_t *s, int type, int a, int b) {
    int h = atomic_load_explicit(&s->jhead, memory_order_relaxed);
    int n = (h + 1) % JOBQ;
    if (n == atomic_load_explicit(&s->jtail, memory_order_acquire)) return 0;
    s->jobs[h].type = type; s->jobs[h].a = a; s->jobs[h].b = b;
    atomic_store_explicit(&s->jhead, n, memory_order_release);
    return 1;
}
static int job_pop(sculpt_t *s, job_t *out) {
    int t = atomic_load_explicit(&s->jtail, memory_order_relaxed);
    if (t == atomic_load_explicit(&s->jhead, memory_order_acquire)) return 0;
    *out = s->jobs[t];
    atomic_store_explicit(&s->jtail, (t + 1) % JOBQ, memory_order_release);
    return 1;
}

static uint32_t rd32(const unsigned char *p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24); }
static uint16_t rd16(const unsigned char *p) { return (uint16_t)(p[0] | (p[1] << 8)); }

#ifndef SCULPT_WEB
/* Load a WAV into a freshly allocated TCAP buffer. Returns frames or -1. */
static int wav_load(const char *path, float **out) {
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    unsigned char hdr[12];
    if (fread(hdr, 1, 12, f) != 12 || memcmp(hdr, "RIFF", 4) || memcmp(hdr + 8, "WAVE", 4)) { fclose(f); return -1; }
    int fmt = 0, ch = 0, bits = 0; uint32_t rate = 0;
    long data_off = -1; uint32_t data_len = 0;
    unsigned char ck[8];
    while (fread(ck, 1, 8, f) == 8) {
        uint32_t sz = rd32(ck + 4);
        if (!memcmp(ck, "fmt ", 4)) {
            unsigned char fb[40] = {0};
            size_t n = sz < 40 ? sz : 40;
            if (fread(fb, 1, n, f) != n) break;
            if (sz > n) fseek(f, sz - n, SEEK_CUR);
            fmt = rd16(fb); ch = rd16(fb + 2); rate = rd32(fb + 4); bits = rd16(fb + 14);
            if (fmt == 0xFFFE && n >= 26) fmt = rd16(fb + 24);
        } else if (!memcmp(ck, "data", 4)) {
            data_off = ftell(f); data_len = sz; break;
        } else {
            fseek(f, sz + (sz & 1), SEEK_CUR);
        }
    }
    if (data_off < 0 || ch < 1 || rate < 8000 || !(fmt == 1 || fmt == 3) ||
        !(bits == 16 || bits == 24 || bits == 32 || bits == 8)) { fclose(f); return -1; }
    int bps = bits / 8;
    long nframes = data_len / (bps * ch);
    unsigned char *raw = malloc((size_t)nframes * bps * ch);
    if (!raw) { fclose(f); return -1; }
    nframes = (long)fread(raw, bps * ch, nframes, f);
    fclose(f);

    float *buf = calloc((size_t)TCAP * 2, sizeof(float));
    if (!buf) { free(raw); return -1; }
    prefault(buf, (size_t)TCAP * 2 * sizeof(float));
    double step = (double)rate / SR;   /* source frames per output frame */
    long outn = (long)(nframes / step);
    if (outn > TCAP) outn = TCAP;
    for (long i = 0; i < outn; i++) {
        double sp = i * step;
        long i0 = (long)sp; float fr = (float)(sp - i0);
        long i1 = i0 + 1 < nframes ? i0 + 1 : i0;
        for (int c = 0; c < 2; c++) {
            int cc = c < ch ? c : 0;
            float a = 0, b = 0;
            for (int k = 0; k < 2; k++) {
                const unsigned char *p = raw + ((k ? i1 : i0) * ch + cc) * bps;
                float v;
                if (fmt == 3 && bits == 32) { float fv; memcpy(&fv, p, 4); v = fv; }
                else if (bits == 16) v = (int16_t)rd16(p) / 32768.f;
                else if (bits == 24) v = (int32_t)((p[0] << 8) | (p[1] << 16) | ((uint32_t)p[2] << 24)) / 2147483648.f;
                else if (bits == 32) v = (int32_t)rd32(p) / 2147483648.f;
                else v = (p[0] - 128) / 128.f;
                if (k) b = v; else a = v;
            }
            buf[i * 2 + c] = a + (b - a) * fr;
        }
    }
    free(raw);
    *out = buf;
    return (int)outn;
}

static int wav_save(const char *path, const float *buf, int frames) {
    FILE *f = fopen(path, "wb");
    if (!f) return 0;
    uint32_t dlen = (uint32_t)frames * 4;
    unsigned char h[44];
    memcpy(h, "RIFF", 4); uint32_t v = 36 + dlen; memcpy(h + 4, &v, 4);
    memcpy(h + 8, "WAVEfmt ", 8); v = 16; memcpy(h + 16, &v, 4);
    uint16_t w = 1; memcpy(h + 20, &w, 2); w = 2; memcpy(h + 22, &w, 2);
    v = 44100; memcpy(h + 24, &v, 4); v = 44100 * 4; memcpy(h + 28, &v, 4);
    w = 4; memcpy(h + 32, &w, 2); w = 16; memcpy(h + 34, &w, 2);
    memcpy(h + 36, "data", 4); memcpy(h + 40, &dlen, 4);
    fwrite(h, 1, 44, f);
    int16_t tmp[1024];
    for (int i = 0; i < frames * 2; i += 1024) {
        int n = frames * 2 - i < 1024 ? frames * 2 - i : 1024;
        for (int k = 0; k < n; k++) tmp[k] = (int16_t)(clampf(buf[i + k], -1.f, 1.f) * 32767.f);
        fwrite(tmp, 2, n, f);
    }
    fclose(f);
    return 1;
}

#endif

static void compute_wave(sculpt_t *s, int t, const float *buf, int len) {
    for (int i = 0; i < WAVEPTS; i++) {
        float pk = 0;
        if (buf && len > 0) {
            int a = (int)((long)len * i / WAVEPTS), b = (int)((long)len * (i + 1) / WAVEPTS);
            int stride = (b - a) / 64 + 1;
            for (int k = a; k < b; k += stride) {
                float x = fabsf(buf[k * 2]) + fabsf(buf[k * 2 + 1]);
                if (x > pk) pk = x;
            }
        }
        int q = (int)(sqrtf(clampf(pk * 0.5f, 0, 1)) * 35.f + 0.5f);
        s->wave[t][i] = (unsigned char)(q < 10 ? '0' + q : 'a' + q - 10);
    }
    atomic_fetch_add(&s->wave_ver[t], 1);
}

static const char *base_name(const char *p) { const char *b = strrchr(p, '/'); return b ? b + 1 : p; }

static void set_track_name(sculpt_t *s, int t, const char *name) {
    snprintf(s->track_name[t], sizeof s->track_name[t], "%s", name);
    char *dot = strrchr(s->track_name[t], '.');
    if (dot) *dot = 0;
    atomic_fetch_add(&s->name_ver, 1);
}

/* Publish a buffer to the RT side; waits until the previous hand-off is done. */
static void publish_buf(sculpt_t *s, int t, float *buf, int len) {
#ifndef SCULPT_WEB
    for (int i = 0; i < 400; i++) {
        float *r = atomic_exchange(&s->retired_buf[t], NULL);
        if (r) free(r);
        if (atomic_load(&s->pending_buf[t]) == NULL) break;
        usleep(5000);
    }
#else
    float *r = atomic_exchange(&s->retired_buf[t], NULL);
    if (r) free(r);
#endif
    atomic_store(&s->pending_len[t], len);
    float *old = atomic_exchange(&s->pending_buf[t], buf);
    if (old) free(old);   /* never picked up: superseded */
}

#ifndef SCULPT_WEB
static int scan_dir(char (*list)[PATHLEN], int n, const char *dir, int depth) {
    if (depth > 4 || n >= MAXFILES) return n;
    DIR *d = opendir(dir);
    if (!d) return n;
    struct dirent *e;
    while ((e = readdir(d)) && n < MAXFILES) {
        if (e->d_name[0] == '.') continue;
        char p[PATHLEN];
        if (snprintf(p, sizeof p, "%s/%s", dir, e->d_name) >= (int)sizeof p) continue;
        struct stat st;
        if (stat(p, &st)) continue;
        if (S_ISDIR(st.st_mode)) n = scan_dir(list, n, p, depth + 1);
        else {
            size_t l = strlen(p);
            if (l > 4 && (!strcasecmp(p + l - 4, ".wav"))) { memcpy(list[n], p, PATHLEN); n++; }
        }
    }
    closedir(d);
    return n;
}
static int cmp_path(const void *a, const void *b) { return strcasecmp((const char *)a, (const char *)b); }

static void do_rescan(sculpt_t *s) {
    static char (*lists[2])[PATHLEN];
    static int which = 0;
    which ^= 1;
    if (!lists[which]) lists[which] = calloc(MAXFILES, PATHLEN);
    if (!lists[which]) return;
    int n = scan_dir(lists[which], 0, SAMPLE_ROOT, 0);
    qsort(lists[which], n, PATHLEN, cmp_path);
    atomic_store(&s->nfiles, 0);
    s->files = lists[which];
    atomic_store(&s->nfiles, n);
}

static void save_state(sculpt_t *s) {
    mkdir(DATA_DIR, 0755);
    mkdir(REC_DIR, 0755);
    /* write dirty recordings first so their paths land in the state file */
    for (int t = 0; t < NT; t++) {
        track_t *tr = &s->t[t];
        if (tr->dirty && tr->buf && tr->len > 0) {
            char p[PATHLEN];
            time_t now = time(NULL);
            struct tm tmv; localtime_r(&now, &tmv);
            snprintf(p, sizeof p, "%s/Sculpt T%d %04d%02d%02d-%02d%02d%02d.wav",
                     REC_DIR, t + 1, tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday, tmv.tm_hour, tmv.tm_min, tmv.tm_sec);
            if (wav_save(p, tr->buf, tr->len)) {
                tr->dirty = 0;
                snprintf(s->track_path[t], PATHLEN, "%s", p);
                set_track_name(s, t, base_name(p));
            }
        }
    }
    char tmp[PATHLEN];
    if (snprintf(tmp, sizeof tmp, "%s.tmp", STATE_FILE) >= (int)sizeof tmp) return;
    FILE *f = fopen(tmp, "w");
    if (!f) return;
    fprintf(f, "sculpt 1\n");
    for (int i = 0; i < NG; i++) fprintf(f, "g %s %g\n", GPARAMS[i].key, s->g[i]);
    for (int t = 0; t < NT; t++) {
        fprintf(f, "path %d %s\n", t, s->track_path[t]);
        for (int i = 0; i < NP; i++) fprintf(f, "p %d %s %g\n", t, PARAMS[i].key, s->t[t].base[i]);
    }
    if (s->mem.scenes) {
        for (int sc = 0; sc < NSCENE; sc++) {
            if (!s->mem.scenes[sc].used) continue;
            for (int t = 0; t < NT; t++)
                for (int i = 0; i < NP; i++)
                    fprintf(f, "s %d %d %s %g\n", sc, t, PARAMS[i].key, s->mem.scenes[sc].v[t][i]);
        }
    }
    fclose(f);
    rename(tmp, STATE_FILE);
}


static void load_state(sculpt_t *s) {
    FILE *f = fopen(STATE_FILE, "r");
    if (!f) return;
    char line[512];
    while (fgets(line, sizeof line, f)) {
        char key[64]; int t, sc; float v;
        if (sscanf(line, "p %d %63s %f", &t, key, &v) == 3 && t >= 0 && t < NT) {
            int i = find_param(key); if (i >= 0) s->t[t].base[i] = v;
        } else if (sscanf(line, "s %d %d %63s %f", &sc, &t, key, &v) == 4 && sc >= 0 && sc < NSCENE && t >= 0 && t < NT) {
            int i = find_param(key);
            if (i >= 0 && s->mem.scenes) { s->mem.scenes[sc].v[t][i] = v; s->mem.scenes[sc].used = 1; }
        } else if (sscanf(line, "g %63s %f", key, &v) == 2) {
            for (int i = 0; i < NG; i++) if (!strcmp(GPARAMS[i].key, key)) s->g[i] = v;
        } else if (!strncmp(line, "path ", 5)) {
            t = line[5] - '0';
            if (t >= 0 && t < NT && line[6] == ' ') {
                char *p = line + 7; p[strcspn(p, "\r\n")] = 0;
                snprintf(s->track_path[t], PATHLEN, "%s", p);
            }
        }
    }
    fclose(f);
}

static void load_track_file(sculpt_t *s, int t, const char *path) {
    float *buf = NULL;
    int n = wav_load(path, &buf);
    if (n <= 0) return;
    compute_wave(s, t, buf, n);
    snprintf(s->track_path[t], PATHLEN, "%s", path);
    set_track_name(s, t, base_name(path));
    publish_buf(s, t, buf, n);
}

#endif /* !SCULPT_WEB */

/* One-time setup: big allocations, saved session, sample scan. */
static int worker_setup(sculpt_t *s) {
#ifndef SCULPT_WEB
    const char *e;
    if ((e = getenv("SCULPT_DATA_DIR"))) {
        snprintf(DATA_DIR, PATHLEN, "%s", e);
        snprintf(STATE_FILE, PATHLEN, "%s/state.txt", e);
    }
    if ((e = getenv("SCULPT_SAMPLE_ROOT"))) {
        snprintf(SAMPLE_ROOT, PATHLEN, "%s", e);
        snprintf(REC_DIR, PATHLEN, "%s/Sculpt", e);
    }
    mkdir(DATA_DIR, 0755);
#endif
    bigmem_t m;
    memset(&m, 0, sizeof m);
    int ok = 1;
    for (int t = 0; t < NT; t++) {
        m.gcap[t] = calloc((size_t)GCAP * 2, sizeof(float));
        m.dl[t]   = calloc((size_t)DCAP * 2, sizeof(float));
        m.rv[t]   = calloc((size_t)RVLEN * 4, sizeof(float));
        m.ap[t]   = calloc(2048, sizeof(float));
        if (!m.gcap[t] || !m.dl[t] || !m.rv[t] || !m.ap[t]) ok = 0;
    }
    m.punch = calloc((size_t)PCAP * 2, sizeof(float));
    m.scenes = calloc(NSCENE, sizeof(scene_t));
    if (!m.punch || !m.scenes) ok = 0;
    if (!ok) return 0;
    for (int t = 0; t < NT; t++) {
        prefault(m.gcap[t], (size_t)GCAP * 2 * sizeof(float));
        prefault(m.dl[t], (size_t)DCAP * 2 * sizeof(float));
        prefault(m.rv[t], (size_t)RVLEN * 4 * sizeof(float));
    }
    prefault(m.punch, (size_t)PCAP * 2 * sizeof(float));
    s->mem = m;

#ifndef SCULPT_WEB
    load_state(s);
#endif
    for (int t = 0; t < NT; t++) {
#ifndef SCULPT_WEB
        if (s->track_path[t][0]) {
            float *buf = NULL;
            int n = wav_load(s->track_path[t], &buf);
            if (n > 0) {
                compute_wave(s, t, buf, n);
                set_track_name(s, t, base_name(s->track_path[t]));
                atomic_store(&s->pending_len[t], n);
                atomic_store(&s->pending_buf[t], buf);
                continue;
            }
            s->track_path[t][0] = 0;
        }
#endif
        float *buf = calloc((size_t)TCAP * 2, sizeof(float));
        prefault(buf, (size_t)TCAP * 2 * sizeof(float));
        compute_wave(s, t, NULL, 0);
        set_track_name(s, t, "(empty)");
        atomic_store(&s->pending_len[t], 0);
        atomic_store(&s->pending_buf[t], buf);
    }
    atomic_store(&s->ready, 1);
#ifndef SCULPT_WEB
    do_rescan(s);
#endif
    return 1;
}

/* Drain queued jobs once. Returns 1 if anything was done. */
static int worker_poll(sculpt_t *s) {
    job_t j;
    int did = 0;
    while (job_pop(s, &j)) {
        did = 1;
        switch (j.type) {
#ifndef SCULPT_WEB
        case JOB_LOAD: {
            int n = atomic_load(&s->nfiles);
            if (j.b >= 0 && j.b < n) load_track_file(s, j.a, s->files[j.b]);
            break;
        }
        case JOB_SAVE:
            save_state(s);
            break;
        case JOB_SAVEWAV:
            s->t[j.a].dirty = 1;
            save_state(s);
            break;
        case JOB_RESCAN:
            do_rescan(s);
            break;
#endif
        case JOB_CLEAR: {
            float *buf = calloc((size_t)TCAP * 2, sizeof(float));
            if (buf) {
                prefault(buf, (size_t)TCAP * 2 * sizeof(float));
                s->track_path[j.a][0] = 0;
                set_track_name(s, j.a, "(empty)");
                compute_wave(s, j.a, NULL, 0);
                publish_buf(s, j.a, buf, 0);
            }
            break;
        }
        case JOB_WAVE:
            compute_wave(s, j.a, s->t[j.a].buf, s->t[j.a].len);
            break;
        }
    }
    for (int t = 0; t < NT; t++) {
        float *r = atomic_exchange(&s->retired_buf[t], NULL);
        if (r) free(r);
    }
    return did;
}

#ifndef SCULPT_WEB
static void *worker_main(void *arg) {
    sculpt_t *s = (sculpt_t *)arg;
    struct sched_param sp = { .sched_priority = 0 };
    sched_setscheduler(0, SCHED_OTHER, &sp);          /* MUST be first */
#ifdef __linux__
    cpu_set_t set; CPU_ZERO(&set);
    CPU_SET(0, &set); CPU_SET(1, &set); CPU_SET(2, &set);
    sched_setaffinity(0, sizeof(set), &set);
#endif
    if (!worker_setup(s)) return NULL;
    for (;;) {
        /* read quit BEFORE draining so a "save" queued just ahead of the
         * unload is still written */
        int quitting = atomic_load(&s->quit);
        int did = worker_poll(s);
        if (quitting) break;
        if (!did) usleep(10000);
    }
    return NULL;
}
#endif

/* ------------------------------------------------------------------ */
/* Parameters                                                          */
/* ------------------------------------------------------------------ */

static void params_default(sculpt_t *s) {
    for (int t = 0; t < NT; t++)
        for (int i = 0; i < NP; i++) s->t[t].base[i] = s->t[t].eff[i] = PARAMS[i].def;
    for (int i = 0; i < NG; i++) s->g[i] = GPARAMS[i].def;
}

static void build_mod_targets(sculpt_t *s) {
    int n = 0;
    for (int i = 0; i < P_MOD0 && n < MAX_MOD_TARGETS; i++) {
        const param_def_t *p = &PARAMS[i];
        if (p->fmt == FMT_ENUM || p->step) continue;
        s->mod_targets[n++] = i;
    }
    s->n_mod_targets = n;
}

static float beat_samples(sculpt_t *s) { return SR * 60.f / (s->bpm > 20.f ? s->bpm : 120.f); }

/* ------------------------------------------------------------------ */
/* Material                                                            */
/* ------------------------------------------------------------------ */

static inline void buf_read(const float *b, int len, double pos, float *l, float *r) {
    if (pos < 0 || pos >= len - 1) { *l = *r = 0; return; }
    int i = (int)pos; float fr = (float)(pos - i);
    const float *p = b + i * 2;
    *l = p[0] + (p[2] - p[0]) * fr;
    *r = p[1] + (p[3] - p[1]) * fr;
}

static void region(track_t *tr, double *rs, double *re) {
    double L = tr->len;
    double a = clampf(tr->eff[P_START], 0, 1) * L;
    double b = a + clampf(tr->eff[P_LEN], 0, 1) * (L - a);
    if (b - a < 64) b = a + 64;
    if (b > L) b = L;
    if (a > b - 1) a = b - 1;
    if (a < 0) a = 0;
    *rs = a; *re = b;
}

static float mat_rate(track_t *tr, float semi) {
    return powf(2.f, (tr->eff[P_PITCH] + tr->eff[P_FINE] * 0.01f + semi) / 12.f);
}

static void tape_jump(track_t *tr, double to) {
    tr->xf_pos = tr->tp; tr->xf_n = XFADE;
    tr->xf_rate = mat_rate(tr, 0) * (tr->tdir < 0 ? -1.f : 1.f);
    tr->tp = to;
}

static void mods_trigger(track_t *tr);

static void note_track(track_t *tr, int pc, int on) {
    pc = ((pc % 12) + 12) % 12;
    if (on) {
        int any = 0;
        for (int i = 0; i < 12; i++) any |= tr->notes_held[i];
        if (!any) tr->note_mask = 0;          /* new chord replaces the latched one */
        tr->notes_held[pc]++;
        tr->note_mask |= 1 << pc;
    } else if (tr->notes_held[pc] > 0) tr->notes_held[pc]--;
}

static void pad_on(sculpt_t *s, int t, int pad, int vel, int is_note) {
    track_t *tr = &s->t[t];
    {   /* feed the filter bank's "Notes" scale */
        static const int sc[8] = {0, 2, 3, 5, 7, 8, 10, 12};
        if (is_note) note_track(tr, pad, 1);
        else if ((int)tr->eff[P_PADMODE] == 1) note_track(tr, sc[pad & 7] + (int)tr->eff[P_PITCH], 1);
    }
    if (!tr->buf || tr->len < 64) { mods_trigger(tr); return; }
    double rs, re; region(tr, &rs, &re);
    int dir = (int)tr->eff[P_DIR];
    if (!is_note) tr->pads_held |= 1 << pad;
    mods_trigger(tr);

    if ((int)tr->eff[P_MODE] == 0) {         /* Tape: cue */
        double to = rs + (re - rs) * (is_note ? 0 : pad) / NSLICE;
        if (dir == 1) to = rs + (re - rs) * (NSLICE - (is_note ? 0 : pad)) / NSLICE - 1;
        if (tr->playing && tr->tgain > 0.01f) tape_jump(tr, to);
        else { tr->tp = to; tr->tgain = 0; }
        tr->tdir = dir == 1 ? -1 : 1;
        tr->playing = 1;
        return;
    }
    /* Poly */
    int padmode = (int)tr->eff[P_PADMODE];
    float semi = 0;
    double vs = rs, ve = re;
    if (is_note) semi = (float)(pad - 60);
    else if (padmode == 1) { static const int sc[8] = {0, 2, 3, 5, 7, 8, 10, 12}; semi = sc[pad]; }
    else { vs = rs + (re - rs) * pad / NSLICE; }
    voice_t *v = NULL;
    for (int i = 0; i < NVOICE; i++) if (!tr->v[i].active) { v = &tr->v[i]; break; }
    if (!v) {                              /* steal oldest */
        v = &tr->v[0];
        for (int i = 1; i < NVOICE; i++) if (tr->v[i].age < v->age) v = &tr->v[i];
    }
    memset(v, 0, sizeof *v);
    v->active = 1; v->gate = 1; v->stage = 0; v->env = 0;
    v->key = is_note ? pad + 100 : pad;
    v->rs = vs; v->re = ve;
    v->rate = mat_rate(tr, semi);
    v->pong = dir == 2;
    if (dir == 1) { v->rate = -v->rate; v->pos = ve - 1; } else v->pos = vs;
    v->vel = 0.25f + 0.75f * (vel / 127.f);
    v->age = ++tr->vage;
}

static void pad_off(sculpt_t *s, int t, int pad, int is_note) {
    track_t *tr = &s->t[t];
    {
        static const int sc[8] = {0, 2, 3, 5, 7, 8, 10, 12};
        if (is_note) note_track(tr, pad, 0);
        else if ((int)tr->eff[P_PADMODE] == 1 && (tr->pads_held >> pad & 1)) note_track(tr, sc[pad & 7] + (int)tr->eff[P_PITCH], 0);
    }
    if (!is_note) tr->pads_held &= ~(1 << pad);
    int key = is_note ? pad + 100 : pad;
    for (int i = 0; i < NVOICE; i++)
        if (tr->v[i].active && tr->v[i].key == key && tr->v[i].gate) { tr->v[i].gate = 0; tr->v[i].stage = 2; }
}

static void rec_toggle(sculpt_t *s, int t) {
    track_t *tr = &s->t[t];
    if (!tr->buf) return;
    if (tr->rec == 0) {
        if (tr->len < 64) { tr->rec = 1; tr->rh = 0; }
        else { tr->rec = 2; tr->last_widx = -1; if (!tr->playing) { tr->playing = 1; } }
        return;
    }
    if (tr->rec == 1) {
        int n = tr->rh;
        int q = (int)tr->base[P_QUANT];
        if (q > 0 && n > 0) {
            float unit = beat_samples(s) * (q == 2 ? 4.f : 1.f);
            int k = (int)floorf(n / unit + 0.5f);
            if (k < 1) k = 1;
            n = (int)(k * unit);
            while (n > TCAP) n -= (int)unit;
            if (n < 64) n = tr->rh;
        }
        tr->len = n;
        tr->base[P_START] = 0; tr->base[P_LEN] = 1;
        tr->eff[P_START] = 0; tr->eff[P_LEN] = 1;
        tr->tp = 0; tr->tdir = 1; tr->playing = 1; tr->tgain = 1.f;
        tr->base[P_MODE] = 0;
    }
    tr->rec = 0;
    tr->dirty = 1;
    job_push(s, JOB_WAVE, t, 0);
}

/* Render the material device for one block into out (stereo interleaved). */
static void material_block(sculpt_t *s, track_t *tr, const float *in, float *out, int n) {
    memset(out, 0, sizeof(float) * n * 2);
    float lvl = tr->eff[P_LEVEL_M];
    float atk_s = expmap(tr->eff[P_ATK], 1, 2000) * 0.001f * SR;
    float rel_s = expmap(tr->eff[P_REL], 5, 4000) * 0.001f * SR;
    float atk_inc = 1.f / (atk_s < 1 ? 1 : atk_s);
    float rel_mul = expf(-6.9f / (rel_s < 1 ? 1 : rel_s));
    int monitor = (int)tr->eff[P_MON] || tr->rec == 1;
    float ingain = tr->eff[P_INGAIN] * 2.f;

    if (tr->buf && tr->len >= 64 && tr->rec != 1) {
        double rs, re; region(tr, &rs, &re);
        double span = re - rs;
        int mode = (int)tr->eff[P_MODE];
        int dir = (int)tr->eff[P_DIR];
        if (mode == 0) {
            float rate = mat_rate(tr, 0);
            float target = tr->playing ? 1.f : 0.f;
            for (int i = 0; i < n; i++) {
                if (target > tr->tgain) { tr->tgain += atk_inc; if (tr->tgain > 1) tr->tgain = 1; }
                else if (target < tr->tgain) { tr->tgain = tr->tgain * rel_mul - 1e-5f; if (tr->tgain < 0) tr->tgain = 0; }
                if (tr->tgain <= 0 && !tr->playing) break;
                float l, r;
                buf_read(tr->buf, tr->len, tr->tp, &l, &r);
                if (tr->xf_n > 0) {
                    float xl, xr, g = tr->xf_n / (float)XFADE;
                    buf_read(tr->buf, tr->len, tr->xf_pos, &xl, &xr);
                    l = l * (1 - g) + xl * g; r = r * (1 - g) + xr * g;
                    tr->xf_pos += tr->xf_rate; tr->xf_n--;
                }
                out[i * 2] = l * tr->tgain * lvl; out[i * 2 + 1] = r * tr->tgain * lvl;
                /* overdub at playhead */
                if (tr->rec == 2 && in) {
                    int wi = (int)tr->tp;
                    if (wi != tr->last_widx && wi >= 0 && wi < tr->len) {
                        float od = tr->eff[P_ODUB];
                        tr->buf[wi * 2]     = tr->buf[wi * 2] * od + in[i * 2] * ingain;
                        tr->buf[wi * 2 + 1] = tr->buf[wi * 2 + 1] * od + in[i * 2 + 1] * ingain;
                        tr->last_widx = wi;
                    }
                }
                double step = rate * tr->tdir;
                if (dir == 1 && tr->tdir > 0 && dir != 2) tr->tdir = -1;
                if (dir == 0 && tr->tdir < 0) tr->tdir = 1;
                tr->tp += step;
                if (tr->tp >= re) {
                    if (dir == 2) { tr->tdir = -1; tr->tp = re - (tr->tp - re) - 1; }
                    else { tr->xf_pos = tr->tp; tr->xf_rate = (float)step; tr->xf_n = XFADE; tr->tp -= span; }
                } else if (tr->tp < rs) {
                    if (dir == 2) { tr->tdir = 1; tr->tp = rs + (rs - tr->tp); }
                    else { tr->xf_pos = tr->tp; tr->xf_rate = (float)step; tr->xf_n = XFADE; tr->tp += span; }
                }
                if (tr->tp < rs || tr->tp >= re) tr->tp = rs;
            }
        } else {
            for (int k = 0; k < NVOICE; k++) {
                voice_t *v = &tr->v[k];
                if (!v->active) continue;
                for (int i = 0; i < n; i++) {
                    if (v->stage == 0) { v->env += atk_inc; if (v->env >= 1) { v->env = 1; v->stage = 1; } }
                    else if (v->stage == 2) { v->env *= rel_mul; if (v->env < 1e-4f) { v->active = 0; break; } }
                    float l, r;
                    buf_read(tr->buf, tr->len, v->pos, &l, &r);
                    /* short fade at region ends for one-shots */
                    float edge = 1.f;
                    if (!v->pong) {
                        double d = v->rate > 0 ? v->re - v->pos : v->pos - v->rs;
                        if (d < 128) edge = (float)(d / 128.0);
                    }
                    float g = v->env * v->vel * lvl * edge;
                    out[i * 2] += l * g; out[i * 2 + 1] += r * g;
                    v->pos += v->rate;
                    if (v->pos >= v->re || v->pos < v->rs) {
                        if (v->pong) {
                            v->rate = -v->rate;
                            v->pos = v->pos >= v->re ? v->re - 1 : v->rs;
                        } else { v->active = 0; break; }
                    }
                }
            }
        }
    }

    /* first-pass recording */
    if (tr->rec == 1 && in && tr->buf) {
        for (int i = 0; i < n && tr->rh < TCAP; i++, tr->rh++) {
            tr->buf[tr->rh * 2] = in[i * 2] * ingain;
            tr->buf[tr->rh * 2 + 1] = in[i * 2 + 1] * ingain;
        }
        if (tr->rh >= TCAP) rec_toggle(s, (int)(tr - s->t));
    }
    if (monitor && in) {
        for (int i = 0; i < n * 2; i++) out[i] += in[i] * ingain;
    }
}

/* ------------------------------------------------------------------ */
/* Granular                                                            */
/* ------------------------------------------------------------------ */

#define WINLEN 1024
static float g_win[3][WINLEN + 1];     /* perc, hann, reverse-perc */
#ifndef SCULPT_WEB
static pthread_once_t g_win_once = PTHREAD_ONCE_INIT;
#endif
static void fbank_tables(void);
static void win_init(void) {
    fbank_tables();
    for (int i = 0; i <= WINLEN; i++) {
        float ph = (float)i / WINLEN, q = 1.f - ph;
        g_win[0][i] = ph < 0.03f ? ph / 0.03f : expf(-5.f * (ph - 0.03f));
        g_win[1][i] = 0.5f - 0.5f * cosf(2.f * (float)M_PI * ph);
        g_win[2][i] = q < 0.03f ? q / 0.03f : expf(-5.f * (q - 0.03f));
    }
}
/* contour 0 = percussive, .5 = smooth, 1 = reversed swell */
static inline float grain_window(float ph, const float *wa, const float *wb, float k) {
    float fi = ph * WINLEN;
    int i = (int)fi;
    if (i >= WINLEN) i = WINLEN - 1;
    float fr = fi - i;
    float a = wa[i] + (wa[i + 1] - wa[i]) * fr;
    float b = wb[i] + (wb[i + 1] - wb[i]) * fr;
    return a + (b - a) * k;
}

static void granular_block(sculpt_t *s, track_t *tr, float *io, int n, int freeze_ovr) {
    float *cap = tr->gcap;
    if (!cap) return;
    float mix = tr->eff[P_GMIX];
    int freeze = (int)tr->eff[P_GFREEZE] || freeze_ovr;
    float fb = tr->eff[P_GFB] * 0.9f;
    if (mix <= 0.0005f && !freeze) {
        for (int i = 0; i < n; i++) {
            cap[tr->gw * 2] = io[i * 2]; cap[tr->gw * 2 + 1] = io[i * 2 + 1];
            tr->gw = (tr->gw + 1) & GMASK;
        }
        for (int k = 0; k < MAXGRAIN; k++) tr->g[k].active = 0;
        return;
    }
    float size_s = expmap(tr->eff[P_GSIZE], 10, 1000) * 0.001f * SR;
    float interval;
    if ((int)tr->eff[P_GSYNC]) {
        int di = (int)clampf(tr->eff[P_GDENS] * 6.999f, 0, 6);
        interval = beat_samples(s) * GRAIN_DIVS[di];
    } else {
        interval = SR / expmap(tr->eff[P_GDENS], 1, 80);
    }
    float overlap = size_s / interval;
    float norm = 1.f / sqrtf(overlap > 1.f ? overlap : 1.f);
    float cont = tr->eff[P_GCONT];
    const float *wa = cont < 0.5f ? g_win[0] : g_win[1];
    const float *wb = cont < 0.5f ? g_win[1] : g_win[2];
    float wk = cont < 0.5f ? cont * 2.f : (cont - 0.5f) * 2.f;
    float wet_g = sinf(mix * (float)M_PI * 0.5f), dry_g = cosf(mix * (float)M_PI * 0.5f);

    for (int i = 0; i < n; i++) {
        float dl = io[i * 2], dr = io[i * 2 + 1];
        /* spawn */
        tr->gnext -= 1.f;
        if (tr->gnext <= 0) {
            float jitter = (int)tr->eff[P_GSYNC] ? 0.f : 0.3f * brand(&tr->rng);
            tr->gnext += interval * (1.f + jitter);
            if (tr->gnext < 1) tr->gnext = 1;
            for (int k = 0; k < MAXGRAIN; k++) {
                grain_t *g = &tr->g[k];
                if (g->active) continue;
                float semi = tr->eff[P_GPITCH] + tr->eff[P_GPRND] * 12.f * brand(&tr->rng);
                float rate = powf(2.f, semi / 12.f);
                if (frand(&tr->rng) < tr->eff[P_GREV]) rate = -rate;
                int len = (int)size_s; if (len < 64) len = 64;
                float need = rate > 0 ? len * (rate - 1.f) : len * -rate;
                if (need < 0) need = 0;
                float maxback = GCAP - 2.f * len * fabsf(rate) - 256;
                float back = need + 64 + tr->eff[P_GSPRAY] * frand(&tr->rng) * (SR * 2.5f);
                if (back > maxback) back = maxback;
                if (back < need + 64) back = need + 64;
                g->pos = (float)tr->gw - back;
                if (g->pos < 0) g->pos += GCAP;
                g->rate = rate; g->len = len; g->n = 0; g->inv_len = 1.f / len;
                float pan = tr->eff[P_GSPREAD] * brand(&tr->rng);
                g->gl = sqrtf(0.5f * (1.f - pan)); g->gr = sqrtf(0.5f * (1.f + pan));
                g->amp = norm * 1.41f;
                g->active = 1;
                break;
            }
        }
        float wl = 0, wr = 0;
        for (int k = 0; k < MAXGRAIN; k++) {
            grain_t *g = &tr->g[k];
            if (!g->active) continue;
            float w = grain_window((float)g->n * g->inv_len, wa, wb, wk) * g->amp;
            int i0 = (int)g->pos; float fr = g->pos - i0;
            int a = i0 & GMASK, b = (i0 + 1) & GMASK;
            float sl = cap[a * 2] + (cap[b * 2] - cap[a * 2]) * fr;
            float sr = cap[a * 2 + 1] + (cap[b * 2 + 1] - cap[a * 2 + 1]) * fr;
            float mono = 0.5f * (sl + sr);
            wl += (sl * 0.5f + mono * 0.5f) * w * g->gl * 1.41f;
            wr += (sr * 0.5f + mono * 0.5f) * w * g->gr * 1.41f;
            g->pos += g->rate;
            if (g->pos >= GCAP) g->pos -= GCAP; else if (g->pos < 0) g->pos += GCAP;
            if (++g->n >= g->len) g->active = 0;
        }
        if (!freeze) {
            cap[tr->gw * 2] = dl + tr->gfb_l * fb;
            cap[tr->gw * 2 + 1] = dr + tr->gfb_r * fb;
            tr->gw = (tr->gw + 1) & GMASK;
        }
        tr->gfb_l = softclip(wl); tr->gfb_r = softclip(wr);
        io[i * 2] = dl * dry_g + wl * wet_g;
        io[i * 2 + 1] = dr * dry_g + wr * wet_g;
    }
}

/* ------------------------------------------------------------------ */
/* Filter                                                              */
/* ------------------------------------------------------------------ */

/*
 * 48-band resonant filter bank.
 *
 * 48 sixth-order band-pass filters (three cascaded SVF stages each) spread
 * log-evenly from 30 Hz to 16 kHz (about 0.19 oct apart) run in parallel;
 * with Q 5.5 per stage the open bank sums flat to within ~1 dB and a band's
 * skirts fall at 18 dB/oct, so a low-pass reaches -45 dB two octaves out. A per-band gain curve, recomputed every 32 samples,
 * turns the bank into a multi-mode filter: Butterworth-shaped low-pass and
 * high-pass magnitudes and a Gaussian band-pass, morphed LP -> BP -> HP, with
 * a resonant bump at the cutoff. Band Q comes from Resonance and from Decay
 * (ring time: Q = pi * f * T60 / ln 1000), so long decays turn the bank into
 * a tuned resonator; Pitch shifts every band and Scale snaps each band to a
 * scale (or to the notes currently held on the track). Waves sweep a
 * travelling gain pattern across the bands; Noise excites the bank.
 */
static const unsigned short SCALE_MASKS[9] = {
    0,       /* Free   */
    0xFFF,   /* Chrom  */
    0xAB5,   /* Major  C D E F G A B       */
    0x5AD,   /* Minor  C D Eb F G Ab Bb    */
    0x6AD,   /* Dorian C D Eb F G A Bb     */
    0x295,   /* Penta  C D E G A           */
    0x4A9,   /* MinPen C Eb F G Bb         */
    0x555,   /* Whole  C D E F# G# A#      */
    0        /* Notes  (held notes)        */
};
#define FB_Q0   5.5f
#define FB_GAIN 1.795f

static float g_band_lx[NB];                 /* free-spaced band pitch, log2 Hz */
static void fbank_tables(void) {
    for (int i = 0; i < NB; i++) g_band_lx[i] = log2f(30.f) + (log2f(16000.f) - log2f(30.f)) * i / (NB - 1);
}

static void fbank_build(track_t *tr) {
    fbank_t *F = &tr->fb;
    float pitch = tr->eff[P_FPITCH], dec = tr->eff[P_FDECAY];
    float spread = tr->eff[P_FSPREAD];
    int scale = (int)(tr->eff[P_FSCALE] + 0.5f);
    int mask = SCALE_MASKS[scale < 0 ? 0 : scale > 8 ? 8 : scale];
    if (scale == 8) mask = tr->note_mask ? tr->note_mask : 0xFFF;
    float key[8] = { pitch, dec, spread, (float)scale, (float)mask, 0, 0, 0 };
    if (F->built && !memcmp(key, F->key, sizeof key)) return;
    memcpy(F->key, key, sizeof key);
    F->built = 1;

    float t60 = dec > 0.001f ? 0.05f * powf(80.f, dec) : 0.f;
    for (int ch = 0; ch < 2; ch++) {
        float det = spread * (ch ? 0.3f : -0.3f);          /* semitones */
        int prev = -1000;
        for (int i = 0; i < NB; i++) {
            float m = 69.f + 12.f * (g_band_lx[i] - log2f(440.f)) + pitch;
            F->off[ch][i] = 0;
            if (mask) {
                int mr = (int)floorf(m + 0.5f), best = mr;
                for (int d = 0; d <= 6; d++) {
                    if (mask >> (((mr + d) % 12 + 12) % 12) & 1) { best = mr + d; break; }
                    if (mask >> (((mr - d) % 12 + 12) % 12) & 1) { best = mr - d; break; }
                }
                if (best == prev) F->off[ch][i] = 1;      /* duplicate band */
                prev = best;
                m = (float)best;
            }
            float f = 440.f * powf(2.f, (m + det - 69.f) / 12.f);
            if (f > SR * 0.45f) { f = SR * 0.45f; F->off[ch][i] = 1; }
            if (f < 16.f) { f = 16.f; F->off[ch][i] = 1; }
            F->gt[ch][i] = tanf((float)M_PI * f / SR);
            F->qd[ch][i] = t60 > 0.f ? (float)M_PI * f * t60 / 6.9078f : 0.f;
            F->lx[ch][i] = log2f(f);
        }
    }
}

static void fbank_gains(sculpt_t *s, track_t *tr, float cut_lx, int nsub) {
    (void)s;
    fbank_t *F = &tr->fb;
    float morph = tr->eff[P_FMORPH], res = tr->eff[P_FRES];
    float order = 1.f + tr->eff[P_FSLOPE] * 7.f;              /* 6..48 dB/oct */
    float bw = 0.25f + (1.f - tr->eff[P_FSLOPE]) * 1.4f;      /* BP width, oct */
    float r2 = res * res;
    float bump = r2 * 5.f;
    float bump_norm = 1.f / (1.f + bump * 0.3f);             /* passband drops as the peak rises */
    float wave = tr->eff[P_FWAVE], cycles = 0.5f + tr->eff[P_FWSIZE] * 7.5f;
    float spread = tr->eff[P_FSPREAD];
    for (int ch = 0; ch < 2; ch++) {
        for (int i = 0; i < NB; i++) {
            float g = 0.f;
            if (!F->off[ch][i]) {
                float x = F->lx[ch][i] - cut_lx;
                float e2 = exp2f(clampf(2.f * order * x, -60.f, 60.f));
                float lp = 1.f / sqrtf(1.f + e2), hp = 1.f / sqrtf(1.f + 1.f / e2);
                float bp = expf(-x * x / (2.f * bw * bw));
                /* Resonance narrows the bands around the cutoff (full effect
                 * within ~half an octave, fading to 5% far away) so a resonant
                 * low-pass keeps a solid, un-notched low end. */
                float prox = 0.05f + 0.95f * expf(-x * x * 2.f);
                float qr = FB_Q0 * (1.f + r2 * 9.f * prox);
                float q = qr > F->qd[ch][i] ? qr : F->qd[ch][i];
                if (q > 3000.f) q = 3000.f;
                float gg = F->gt[ch][i], k = 1.f / q, a1 = 1.f / (1.f + gg * (gg + k));
                F->a1[ch][i] = a1; F->a2[ch][i] = gg * a1; F->a3[ch][i] = gg * gg * a1; F->kk[ch][i] = k;
                /* FB_GAIN makes overlapping Q0 bands sum to unity; narrow bands
                 * stop overlapping, so fade it toward 1 (a tone at a band centre
                 * stays at unity). Resonance adds a little back for body; Decay
                 * adds ring time, not loudness. */
                float ov = FB_GAIN * sqrtf(FB_Q0 / q);
                F->norm[ch][i] = (ov > 1.f ? ov : 1.f) * sqrtf(sqrtf(qr / FB_Q0));
                g = morph < 0.5f ? lerpf(lp, bp, morph * 2.f) : lerpf(bp, hp, morph * 2.f - 1.f);
                if (bump > 0.f) g *= (1.f + bump * expf(-x * x * 34.7f)) * bump_norm;   /* ~0.12 oct peak */
                if (wave > 0.f) g *= 1.f - wave * (0.5f + 0.5f * sinf(2.f * (float)M_PI * ((float)i / NB * cycles + F->wph)));
                if (spread > 0.f) g *= 1.f - spread * 0.85f * (float)((i + ch) & 1);
                g *= F->norm[ch][i];
            }
            if (!F->active) F->g[ch][i] = g;
            F->dg[ch][i] = (g - F->g[ch][i]) / nsub;
        }
    }
    F->active = 1;
}

static void filter_block(sculpt_t *s, track_t *tr, float *io, int n) {
    fbank_t *F = &tr->fb;
    float mix = tr->eff[P_FMIX], envamt = tr->eff[P_FENV];
    float drive = tr->eff[P_FDRIVE], noise = tr->eff[P_FNOISE];
    int idle = tr->eff[P_FCUT] >= 0.999f && tr->eff[P_FMORPH] <= 0.001f && tr->eff[P_FRES] <= 0.001f &&
               tr->eff[P_FDECAY] <= 0.001f && tr->eff[P_FWAVE] <= 0.001f && noise <= 0.001f &&
               envamt == 0.f && drive <= 0.001f && (int)(tr->eff[P_FSCALE] + 0.5f) == 0 && tr->eff[P_FPITCH] == 0.f;
    if (mix <= 0.0005f || idle) { F->active = 0; return; }

    fbank_build(tr);
    float env_a = op_coef(80.f), env_r = op_coef(6.f);
    float dgain = 1.f + drive * 7.f, dnorm = 1.f / softclip(dgain);
    float namp = noise * noise * 0.35f, tex = tr->eff[P_FNTEX];
    float dust_p = 0.0004f + tex * 0.004f;
    float wrate = expmap(tr->eff[P_FWRATE], 0.02f, 12.f);

    /* Resonator auto-gain: long Decay makes the bands so narrow that broadband
     * material (and the Noise exciter) gets very quiet, while a tone sitting on
     * a band centre stays at unity. Rather than a fixed boost (which makes bass
     * notes on a band centre clip), match the bank's output level to its input
     * level (half the difference in dB, up to +15 dB), blended in with Decay. Held while the input is
     * silent so ringing tails decay naturally. */
    float agc_amt = clampf(tr->eff[P_FDECAY] * 1.5f, 0.f, 1.f);
    if (F->agc_g <= 0.f) F->agc_g = 1.f;

    const int SUB = 32;
    for (int s0 = 0; s0 < n; s0 += SUB) {
        float sum_in = 0.f, sum_out = 0.f;
        float pk = 0;
        for (int i = s0; i < s0 + SUB; i++) { float a = fabsf(io[i * 2]) + fabsf(io[i * 2 + 1]); if (a > pk) pk = a; }
        tr->fenv += (pk > tr->fenv ? env_a * SUB * 0.25f : env_r * SUB * 0.25f) * (pk - tr->fenv);
        if (tr->fenv < 0) tr->fenv = 0;
        float cut_lx = log2f(expmap(tr->eff[P_FCUT], 20, 20000)) + envamt * clampf(tr->fenv, 0, 1.5f) * 6.f;
        F->wph += wrate * SUB / SR; if (F->wph > 1.f) F->wph -= 1.f;
        fbank_gains(s, tr, cut_lx, SUB);

        for (int ch = 0; ch < 2; ch++) {
            float *i1a = F->ic1[ch][0], *i2a = F->ic2[ch][0];
            float *i1b = F->ic1[ch][1], *i2b = F->ic2[ch][1];
            float *i1c = F->ic1[ch][2], *i2c = F->ic2[ch][2];
            const float *a1 = F->a1[ch], *a2 = F->a2[ch], *a3 = F->a3[ch], *kk = F->kk[ch];
            float *g = F->g[ch]; const float *dg = F->dg[ch];
            for (int i = s0; i < s0 + SUB; i++) {
                float dry = io[i * 2 + ch];
                float x = drive > 0.001f ? softclip(dry * dgain) * dnorm : dry;
                if (namp > 0.f) {
                    float w = brand(&tr->rng);
                    F->nlp += 0.3f * (w - F->nlp);
                    float hiss = w - F->nlp;                                  /* airy hiss */
                    float dust = frand(&tr->rng) < dust_p ? brand(&tr->rng) * 3.f : 0.f;
                    x += (hiss * (1.f - tex) + dust * tex) * namp;
                }
                float acc = 0.f;
                for (int b = 0; b < NB; b++) {
                    float A1 = a1[b], A2 = a2[b], A3 = a3[b], K = kk[b];
                    /* stage 1 */
                    float v3 = x - i2a[b];
                    float v1 = A1 * i1a[b] + A2 * v3;
                    float v2 = i2a[b] + A2 * i1a[b] + A3 * v3;
                    i1a[b] = 2.f * v1 - i1a[b]; i2a[b] = 2.f * v2 - i2a[b];
                    float y1 = v1 * K;
                    /* stage 2 */
                    v3 = y1 - i2b[b];
                    v1 = A1 * i1b[b] + A2 * v3;
                    v2 = i2b[b] + A2 * i1b[b] + A3 * v3;
                    i1b[b] = 2.f * v1 - i1b[b]; i2b[b] = 2.f * v2 - i2b[b];
                    float y2 = v1 * K;
                    /* stage 3 */
                    v3 = y2 - i2c[b];
                    v1 = A1 * i1c[b] + A2 * v3;
                    v2 = i2c[b] + A2 * i1c[b] + A3 * v3;
                    i1c[b] = 2.f * v1 - i1c[b]; i2c[b] = 2.f * v2 - i2c[b];
                    g[b] += dg[b];
                    acc += v1 * K * g[b];
                }
                sum_in += x * x; sum_out += acc * acc;
                float y = knee_clip(acc * F->agc_g, 2.5f, 4.f);   /* only guards runaway ringing */
                io[i * 2 + ch] = dry + (y - dry) * mix;
            }
        }
        const float ac = 0.03f;                                   /* ~100 ms at 32-sample steps */
        F->agc_in += ac * (sum_in - F->agc_in);
        F->agc_out += ac * (sum_out - F->agc_out);
        float target = 1.f;
        if (agc_amt > 0.f && F->agc_in > 1e-7f) {
            /* half the level difference in dB (ratio^0.5), up to +15 dB:
             * lifts quiet resonances without pumping kicks into the limiter */
            float ratio = sqrtf(sqrtf(F->agc_in / (F->agc_out + 1e-12f)));
            ratio = clampf(ratio, 1.f, 5.6f);
            target = 1.f + agc_amt * (ratio - 1.f);
            F->agc_g += 0.05f * (target - F->agc_g);
        } else if (agc_amt <= 0.f) F->agc_g += 0.05f * (1.f - F->agc_g);
    }
}

/* ------------------------------------------------------------------ */
/* Color                                                               */
/* ------------------------------------------------------------------ */

static void color_block(sculpt_t *s, track_t *tr, float *io, int n) {
    (void)s;
    float drv = tr->eff[P_CDRIVE], bits = tr->eff[P_CBITS], rate = tr->eff[P_CRATE];
    float comp = tr->eff[P_CCOMP], noise = tr->eff[P_CNOISE], mix = tr->eff[P_CMIX];
    if (mix <= 0.0005f || (drv < 0.001f && bits < 0.001f && rate < 0.001f && comp < 0.001f && noise < 0.001f)) return;
    float bal = tr->eff[P_CBAL];
    float dl = drv * (bal <= 0 ? 1.f : 1.f - bal), dh = drv * (bal >= 0 ? 1.f : 1.f + bal);
    float gl = 1.f + dl * dl * 30.f, gh = 1.f + dh * dh * 30.f;
    float cl = 1.f / sqrtf(gl), chh = 1.f / sqrtf(gh);
    float sa = op_coef(expmap(tr->eff[P_CSPLIT], 80, 8000));
    float nbits = 16.f - bits * 14.f;
    float q = powf(2.f, nbits - 1.f), iq = 1.f / q;
    float hold_n = 1.f + rate * rate * 40.f;
    float thr = db2lin(-comp * 30.f), ratio = 1.f + comp * 7.f;
    float makeup = db2lin(comp * 12.f);
    float ca = op_coef(1.f / 0.003f), cr = op_coef(1.f / 0.12f);
    float nlp = op_coef(6000.f);

    for (int i = 0; i < n; i++) {
        float x[2] = { io[i * 2], io[i * 2 + 1] }, y[2];
        for (int ch = 0; ch < 2; ch++) {
            float v = x[ch];
            if (drv > 0.001f) {
                float lo = op_lp(&tr->split[ch], v, sa), hi = v - lo;
                lo = softclip(lo * gl) * (gl > 1.01f ? cl * 1.3f : 1.f);
                float bias = 0.3f * dh;
                hi = (softclip(hi * gh + bias) - softclip(bias)) * (gh > 1.01f ? chh * 1.3f : 1.f);
                v = lo + hi;
            }
            y[ch] = v;
        }
        if (rate > 0.001f) {
            tr->hphase += 1.f;
            if (tr->hphase >= hold_n) { tr->hphase -= hold_n; tr->hold[0] = y[0]; tr->hold[1] = y[1]; }
            y[0] = tr->hold[0]; y[1] = tr->hold[1];
        }
        if (bits > 0.001f) { y[0] = floorf(y[0] * q + 0.5f) * iq; y[1] = floorf(y[1] * q + 0.5f) * iq; }
        float lvl = fabsf(y[0]) > fabsf(y[1]) ? fabsf(y[0]) : fabsf(y[1]);
        if (lvl > tr->cenv) { tr->cenv += ca * (lvl - tr->cenv); tr->chold = 900; }
        else if (tr->chold > 0) tr->chold--;
        else tr->cenv += cr * (lvl - tr->cenv);
        if (comp > 0.001f) {
            if ((i & 15) == 0) {
                float gr = 1.f;
                if (tr->cenv > thr) gr = powf(tr->cenv / thr, 1.f / ratio - 1.f);
                tr->cgain = gr * makeup;
            }
            y[0] *= tr->cgain; y[1] *= tr->cgain;
        }
        if (noise > 0.001f) {
            float nz = op_lp(&tr->noise_lp, brand(&tr->rng), nlp);
            float amt = noise * noise * 0.25f * (0.15f + 3.f * tr->cenv);
            y[0] += nz * amt; y[1] += (nz * 0.7f + brand(&tr->rng) * 0.3f) * amt;
        }
        io[i * 2] = x[0] + (y[0] - x[0]) * mix;
        io[i * 2 + 1] = x[1] + (y[1] - x[1]) * mix;
    }
}

/* ------------------------------------------------------------------ */
/* Space                                                               */
/* ------------------------------------------------------------------ */

static const int RV_LENS[4] = { 1557, 1617, 1491, 1422 };
static const int AP_LENS[2] = { 347, 113 };

static void space_block(sculpt_t *s, track_t *tr, float *io, int n, int freeze_ovr) {
    float dmix = tr->eff[P_DMIX], rmix = tr->eff[P_RMIX];
    int freeze = (int)tr->eff[P_FREEZE] || freeze_ovr;
    if (!tr->dl || (dmix <= 0.0005f && rmix <= 0.0005f)) return;
    int dtype = (int)tr->eff[P_DTYPE];
    float target;
    if ((int)tr->eff[P_DSYNC]) {
        int di = (int)clampf(tr->eff[P_DTIME] * 11.999f, 0, 11);
        target = beat_samples(s) * DELAY_DIVS[di];
    } else target = expmap(tr->eff[P_DTIME], 10, 2000) * 0.001f * SR;
    if (target > DCAP - 4096) target = DCAP - 4096;
    if (tr->dsm <= 0) tr->dsm = target;
    float fb = freeze ? 1.f : tr->eff[P_DFB] * (dtype == 1 ? 1.05f : 0.98f);
    float tone = op_coef(expmap(tr->eff[P_DTONE], 400, 16000));
    float slew = dtype == 1 ? 0.00008f : 0.0004f;

    float size = 0.3f + tr->eff[P_RSIZE] * 1.3f;
    float rt60 = 0.3f * powf(100.f, tr->eff[P_RDECAY]);
    int rl[4]; float rg[4];
    for (int k = 0; k < 4; k++) {
        rl[k] = (int)(RV_LENS[k] * size); if (rl[k] > RVLEN - 2) rl[k] = RVLEN - 2;
        rg[k] = freeze ? 1.f : powf(10.f, -3.f * rl[k] / (rt60 * SR));
    }
    float damp = freeze ? 1.f : op_coef(expmap(1.f - tr->eff[P_RDAMP], 1000, 18000));
    float in_g = freeze ? 0.f : 1.f;
    float dry_g = 1.f - 0.35f * dmix - 0.35f * rmix;
    float *D = tr->dl, *R = tr->rv, *A = tr->ap;
    /* tape wow/flutter, evaluated at the block edges and interpolated
     * (0.6 Hz wow + 11 Hz flutter: linear over 2.9 ms is plenty) */
    float wow0 = 0.f, wow_step = 0.f;
    if (dtype == 1 && dmix > 0.0005f) {
        float w1 = tr->wow + 0.6f * n / SR;
        float a0 = sinf(tr->wow * 2.f * (float)M_PI) * 18.f + sinf(tr->wow * 37.f * (float)M_PI) * 2.f;
        float a1 = sinf(w1 * 2.f * (float)M_PI) * 18.f + sinf(w1 * 37.f * (float)M_PI) * 2.f;
        wow0 = a0; wow_step = (a1 - a0) / n;
        tr->wow = w1 - floorf(w1);
    }

    for (int i = 0; i < n; i++) {
        float xl = io[i * 2], xr = io[i * 2 + 1];
        float ol = 0, orr = 0;
        float yl = 0, yr = 0;
        if (dmix > 0.0005f) {
            tr->dsm += (target - tr->dsm) * slew;
            float dd = tr->dsm;
            if (dtype == 1) dd += wow0 + wow_step * i;
            float rp = tr->dw - dd; if (rp < 0) rp += DCAP;
            int i0 = (int)rp; float fr = rp - i0;
            int a = i0 & DMASK, b = (i0 + 1) & DMASK;
            yl = D[a * 2] + (D[b * 2] - D[a * 2]) * fr;
            yr = D[a * 2 + 1] + (D[b * 2 + 1] - D[a * 2 + 1]) * fr;
            float fl = yl, fr2 = yr;
            if (!freeze) {
                tr->dlp[0] += tone * (fl - tr->dlp[0]); fl = tr->dlp[0];
                tr->dlp[1] += tone * (fr2 - tr->dlp[1]); fr2 = tr->dlp[1];
            }
            float wl, wr;
            if (dtype == 2) { wl = (xl + xr) * 0.5f * in_g + fr2 * fb; wr = fl * fb; }
            else { wl = xl * in_g + fl * fb; wr = xr * in_g + fr2 * fb; }
            if (dtype == 1) { wl = softclip(wl); wr = softclip(wr); }
            else { wl = clampf(wl, -4.f, 4.f); wr = clampf(wr, -4.f, 4.f); }
            D[tr->dw * 2] = wl; D[tr->dw * 2 + 1] = wr;
            tr->dw = (tr->dw + 1) & DMASK;
            ol += yl * dmix; orr += yr * dmix;
        }
        if (rmix > 0.0005f) {
            float x = ((xl + xr) * 0.5f + (yl + yr) * 0.25f * dmix) * in_g * 0.5f;
            /* two input allpasses */
            for (int k = 0; k < 2; k++) {
                float *ap = A + k * 1024;
                int rp = (tr->apw - AP_LENS[k]) & 1023;
                float d = ap[rp];
                float w = x + d * 0.6f;
                ap[tr->apw] = w;
                x = d - w * 0.6f;
            }
            tr->apw = (tr->apw + 1) & 1023;
            float o[4];
            for (int k = 0; k < 4; k++) o[k] = R[k * RVLEN + ((tr->rvw - rl[k]) & RVMASK)];
            float h0 = o[0] + o[1] + o[2] + o[3], h1 = o[0] - o[1] + o[2] - o[3];
            float h2 = o[0] + o[1] - o[2] - o[3], h3 = o[0] - o[1] - o[2] + o[3];
            float hm[4] = { h0 * 0.5f, h1 * 0.5f, h2 * 0.5f, h3 * 0.5f };
            for (int k = 0; k < 4; k++) {
                tr->rvlp[k] += damp * (hm[k] - tr->rvlp[k]);
                R[k * RVLEN + tr->rvw] = tr->rvlp[k] * rg[k] + x;
            }
            tr->rvw = (tr->rvw + 1) & RVMASK;
            ol += (o[0] + o[2] * 0.6f) * rmix;
            orr += (o[1] + o[3] * 0.6f) * rmix;
        }
        io[i * 2] = xl * dry_g + ol;
        io[i * 2 + 1] = xr * dry_g + orr;
    }
}

/* ------------------------------------------------------------------ */
/* Modulation                                                          */
/* ------------------------------------------------------------------ */

static void mods_trigger(track_t *tr) {
    for (int k = 0; k < NMOD; k++) {
        const float *mp = &tr->base[P_MOD0 + k * M_COUNT];
        int type = (int)mp[M_TYPE];
        if (type == 6) { tr->m[k].env_stage = 1; }
        if ((int)mp[M_RETRIG]) tr->m[k].phase = 0;
    }
}

static void mods_block(sculpt_t *s, track_t *tr, int n) {
    memcpy(tr->eff, tr->base, sizeof tr->eff);
    float bpm = s->bpm > 20.f ? s->bpm : 120.f;
    for (int k = 0; k < NMOD; k++) {
        const float *mp = &tr->base[P_MOD0 + k * M_COUNT];
        mod_t *m = &tr->m[k];
        int type = (int)mp[M_TYPE];
        float rate_hz;
        if ((int)mp[M_SYNC]) {
            int di = (int)clampf(mp[M_RATE] * 9.999f, 0, 9);
            rate_hz = bpm / 60.f / MOD_DIVS[di];
        } else rate_hz = expmap(mp[M_RATE], 0.02f, 20.f);
        float inc = rate_hz * n / SR;
        float prev_phase = m->phase;
        m->phase += inc;
        int wrapped = 0;
        if (m->phase >= 1.f) { m->phase -= floorf(m->phase); wrapped = 1; }
        (void)prev_phase;
        float ph = m->phase, sh = mp[M_SHAPE], v = 0;
        switch (type) {
        case 0: v = sinf(2.f * (float)M_PI * (ph + sh)); break;
        case 1: { float skew = clampf(sh, 0.02f, 0.98f);
                  v = ph < skew ? ph / skew : 1.f - (ph - skew) / (1.f - skew); v = v * 2.f - 1.f; break; }
        case 2: v = 1.f - 2.f * ph; if (sh > 0.5f) v = -v; break;
        case 3: v = ph < clampf(sh, 0.05f, 0.95f) ? 1.f : -1.f; break;
        case 4: if (wrapped) m->sh = brand(&tr->rng); v = m->sh; break;
        case 5: if (wrapped) { m->sh_prev = m->sh_next; m->sh_next = brand(&tr->rng); }
                { float t = ph * ph * (3 - 2 * ph); v = lerpf(m->sh_prev, m->sh_next, t); } break;
        case 6: {  /* AD envelope: rate -> decay speed, shape -> attack */
            float atk = expmap(sh, 0.001f, 2.f) * SR, dec = SR / (rate_hz > 0.01f ? rate_hz : 0.01f);
            if (m->env_stage == 1) { m->env += n / (atk < 1 ? 1 : atk); if (m->env >= 1) { m->env = 1; m->env_stage = 2; } }
            else if (m->env_stage == 2) { m->env *= expf(-5.f * n / dec); if (m->env < 1e-4f) { m->env = 0; m->env_stage = 0; } }
            v = m->env * 2.f - 1.f; break;
        }
        case 7: {
            float f = clampf(tr->follow * (1.f + sh * 8.f), 0, 1);
            float a = clampf(inc * 4.f, 0.001f, 1.f);
            m->env += (f - m->env) * a;
            v = m->env * 2.f - 1.f; break;
        }
        }
        if ((int)mp[M_POL] || type >= 6) v = (v + 1.f) * 0.5f;   /* unipolar */
        m->value = v;
        int tgt = (int)mp[M_TARGET];
        float depth = mp[M_DEPTH];
        if (tgt <= 0 || tgt > s->n_mod_targets || depth == 0.f) continue;
        int pi = s->mod_targets[tgt - 1];
        const param_def_t *pd = &PARAMS[pi];
        tr->eff[pi] += v * depth * (pd->max - pd->min);
    }
    for (int i = 0; i < P_MOD0; i++) {
        const param_def_t *pd = &PARAMS[i];
        tr->eff[i] = clampf(tr->eff[i], pd->min, pd->max);
    }
}

/* ------------------------------------------------------------------ */
/* Scenes                                                              */
/* ------------------------------------------------------------------ */

static void scene_store(sculpt_t *s, int sc) {
    if (!s->mem.scenes || sc < 0 || sc >= NSCENE) return;
    for (int t = 0; t < NT; t++) memcpy(s->mem.scenes[sc].v[t], s->t[t].base, sizeof(float) * NP);
    s->mem.scenes[sc].used = 1;
    s->cur_scene = sc;
}

static void scene_recall(sculpt_t *s, int sc) {
    if (!s->mem.scenes || sc < 0 || sc >= NSCENE || !s->mem.scenes[sc].used) return;
    scene_t *S = &s->mem.scenes[sc];
    float morph_ms = s->g[G_MORPH] > 0.001f ? expmap(s->g[G_MORPH], 1, 8000) : 0.f;
    s->cur_scene = sc;
    for (int t = 0; t < NT; t++) {
        for (int i = 0; i < NP; i++) {
            const param_def_t *pd = &PARAMS[i];
            s->morph_from[t][i] = s->t[t].base[i];
            s->morph_to[t][i] = S->v[t][i];
            if (morph_ms <= 0.f || pd->fmt == FMT_ENUM || pd->fmt == FMT_TARGET || pd->step)
                s->t[t].base[i] = S->v[t][i];
        }
    }
    if (morph_ms > 0.f) {
        s->morphing = 1; s->morph_t = 0;
        s->morph_step = BLOCK / (morph_ms * 0.001f * SR);
    } else s->morphing = 0;
}

static void scene_morph_block(sculpt_t *s) {
    if (!s->morphing) return;
    s->morph_t += s->morph_step;
    if (s->morph_t >= 1.f) { s->morph_t = 1.f; s->morphing = 0; }
    float k = s->morph_t;
    for (int t = 0; t < NT; t++)
        for (int i = 0; i < NP; i++) {
            const param_def_t *pd = &PARAMS[i];
            if (pd->fmt == FMT_ENUM || pd->fmt == FMT_TARGET || pd->step) continue;
            s->t[t].base[i] = lerpf(s->morph_from[t][i], s->morph_to[t][i], k);
        }
}

/* ------------------------------------------------------------------ */
/* Punch FX (master)                                                   */
/* ------------------------------------------------------------------ */

static void punch_block(sculpt_t *s, float *io, int n) {
    float *P = s->mem.punch;
    if (!P) return;
    /* choose active mode by priority (lowest bit wins) */
    int want = 0;
    if (s->punch & 1) want = 1; else if (s->punch & 2) want = 2; else if (s->punch & 4) want = 3;
    if (want != s->punch_mode && want != 0) {
        s->punch_mode = want;
        float beat = beat_samples(s);
        s->pk = 0;
        if (want == 1) { s->plen = (int)(beat * 0.5f); if (s->plen < 256) s->plen = 256; s->pstart = (s->pw - s->plen) & PMASK; }
        else if (want == 2) { s->pr = s->pw; s->pspeed = 1.f; }
        else { s->plen = (int)(beat * 2.f); if (s->plen > (int)(SR * 2.5f)) s->plen = (int)(SR * 2.5f); s->pr = s->pw - 1; s->pstart = s->pw; }
    }
    float target = want ? 1.f : 0.f;
    float inc = 1.f / 220.f;
    for (int i = 0; i < n; i++) {
        float xl = io[i * 2], xr = io[i * 2 + 1];
        P[s->pw * 2] = xl; P[s->pw * 2 + 1] = xr;
        s->pw = (s->pw + 1) & PMASK;
        if (s->pmix < target) { s->pmix += inc; if (s->pmix > 1) s->pmix = 1; }
        else if (s->pmix > target) { s->pmix -= inc; if (s->pmix < 0) s->pmix = 0; }
        if (s->pmix <= 0.f) { if (!want) s->punch_mode = 0; continue; }
        float yl = 0, yr = 0;
        if (s->punch_mode == 1) {
            int k = s->pk % s->plen;
            int idx = (s->pstart + k) & PMASK;
            float e = 1.f;
            if (k < 64) e = k / 64.f; else if (s->plen - k < 64) e = (s->plen - k) / 64.f;
            yl = P[idx * 2] * e; yr = P[idx * 2 + 1] * e;
            s->pk++;
        } else if (s->punch_mode == 2) {
            int i0 = (int)floor(s->pr); float fr = (float)(s->pr - i0);
            int a = i0 & PMASK, b = (i0 + 1) & PMASK;
            float g = s->pspeed < 0.2f ? s->pspeed * 5.f : 1.f;
            yl = (P[a * 2] + (P[b * 2] - P[a * 2]) * fr) * g;
            yr = (P[a * 2 + 1] + (P[b * 2 + 1] - P[a * 2 + 1]) * fr) * g;
            s->pr += s->pspeed;
            s->pspeed -= 1.f / (SR * 0.9f);
            if (s->pspeed < 0) s->pspeed = 0;
        } else if (s->punch_mode == 3) {
            int k = s->pk;
            int idx = ((int)s->pr) & PMASK;
            float e = 1.f;
            if (k < 64) e = k / 64.f; else if (s->plen - k < 64) e = (s->plen - k) / 64.f;
            yl = P[idx * 2] * e; yr = P[idx * 2 + 1] * e;
            s->pr -= 1.0; s->pk++;
            if (s->pk >= s->plen) { s->pk = 0; s->pr = s->pw - 1; }
        }
        io[i * 2] = xl * (1.f - s->pmix) + yl * s->pmix;
        io[i * 2 + 1] = xr * (1.f - s->pmix) + yr * s->pmix;
    }
}

/* ------------------------------------------------------------------ */
/* Plugin API                                                          */
/* ------------------------------------------------------------------ */

static void *create_instance(const char *module_dir, const char *json_defaults) {
    (void)json_defaults;
    sculpt_t *s = calloc(1, sizeof(sculpt_t));
    if (!s) return NULL;
    snprintf(s->module_dir, sizeof s->module_dir, "%s", module_dir ? module_dir : "");
    params_default(s);
    build_mod_targets(s);
    s->rng = 0x12345u;
    s->bpm = 120.f;
    s->cur_scene = -1;
    for (int t = 0; t < NT; t++) {
        s->t[t].rng = 0x9E3779B9u * (t + 1);
        s->t[t].tdir = 1;
        s->t[t].mute_g = 1.f;
        s->t[t].m[0].sh_next = 0;
        snprintf(s->track_name[t], sizeof s->track_name[t], "...");
    }
#ifdef SCULPT_WEB
    worker_setup(s);
#else
    pthread_attr_t at;
    pthread_attr_init(&at);
    pthread_attr_setinheritsched(&at, PTHREAD_EXPLICIT_SCHED);
    pthread_attr_setschedpolicy(&at, SCHED_OTHER);
    struct sched_param sp = { .sched_priority = 0 };
    pthread_attr_setschedparam(&at, &sp);
    if (pthread_create(&s->worker, &at, worker_main, s) != 0) {
        /* fall back to inheriting; worker demotes itself first thing */
        pthread_create(&s->worker, NULL, worker_main, s);
    }
    pthread_attr_destroy(&at);
#endif
    return s;
}

static void destroy_instance(void *inst) {
    sculpt_t *s = (sculpt_t *)inst;
    if (!s) return;
    atomic_store(&s->quit, 1);
#ifndef SCULPT_WEB
    pthread_join(s->worker, NULL);
#endif
    for (int t = 0; t < NT; t++) {
        free(s->t[t].buf);
        free(atomic_exchange(&s->pending_buf[t], NULL));
        free(atomic_exchange(&s->retired_buf[t], NULL));
        free(s->mem.gcap[t]); free(s->mem.dl[t]); free(s->mem.rv[t]); free(s->mem.ap[t]);
    }
    free(s->mem.punch); free(s->mem.scenes);
    free(s);
}

static void on_midi(void *inst, const uint8_t *msg, int len, int source) {
    sculpt_t *s = (sculpt_t *)inst;
    if (!s || len < 3) return;
    int st = msg[0] & 0xF0, ch = msg[0] & 0x0F;
    int on = st == 0x90 && msg[2] > 0, off = st == 0x80 || (st == 0x90 && msg[2] == 0);
    if (!on && !off) return;
    if (source == MOVE_MIDI_SOURCE_INTERNAL) {
        /* Move pads: notes 68..99, bottom row first. Top row = track 1. */
        if (msg[1] < 68 || msg[1] > 99 || s->pads_are_scenes) return;
        int idx = msg[1] - 68;
        int t = 3 - idx / 8, pad = idx % 8;
        if (on) pad_on(s, t, pad, msg[2], 0); else pad_off(s, t, pad, 0);
        return;
    }
    /* external MIDI: channels 1-4 play tracks 1-4 chromatically (C3 = root) */
    if (ch >= NT) return;
    if (on) pad_on(s, ch, msg[1], msg[2], 1); else pad_off(s, ch, msg[1], 1);
}

/* Parse one command line. */
static void command(sculpt_t *s, const char *ln) {
    char cmd[16] = {0};
    int a = 0, b = 0, c = 0; float f = 0;
    int nc = 0;
    if (sscanf(ln, "%15s%n", cmd, &nc) != 1) return;
    const char *rest = ln + nc;
    if (!strcmp(cmd, "p")) {
        if (sscanf(rest, "%d %d %f", &a, &b, &f) == 3 && a >= 0 && a < NT && b >= 0 && b < NP) {
            s->t[a].base[b] = clampf(f, PARAMS[b].min, PARAMS[b].max);
            if (s->morphing) s->morph_to[a][b] = s->t[a].base[b];
        }
    } else if (!strcmp(cmd, "k")) {           /* k <track> <key> <value> */
        char key[32];
        if (sscanf(rest, "%d %31s %f", &a, key, &f) == 3 && a >= 0 && a < NT) {
            int i = find_param(key);
            if (i >= 0) { s->t[a].base[i] = clampf(f, PARAMS[i].min, PARAMS[i].max); if (s->morphing) s->morph_to[a][i] = s->t[a].base[i]; }
        }
    } else if (!strcmp(cmd, "g")) {
        if (sscanf(rest, "%d %f", &a, &f) == 2 && a >= 0 && a < NG) s->g[a] = clampf(f, GPARAMS[a].min, GPARAMS[a].max);
    } else if (!strcmp(cmd, "trig")) {
        if (sscanf(rest, "%d %d %d", &a, &b, &c) == 3 && a >= 0 && a < NT && b >= 0 && b < NSLICE) pad_on(s, a, b, c, 0);
    } else if (!strcmp(cmd, "rel")) {
        if (sscanf(rest, "%d %d", &a, &b) == 2 && a >= 0 && a < NT && b >= 0 && b < NSLICE) pad_off(s, a, b, 0);
    } else if (!strcmp(cmd, "note")) {
        if (sscanf(rest, "%d %d %d", &a, &b, &c) == 3 && a >= 0 && a < NT) { if (c > 0) pad_on(s, a, b, c, 1); else pad_off(s, a, b, 1); }
    } else if (!strcmp(cmd, "play")) {
        if (sscanf(rest, "%d %d", &a, &b) == 2 && a >= 0 && a < NT) {
            track_t *tr = &s->t[a];
            if (b && !tr->playing) { double rs, re; region(tr, &rs, &re); if (tr->tp < rs || tr->tp >= re) tr->tp = rs; }
            tr->playing = b ? 1 : 0;
            if (!b) for (int i = 0; i < NVOICE; i++) if (tr->v[i].active) { tr->v[i].gate = 0; tr->v[i].stage = 2; }
        }
    } else if (!strcmp(cmd, "playall")) {
        if (sscanf(rest, "%d", &b) == 1) {
            for (int t = 0; t < NT; t++) {
                track_t *tr = &s->t[t];
                if (b) { double rs, re; region(tr, &rs, &re); tr->tp = tr->tdir < 0 ? re - 1 : rs; tr->xf_n = 0; tr->tgain = 0; }
                tr->playing = b && tr->len >= 64;
                if (!b) for (int i = 0; i < NVOICE; i++) if (tr->v[i].active) { tr->v[i].gate = 0; tr->v[i].stage = 2; }
            }
            s->beatpos = 0;
        }
    } else if (!strcmp(cmd, "rec")) {
        if (sscanf(rest, "%d", &a) == 1 && a >= 0 && a < NT) rec_toggle(s, a);
    } else if (!strcmp(cmd, "clear")) {
        if (sscanf(rest, "%d", &a) == 1 && a >= 0 && a < NT) {
            s->t[a].rec = 0; s->t[a].playing = 0; s->t[a].dirty = 0;
            job_push(s, JOB_CLEAR, a, 0);
        }
    } else if (!strcmp(cmd, "load")) {
        if (sscanf(rest, "%d %d", &a, &b) == 2 && a >= 0 && a < NT) { s->t[a].rec = 0; job_push(s, JOB_LOAD, a, b); }
    } else if (!strcmp(cmd, "punch")) {
        if (sscanf(rest, "%d %d", &a, &b) == 2 && a >= 0 && a < 4) {
            if (a == 3) s->freeze_all = b;
            else if (b) s->punch |= 1 << a; else s->punch &= ~(1 << a);
        }
    } else if (!strcmp(cmd, "sstore")) {
        if (sscanf(rest, "%d", &a) == 1) scene_store(s, a);
    } else if (!strcmp(cmd, "srecall")) {
        if (sscanf(rest, "%d", &a) == 1) scene_recall(s, a);
    } else if (!strcmp(cmd, "sclear")) {
        if (sscanf(rest, "%d", &a) == 1 && s->mem.scenes && a >= 0 && a < NSCENE) { s->mem.scenes[a].used = 0; if (s->cur_scene == a) s->cur_scene = -1; }
    } else if (!strcmp(cmd, "copyt")) {
        if (sscanf(rest, "%d %d", &a, &b) == 2 && a >= 0 && a < NT && b >= 0 && b < NT && a != b)
            memcpy(s->t[b].base, s->t[a].base, sizeof s->t[b].base);
    } else if (!strcmp(cmd, "reset")) {
        if (sscanf(rest, "%d %d", &a, &b) == 2 && a >= 0 && a < NT && b >= 0 && b < NP) s->t[a].base[b] = PARAMS[b].def;
    } else if (!strcmp(cmd, "save")) {
        job_push(s, JOB_SAVE, 0, 0);
    } else if (!strcmp(cmd, "savewav")) {
        if (sscanf(rest, "%d", &a) == 1 && a >= 0 && a < NT) job_push(s, JOB_SAVEWAV, a, 0);
    } else if (!strcmp(cmd, "scenepads")) {
        if (sscanf(rest, "%d", &a) == 1) {
            s->pads_are_scenes = a;
            if (a) for (int t = 0; t < NT; t++) for (int k = 0; k < NSLICE; k++) pad_off(s, t, k, 0);
        }
    } else if (!strcmp(cmd, "rescan")) {
        job_push(s, JOB_RESCAN, 0, 0);
    }
}

static void set_param(void *inst, const char *key, const char *val) {
    sculpt_t *s = (sculpt_t *)inst;
    if (!s || !key || !val) return;
    if (!strcmp(key, "cmd")) {
        /* newline-separated batch of commands */
        char line[128];
        const char *p = val;
        while (*p) {
            const char *e = strchr(p, '\n');
            size_t l = e ? (size_t)(e - p) : strlen(p);
            if (l >= sizeof line) l = sizeof line - 1;
            memcpy(line, p, l); line[l] = 0;
            command(s, line);
            if (!e) break;
            p = e + 1;
        }
    }
}

static int append(char *buf, int len, int pos, const char *fmt, ...) __attribute__((format(printf, 4, 5)));
static int append(char *buf, int len, int pos, const char *fmt, ...) {
    if (pos >= len - 1) return pos;
    va_list ap; va_start(ap, fmt);
    int n = vsnprintf(buf + pos, len - pos, fmt, ap);
    va_end(ap);
    if (n < 0) return pos;
    return pos + n >= len ? len - 1 : pos + n;
}

static int meta_param(char *buf, int len, int pos, const param_def_t *p, int idx) {
    return append(buf, len, pos,
        "%s{\"i\":%d,\"k\":\"%s\",\"n\":\"%s\",\"s\":\"%s\",\"d\":%d,\"pg\":%d,\"sl\":%d,"
        "\"mn\":%g,\"mx\":%g,\"df\":%g,\"f\":%d,\"lo\":%g,\"hi\":%g,\"e\":\"%s\",\"st\":%d}",
        idx ? "," : "", idx, p->key, p->name, p->sname, p->dev, p->page, p->slot,
        p->min, p->max, p->def, p->fmt, p->lo, p->hi, p->enums ? p->enums : "", p->step);
}

static int get_param(void *inst, const char *key, char *buf, int len) {
    sculpt_t *s = (sculpt_t *)inst;
    if (!s || !key || !buf || len < 2) return -1;
    int pos = 0;
    if (!strcmp(key, "meta")) {
        pos = append(buf, len, pos, "{\"params\":[");
        for (int i = 0; i < NP; i++) pos = meta_param(buf, len, pos, &PARAMS[i], i);
        pos = append(buf, len, pos, "],\"globals\":[");
        for (int i = 0; i < NG; i++) pos = meta_param(buf, len, pos, &GPARAMS[i], i);
        pos = append(buf, len, pos, "],\"targets\":[");
        for (int i = 0; i < s->n_mod_targets; i++) pos = append(buf, len, pos, "%s%d", i ? "," : "", s->mod_targets[i]);
        pos = append(buf, len, pos, "],\"ddiv\":\"%s\",\"mdiv\":\"%s\",\"gdiv\":\"%s\"}",
                     DELAY_DIV_NAMES, MOD_DIV_NAMES, GRAIN_DIV_NAMES);
        return pos;
    }
    if (!strcmp(key, "values")) {
        /* all base values: globals first, then track by track */
        for (int i = 0; i < NG; i++) pos = append(buf, len, pos, "%s%.4g", i ? "," : "", s->g[i]);
        for (int t = 0; t < NT; t++) {
            pos = append(buf, len, pos, ";");
            for (int i = 0; i < NP; i++) pos = append(buf, len, pos, "%s%.4g", i ? "," : "", s->t[t].base[i]);
        }
        return pos;
    }
    if (!strcmp(key, "ui")) {
        int scenes = 0;
        if (s->mem.scenes) for (int i = 0; i < NSCENE; i++) if (s->mem.scenes[i].used) scenes |= 1 << i;
        pos = append(buf, len, pos, "%d %.1f %u %d %d %d %d %d %d\n",
                     atomic_load(&s->ready), s->bpm, (unsigned)scenes, s->cur_scene, s->punch | (s->freeze_all << 3),
                     atomic_load(&s->nfiles), atomic_load(&s->name_ver), s->morphing, 0);
        for (int t = 0; t < NT; t++) {
            track_t *tr = &s->t[t];
            int ppos = 0;
            if (tr->len > 0) ppos = (int)(tr->tp * 1000.0 / tr->len);
            if (tr->rec == 1) ppos = (int)((long)tr->rh * 1000 / TCAP);
            int vmask = 0;
            for (int i = 0; i < NVOICE; i++) if (tr->v[i].active) {
                double rs = tr->v[i].rs, span = tr->len > 0 ? tr->len : 1;
                (void)rs;
                int sl = (int)(tr->v[i].pos / span * NSLICE);
                if (sl >= 0 && sl < NSLICE) vmask |= 1 << sl;
            }
            int pk = (int)(clampf(tr->peak, 0, 1) * 99);
            pos = append(buf, len, pos, "%d %d %d %d %d %d %d %d\n",
                         tr->playing, tr->rec, tr->len, ppos, vmask, pk, atomic_load(&s->wave_ver[t]), tr->dirty);
        }
        return pos;
    }
    if (!strncmp(key, "wave", 4)) {
        int t = key[4] - '0';
        if (t < 0 || t >= NT || len <= WAVEPTS) return -1;
        memcpy(buf, s->wave[t], WAVEPTS); buf[WAVEPTS] = 0;
        return WAVEPTS;
    }
    if (!strncmp(key, "name", 4)) {
        int t = key[4] - '0';
        if (t < 0 || t >= NT) return -1;
        return append(buf, len, 0, "%s", s->track_name[t]);
    }
#ifndef SCULPT_WEB
    if (!strncmp(key, "file:", 5)) {
        int i = atoi(key + 5), n = atomic_load(&s->nfiles);
        if (i < 0 || i >= n || !s->files) return -1;
        const char *p = s->files[i];
        size_t rl = strlen(SAMPLE_ROOT);
        const char *rel = (!strncmp(p, SAMPLE_ROOT, rl) && p[rl] == '/') ? p + rl + 1 : p;
        return append(buf, len, 0, "%s", rel);
    }
#endif
    if (!strncmp(key, "bank", 4)) {          /* current band gain curve, 48 chars 0-9a-z */
        int t = key[4] - '0';
        if (t < 0 || t >= NT || len <= NB) return -1;
        fbank_t *F = &s->t[t].fb;
        for (int i = 0; i < NB; i++) {
            float g = F->active && F->norm[0][i] > 0 ? F->g[0][i] / F->norm[0][i] : 1.f;
            int q = (int)(sqrtf(clampf(g / 2.5f, 0, 1)) * 35.f + 0.5f);
            buf[i] = (char)(q < 10 ? '0' + q : 'a' + q - 10);
        }
        buf[NB] = 0;
        return NB;
    }
    if (!strncmp(key, "modv", 4)) {
        int t = key[4] - '0';
        if (t < 0 || t >= NT) return -1;
        for (int k = 0; k < NMOD; k++) pos = append(buf, len, pos, "%s%.3f", k ? "," : "", s->t[t].m[k].value);
        return pos;
    }
    return -1;
}

static void render_block(void *inst, int16_t *out, int frames) {
    sculpt_t *s = (sculpt_t *)inst;
    if (!s || frames > BLOCK) { if (out) memset(out, 0, frames * 4); return; }
    memset(out, 0, sizeof(int16_t) * frames * 2);
    if (!atomic_load_explicit(&s->ready, memory_order_acquire)) return;

    /* tempo */
    if (g_host && g_host->get_bpm) { float b = g_host->get_bpm(); if (b > 20.f && b < 400.f) s->bpm = b; }

    /* input */
    float in[BLOCK * 2];
    const int16_t *ain = NULL;
    if (g_host && g_host->mapped_memory) ain = (const int16_t *)(g_host->mapped_memory + g_host->audio_in_offset);
    for (int i = 0; i < frames * 2; i++) in[i] = ain ? ain[i] / 32768.f : 0.f;

    scene_morph_block(s);

    float mix[BLOCK * 2];
    memset(mix, 0, sizeof mix);
    float tb[BLOCK * 2];

    for (int t = 0; t < NT; t++) {
        track_t *tr = &s->t[t];
        /* pick up buffers from the worker */
        float *nb = atomic_exchange_explicit(&s->pending_buf[t], NULL, memory_order_acq_rel);
        if (nb) {
            float *old = tr->buf;
            tr->buf = nb;
            tr->len = atomic_load(&s->pending_len[t]);
            tr->rec = 0; tr->tp = 0; tr->xf_n = 0; tr->dirty = 0;
            for (int i = 0; i < NVOICE; i++) tr->v[i].active = 0;
            if (tr->len <= 0) tr->playing = 0;
            if (old) atomic_store(&s->retired_buf[t], old);
        }
        if (!tr->gcap) {
            tr->gcap = s->mem.gcap[t]; tr->dl = s->mem.dl[t]; tr->rv = s->mem.rv[t];
            tr->ap = s->mem.ap[t];
        }
        mods_block(s, tr, frames);

        const float *src = (int)tr->eff[P_SRC] == 1 ? s->last_master : in;
        material_block(s, tr, src, tb, frames);

        /* follower & idle detection */
        float pk = 0;
        for (int i = 0; i < frames * 2; i++) { float a = fabsf(tb[i]); if (a > pk) pk = a; }
        tr->follow = tr->follow * 0.9f + pk * 0.1f;
        int active = pk > 1e-5f || tr->playing || tr->rec;
        tr->idle_blocks = active ? 0 : tr->idle_blocks + 1;
        /* keep effects running for tails (~12 s) unless frozen */
        int frz = (int)tr->eff[P_FREEZE] || (int)tr->eff[P_GFREEZE] || s->freeze_all;
        if (tr->idle_blocks > 4200 && !frz) { tr->peak *= 0.9f; continue; }

        granular_block(s, tr, tb, frames, s->freeze_all);
        filter_block(s, tr, tb, frames);
        color_block(s, tr, tb, frames);
        space_block(s, tr, tb, frames, s->freeze_all);

        /* mixer: DJ filter, level, pan, mute */
        float djf = tr->eff[P_DJF];
        if (fabsf(djf) > 0.02f) {
            svfc_t c;
            float fc = djf < 0 ? 20000.f * powf(2.f, djf * 9.5f) : 20.f * powf(2.f, djf * 9.5f);
            svf_coef(&c, fc, 0.35f);
            for (int i = 0; i < frames; i++)
                for (int ch = 0; ch < 2; ch++) {
                    float bp, hp, lp = svf_tick(&tr->dj[ch], &c, tb[i * 2 + ch], &bp, &hp);
                    tb[i * 2 + ch] = djf < 0 ? lp : hp;
                }
        }
        float lv = tr->eff[P_LEVEL]; lv = 2.f * lv * lv;
        float pan = tr->eff[P_PAN];
        float gl = lv * cosf((pan + 1.f) * (float)M_PI * 0.25f) * 1.4142f;
        float gr = lv * sinf((pan + 1.f) * (float)M_PI * 0.25f) * 1.4142f;
        float mt = (int)tr->eff[P_MUTE] ? 0.f : 1.f;
        float tpk = 0;
        for (int i = 0; i < frames; i++) {
            float k = (float)i / frames;
            tr->mute_g += (mt - tr->mute_g) * 0.01f;
            float l = tb[i * 2] * lerpf(tr->g_prev_l, gl, k) * tr->mute_g;
            float r = tb[i * 2 + 1] * lerpf(tr->g_prev_r, gr, k) * tr->mute_g;
            mix[i * 2] += l; mix[i * 2 + 1] += r;
            float a = fabsf(l) > fabsf(r) ? fabsf(l) : fabsf(r);
            if (a > tpk) tpk = a;
        }
        tr->g_prev_l = gl; tr->g_prev_r = gr;
        tr->peak = tpk > tr->peak ? tpk : tr->peak * 0.92f;
    }

    punch_block(s, mix, frames);

    /* master: compressor -> drive -> out */
    float c = s->g[G_COMP];
    float thr = db2lin(-6.f - 24.f * c), ratio = 2.f + 4.f * c, makeup = 1.f + c * 1.2f;
    float ca = op_coef(1.f / 0.01f), cr = op_coef(1.f / 0.15f);
    float drv = 1.f + s->g[G_DRIVE] * 6.f;
    float og = s->g[G_OUT] * 1.25f;
    if (s->mcomp_g <= 0) s->mcomp_g = 1.f;
    for (int i = 0; i < frames; i++) {
        float l = mix[i * 2], r = mix[i * 2 + 1];
        if (c > 0.001f) {
            float lvl = fabsf(l) > fabsf(r) ? fabsf(l) : fabsf(r);
            /* peak-hold detector: holding ~25 ms stops the envelope from
             * rippling at bass frequencies (which modulates gain = distortion) */
            if (lvl > s->mcomp_env) { s->mcomp_env += ca * (lvl - s->mcomp_env); s->mcomp_hold = 1100; }
            else if (s->mcomp_hold > 0) s->mcomp_hold--;
            else s->mcomp_env += cr * (lvl - s->mcomp_env);
            if ((i & 15) == 0) s->mcomp_g = s->mcomp_env > thr ? powf(s->mcomp_env / thr, 1.f / ratio - 1.f) * makeup : makeup;
            l *= s->mcomp_g; r *= s->mcomp_g;
        }
        if (s->g[G_DRIVE] > 0.001f) { l = softclip(l * drv) / softclip(drv) ; r = softclip(r * drv) / softclip(drv); }
        l *= og; r *= og;
        /* DC blocker (~7 Hz) */
        float yl = l - s->dc_x[0] + 0.999f * s->dc_y[0]; s->dc_x[0] = l; s->dc_y[0] = yl; l = yl;
        float yr = r - s->dc_x[1] + 0.999f * s->dc_y[1]; s->dc_x[1] = r; s->dc_y[1] = yr; r = yr;
        l = knee_clip(l, 0.89f, 0.999f); r = knee_clip(r, 0.89f, 0.999f);
        s->last_master[i * 2] = l; s->last_master[i * 2 + 1] = r;
        out[i * 2] = (int16_t)(clampf(l, -1.f, 1.f) * 32767.f);
        out[i * 2 + 1] = (int16_t)(clampf(r, -1.f, 1.f) * 32767.f);
    }
}

static int get_error(void *inst, char *buf, int len) { (void)inst; (void)buf; (void)len; return 0; }

static plugin_api_v2_t g_api = {
    .api_version = MOVE_PLUGIN_API_VERSION_2,
    .create_instance = create_instance,
    .destroy_instance = destroy_instance,
    .on_midi = on_midi,
    .set_param = set_param,
    .get_param = get_param,
    .get_error = get_error,
    .render_block = render_block,
};

plugin_api_v2_t *move_plugin_init_v2(const host_api_v1_t *host) {
    g_host = host;
#ifndef SCULPT_WEB
    pthread_once(&g_win_once, win_init);
#else
    win_init();
#endif
    return &g_api;
}

#ifdef SCULPT_WEB
/* ------------------------------------------------------------------ */
/* Browser build: exports for the simulator page                       */
/* ------------------------------------------------------------------ */
#define WEB_EXPORT(n) __attribute__((export_name(n)))

static host_api_v1_t g_web_host;
static uint8_t g_web_mailbox[4096];
static float g_web_bpm = 120.f;
static float web_get_bpm(void) { return g_web_bpm; }
static sculpt_t *g_web;
static int16_t g_web_out[BLOCK * 2];
static char g_web_key[256];
static char g_web_val[65536];
static char g_web_res[65536];

WEB_EXPORT("web_init") int web_init(void) {
    memset(&g_web_host, 0, sizeof g_web_host);
    g_web_host.api_version = 1; g_web_host.sample_rate = 44100; g_web_host.frames_per_block = BLOCK;
    g_web_host.mapped_memory = g_web_mailbox;
    g_web_host.audio_out_offset = MOVE_AUDIO_OUT_OFFSET;
    g_web_host.audio_in_offset = MOVE_AUDIO_IN_OFFSET;
    g_web_host.get_bpm = web_get_bpm;
    move_plugin_init_v2(&g_web_host);
    g_web = create_instance("/web", NULL);
    return g_web != NULL;
}
WEB_EXPORT("web_in_ptr") int16_t *web_in_ptr(void) { return (int16_t *)(g_web_mailbox + MOVE_AUDIO_IN_OFFSET); }
WEB_EXPORT("web_out_ptr") int16_t *web_out_ptr(void) { return g_web_out; }
WEB_EXPORT("web_key_ptr") char *web_key_ptr(void) { return g_web_key; }
WEB_EXPORT("web_val_ptr") char *web_val_ptr(void) { return g_web_val; }
WEB_EXPORT("web_res_ptr") char *web_res_ptr(void) { return g_web_res; }
WEB_EXPORT("web_render") void web_render(void) { render_block(g_web, g_web_out, BLOCK); }
WEB_EXPORT("web_poll") int web_poll(void) { return worker_poll(g_web); }
WEB_EXPORT("web_set_bpm") void web_set_bpm(float b) { g_web_bpm = b; }
WEB_EXPORT("web_set_param") void web_set_param(void) { set_param(g_web, g_web_key, g_web_val); }
WEB_EXPORT("web_get_param") int web_get_param(void) { return get_param(g_web, g_web_key, g_web_res, sizeof g_web_res); }
WEB_EXPORT("web_midi") void web_midi(int st, int d1, int d2, int source) {
    uint8_t m[3] = { (uint8_t)st, (uint8_t)d1, (uint8_t)d2 };
    on_midi(g_web, m, 3, source);
}
WEB_EXPORT("web_set_nfiles") void web_set_nfiles(int n) { atomic_store(&g_web->nfiles, n); }
/* Allocate a track buffer for JS to fill with interleaved stereo float PCM. */
WEB_EXPORT("web_alloc_track") float *web_alloc_track(void) { return calloc((size_t)TCAP * 2, sizeof(float)); }
WEB_EXPORT("web_track_cap") int web_track_cap(void) { return TCAP; }
/* Hand a filled buffer (from web_alloc_track) to a track; name in key buffer. */
WEB_EXPORT("web_commit_track") void web_commit_track(int t, float *buf, int frames) {
    if (t < 0 || t >= NT || !buf) return;
    if (frames > TCAP) frames = TCAP;
    compute_wave(g_web, t, buf, frames);
    set_track_name(g_web, t, g_web_key);
    publish_buf(g_web, t, buf, frames);
}
WEB_EXPORT("web_track_buf") float *web_track_buf(int t) { return (t >= 0 && t < NT) ? g_web->t[t].buf : NULL; }
WEB_EXPORT("web_track_len") int web_track_len(int t) { return (t >= 0 && t < NT) ? g_web->t[t].len : 0; }
#endif
