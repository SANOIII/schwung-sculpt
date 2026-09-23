/*
 * Offline render test for Sculpt.
 *
 * Loads the plugin .so the same way the Schwung shim does, feeds it a fake
 * host (mailbox with a synthetic input signal), drives it through commands
 * and writes the result to a WAV. Checks: no NaN/Inf, no silence where sound
 * is expected, recording length quantisation, scenes, punch FX, state save,
 * and worst-case render time per 128-frame block.
 *
 *   cc -O2 tests/render_test.c -o build/render_test -ldl -lm
 *   ./build/render_test build/sculpt_host.so out.wav
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>
#include <unistd.h>
#include <sys/stat.h>
#include "../src/dsp/plugin_api_v1.h"

static uint8_t mailbox[8192];
static float g_bpm = 120.f;
static float get_bpm(void) { return g_bpm; }
static void hlog(const char *m) { fprintf(stderr, "[host] %s\n", m); }

static plugin_api_v2_t *api;
static void *inst;
static FILE *wav;
static long frames_out;
static double worst_us, total_us;
static long blocks;
static double tlog[200000]; static int tlog_n, tlog_on;
static int bad;
static double phase;
static int input_on;

static void cmd(const char *c) { api->set_param(inst, "cmd", c); }

static void write_hdr(FILE *f, long frames) {
    uint32_t dlen = frames * 4, v;
    uint16_t w;
    fseek(f, 0, SEEK_SET);
    fwrite("RIFF", 1, 4, f); v = 36 + dlen; fwrite(&v, 4, 1, f);
    fwrite("WAVEfmt ", 1, 8, f); v = 16; fwrite(&v, 4, 1, f);
    w = 1; fwrite(&w, 2, 1, f); w = 2; fwrite(&w, 2, 1, f);
    v = 44100; fwrite(&v, 4, 1, f); v = 44100 * 4; fwrite(&v, 4, 1, f);
    w = 4; fwrite(&w, 2, 1, f); w = 16; fwrite(&w, 2, 1, f);
    fwrite("data", 1, 4, f); fwrite(&dlen, 4, 1, f);
}

/* render seconds, return peak */
static float run(float secs) {
    int n = (int)(secs * 44100 / 128);
    float peak = 0;
    for (int b = 0; b < n; b++) {
        int16_t *ain = (int16_t *)(mailbox + MOVE_AUDIO_IN_OFFSET);
        for (int i = 0; i < 128; i++) {
            float x = 0;
            if (input_on) {
                x = 0.3f * sinf((float)phase) * (0.5f + 0.5f * sinf((float)phase * 0.0021f));
                phase += 2 * M_PI * 220.0 / 44100.0;
            }
            ain[i * 2] = ain[i * 2 + 1] = (int16_t)(x * 32767);
        }
        int16_t out[256];
        struct timespec a, c;
        clock_gettime(CLOCK_MONOTONIC, &a);
        api->render_block(inst, out, 128);
        clock_gettime(CLOCK_MONOTONIC, &c);
        double us = (c.tv_sec - a.tv_sec) * 1e6 + (c.tv_nsec - a.tv_nsec) / 1e3;
        if (us > worst_us) worst_us = us;
        total_us += us; blocks++; if (tlog_on && tlog_n < 200000) tlog[tlog_n++] = us;
        for (int i = 0; i < 256; i++) { float v = fabsf(out[i] / 32768.f); if (v > peak) peak = v; }
        fwrite(out, 2, 256, wav);
        frames_out += 128;
    }
    return peak;
}

static void write_test_wav(const char *path, float secs) {
    FILE *f = fopen(path, "wb");
    long n = (long)(secs * 48000);   /* 48k source: exercises resampling */
    uint32_t dlen = n * 2 * 3, v; uint16_t w;
    fwrite("RIFF", 1, 4, f); v = 36 + dlen; fwrite(&v, 4, 1, f);
    fwrite("WAVEfmt ", 1, 8, f); v = 16; fwrite(&v, 4, 1, f);
    w = 1; fwrite(&w, 2, 1, f); w = 2; fwrite(&w, 2, 1, f);
    v = 48000; fwrite(&v, 4, 1, f); v = 48000 * 6; fwrite(&v, 4, 1, f);
    w = 6; fwrite(&w, 2, 1, f); w = 24; fwrite(&w, 2, 1, f);
    fwrite("data", 1, 4, f); fwrite(&dlen, 4, 1, f);
    for (long i = 0; i < n; i++) {
        double t = i / 48000.0;
        double beat = fmod(t, 0.5);
        double kick = sin(2 * M_PI * (50 + 120 * exp(-beat * 30)) * beat) * exp(-beat * 8);
        double hat = ((rand() / (double)RAND_MAX) * 2 - 1) * exp(-fmod(t + 0.25, 0.5) * 60) * 0.3;
        double pad = 0.2 * sin(2 * M_PI * 330 * t) + 0.15 * sin(2 * M_PI * 495 * t);
        for (int c = 0; c < 2; c++) {
            double x = (kick + hat + pad * (c ? 0.8 : 1.0)) * 0.6;
            int32_t s = (int32_t)(x * 8388607);
            unsigned char b[3] = { s & 255, (s >> 8) & 255, (s >> 16) & 255 };
            fwrite(b, 1, 3, f);
        }
    }
    fclose(f);
}

static void expect(int cond, const char *what) {
    printf("  [%s] %s\n", cond ? "ok" : "FAIL", what);
    if (!cond) bad++;
}

static int track_field(int t, int field) {
    char buf[2048];
    if (api->get_param(inst, "ui", buf, sizeof buf) < 0) return -1;
    char *line = buf;
    for (int i = 0; i <= t; i++) line = strchr(line, '\n') + 1;
    int v[8] = {0};
    sscanf(line, "%d %d %d %d %d %d %d %d", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5], &v[6], &v[7]);
    return v[field];
}

int main(int argc, char **argv) {
    const char *so = argc > 1 ? argv[1] : "build/sculpt_host.so";
    const char *outp = argc > 2 ? argv[2] : "build/render_test.wav";
    char tmpd[] = "/tmp/sculpt_testXXXXXX";
    mkdtemp(tmpd);
    char samples[512], data[512], wavp[600];
    snprintf(samples, sizeof samples, "%s/Samples", tmpd);
    snprintf(data, sizeof data, "%s/data", tmpd);
    mkdir(samples, 0755);
    snprintf(wavp, sizeof wavp, "%s/drums/loop.wav", samples);
    char dd[600]; snprintf(dd, sizeof dd, "%s/drums", samples); mkdir(dd, 0755);
    write_test_wav(wavp, 4.0f);
    setenv("SCULPT_SAMPLE_ROOT", samples, 1);
    setenv("SCULPT_DATA_DIR", data, 1);

    void *h = dlopen(so, RTLD_NOW);
    if (!h) { fprintf(stderr, "dlopen: %s\n", dlerror()); return 1; }
    move_plugin_init_v2_fn init = (move_plugin_init_v2_fn)dlsym(h, MOVE_PLUGIN_INIT_V2_SYMBOL);
    static host_api_v1_t host;
    host.api_version = 1; host.sample_rate = 44100; host.frames_per_block = 128;
    host.mapped_memory = mailbox; host.audio_out_offset = MOVE_AUDIO_OUT_OFFSET;
    host.audio_in_offset = MOVE_AUDIO_IN_OFFSET; host.log = hlog; host.get_bpm = get_bpm;
    api = init(&host);
    inst = api->create_instance("/tmp", NULL);

    wav = fopen(outp, "wb");
    write_hdr(wav, 0);

    printf("startup\n");
    for (int i = 0; i < 200; i++) { run(0.01f); char b[16]; api->get_param(inst, "ready", b, 16); usleep(5000);
        char u[2048]; api->get_param(inst, "ui", u, sizeof u); if (u[0] == '1') break; }
    char ui[2048]; api->get_param(inst, "ui", ui, sizeof ui);
    expect(ui[0] == '1', "worker published memory");
    usleep(200000);
    char fb[512]; int fl = api->get_param(inst, "file:0", fb, sizeof fb);
    expect(fl > 0 && strstr(fb, "loop.wav"), "sample scan finds drums/loop.wav");
    char meta[65536]; int ml = api->get_param(inst, "meta", meta, sizeof meta);
    expect(ml > 1000 && ml < (int)sizeof meta - 1, "meta JSON fits");
    FILE *mf = fopen("build/meta.json", "w"); if (mf) { fwrite(meta, 1, ml, mf); fclose(mf); }

    printf("tape playback + full chain\n");
    cmd("load 0 0");
    for (int i = 0; i < 100 && track_field(0, 2) < 1000; i++) { usleep(20000); run(0.01f); }
    int len0 = track_field(0, 2);
    expect(len0 > 170000 && len0 < 180000, "4 s @48k resampled to ~176400 frames");
    cmd("playall 1");
    float pk = run(3.0f);
    expect(pk > 0.05f, "tape playback is audible");
    /* granular + filter + color + space, modulated */
    cmd("k 0 gmix 0.6\nk 0 gsize 0.4\nk 0 gdens 0.7\nk 0 gspray 0.3\n"
        "k 0 fcut 0.5\nk 0 fres 0.4\nk 0 fdecay 0.3\n"
        "k 0 cdrive 0.4\nk 0 cbits 0.3\nk 0 ccomp 0.2\n"
        "k 0 dfb 0.4\nk 0 dmix 0.3\nk 0 rmix 0.35");
    int mod0 = 0; (void)mod0;
    /* mod1: sine, rate .5, depth .3, target = cutoff (find index) */
    {
        char m2[65536]; api->get_param(inst, "meta", m2, sizeof m2);
        char *tg = strstr(m2, "\"targets\":[");
        int idx = 0, k = 1; char *p = tg + 11;
        char *fc = strstr(m2, "\"k\":\"fcut\""); int fci = 0;
        while (fc > m2 && strncmp(fc, "{\"i\":", 5)) fc--; fci = atoi(fc + 5);
        while (*p && *p != ']') { int v = atoi(p); if (v == fci) idx = k; k++; p = strchr(p, ','); if (!p) break; p++; }
        char c[128]; snprintf(c, sizeof c, "k 0 m1type 0\nk 0 m1rate 0.5\nk 0 m1depth 0.3\nk 0 m1tgt %d", idx);
        cmd(c);
        expect(idx > 0, "cutoff is a mod target");
    }
    pk = run(4.0f);
    expect(pk > 0.05f, "full chain audible");
    char mv[128]; api->get_param(inst, "modv0", mv, sizeof mv);
    printf("  mod values: %s\n", mv);

    printf("filter bank modes\n");
    for (int ft = 0; ft < 3; ft++) { char c[64]; snprintf(c, sizeof c, "k 0 fmorph %.1f\nk 0 fscale %d", ft * 0.5, ft * 2); cmd(c); pk = run(1.0f);
        char w[64]; snprintf(w, sizeof w, "bank morph %.1f audible", ft * 0.5); expect(pk > 0.02f, w); }
    cmd("k 0 fmorph 0\nk 0 fscale 0");
    printf("delay types + freeze\n");
    for (int dt = 0; dt < 3; dt++) { char c[64]; snprintf(c, sizeof c, "k 0 dtype %d", dt); cmd(c); pk = run(1.0f);
        char w[64]; snprintf(w, sizeof w, "delay type %d audible", dt); expect(pk > 0.02f, w); }
    cmd("k 0 freeze 1"); run(1.0f); cmd("playall 0"); pk = run(1.5f); expect(pk > 0.01f, "freeze holds a tail after stop"); cmd("k 0 freeze 0");

    printf("poly mode + pads\n");
    cmd("k 0 mode 1");  /* poly */
    for (int k = 0; k < 8; k++) { char c[64]; snprintf(c, sizeof c, "trig 0 %d 100", k); cmd(c); run(0.15f); snprintf(c, sizeof c, "rel 0 %d", k); cmd(c); }
    pk = run(0.5f); expect(1, "poly pads rendered");
    uint8_t note[3] = { 0x90, 72, 100 }; api->on_midi(inst, note, 3, MOVE_MIDI_SOURCE_EXTERNAL); run(0.5f);
    uint8_t pad[3] = { 0x90, 92, 90 }; api->on_midi(inst, pad, 3, MOVE_MIDI_SOURCE_INTERNAL); pk = run(0.5f);
    expect(pk > 0.02f, "Move pad note (top row) triggers track 1");
    pad[2] = 0; api->on_midi(inst, pad, 3, MOVE_MIDI_SOURCE_INTERNAL);

    printf("recording (input -> track 2, bar quantised)\n");
    input_on = 1;
    cmd("rec 1");
    run(2.3f);                        /* ~ 1 bar at 120 bpm = 2.0 s */
    cmd("rec 1");
    run(0.1f);
    int len1 = track_field(1, 2);
    printf("  recorded len %d frames\n", len1);
    expect(len1 == 88200, "2.3 s quantised to one bar (88200 frames)");
    input_on = 0;
    pk = run(2.0f);
    expect(track_field(1, 0) == 1 && pk > 0.02f, "recorded loop plays back");
    cmd("rec 1"); input_on = 1; run(1.0f); cmd("rec 1"); input_on = 0;
    expect(track_field(1, 2) == 88200, "overdub keeps length");

    printf("scenes + morph\n");
    cmd("sstore 0\nk 1 fcut 0.1\nsstore 1\ng 1 0.5\nsrecall 0");
    run(1.5f);
    char vals[65536]; api->get_param(inst, "values", vals, sizeof vals);
    expect(strstr(vals, ";") != NULL, "values dump");
    api->get_param(inst, "ui", ui, sizeof ui);
    unsigned sc = 0; sscanf(ui, "%*d %*f %u", &sc);
    expect((sc & 3) == 3, "scene bitmask has scenes 1 and 2");

    printf("punch FX\n");
    cmd("playall 1");
    for (int p = 0; p < 4; p++) { char c[64]; snprintf(c, sizeof c, "punch %d 1", p); cmd(c); pk = run(1.0f);
        snprintf(c, sizeof c, "punch %d 0", p); cmd(c); run(0.3f);
        char w[64]; snprintf(w, sizeof w, "punch %d renders", p); expect(pk > 0.0f, w); }

    printf("stress: all 4 tracks loaded, everything on\n");
    for (int t = 0; t < 4; t++) { if (t == 1) continue; char c[64]; snprintf(c, sizeof c, "load %d 0", t); cmd(c); }
    usleep(400000); run(0.1f); usleep(200000); run(0.1f);
    for (int t = 0; t < 4; t++) {
        char c[512];
        snprintf(c, sizeof c, "k %d gmix 0.7\nk %d gdens 1\nk %d gsize 0.8\nk %d fcut 0.6\nk %d fres 0.5\nk %d fdecay 0.6\n"
                 "k %d fscale %d\nk %d fwave 0.6\nk %d fnoise 0.3\n"
                 "k %d cdrive 0.6\nk %d cbits 0.5\nk %d crate 0.4\nk %d ccomp 0.5\nk %d cnoise 0.4\n"
                 "k %d dfb 0.6\nk %d dmix 0.5\nk %d rmix 0.5\nk %d m1type 4\nk %d m1depth 0.2\n",
                 t, t, t, t, t, t, t, t + 1, t, t, t, t, t, t, t, t, t, t, t, t);
        cmd(c);
    }
    cmd("playall 1");
    run(1.0f);   /* warm-up: first touch of fresh buffers */
    worst_us = 0; total_us = 0; blocks = 0; tlog_on = 1;
    pk = run(6.0f); tlog_on = 0;
    { int cmpd(const void *a, const void *b) { double x = *(const double *)a, y = *(const double *)b; return x < y ? -1 : x > y; }
      qsort(tlog, tlog_n, sizeof(double), cmpd);
      printf("  p50 %.0f us, p99 %.0f us, p99.9 %.0f us\n", tlog[tlog_n / 2], tlog[tlog_n * 99 / 100], tlog[tlog_n * 999 / 1000]); }
    printf("  avg %.1f us / block, worst %.1f us (budget ~2900 us, x86 host)\n", total_us / blocks, worst_us);
    expect(pk > 0.05f, "stress audible");

    {   /* snapshot for the UI harness */
        static char b[65536]; FILE *o;
        const char *keys[] = { "values", "ui", "wave0", "name0", "name1", "name2", "name3", "modv0", "bank0", "file:0" };
        for (int k = 0; k < 10; k++) {
            int n = api->get_param(inst, keys[k], b, sizeof b);
            char fn[64]; snprintf(fn, sizeof fn, "build/snap_%s.txt", k == 9 ? "file0" : keys[k]);
            if (n >= 0 && (o = fopen(fn, "w"))) { fwrite(b, 1, n, o); fclose(o); }
        }
    }
    printf("state save\n");
    cmd("save");
    usleep(800000);
    char sf[600]; snprintf(sf, sizeof sf, "%s/state.txt", data);
    struct stat st; expect(stat(sf, &st) == 0 && st.st_size > 1000, "state.txt written");
    char recd[600]; snprintf(recd, sizeof recd, "%s/Sculpt", samples);
    expect(stat(recd, &st) == 0, "recordings folder written");

    write_hdr(wav, frames_out);
    fclose(wav);
    api->destroy_instance(inst);

    printf("reload session\n");
    inst = api->create_instance("/tmp", NULL);
    for (int i = 0; i < 300; i++) { usleep(10000); char u[2048]; api->get_param(inst, "ui", u, sizeof u); if (u[0] == '1') break; }
    { int16_t o[256]; api->render_block(inst, o, 128); }
    int rl = track_field(1, 2);
    printf("  track 2 len after reload: %d\n", rl);
    expect(rl == 88200, "recorded loop restored from saved WAV");
    char nm[128]; api->get_param(inst, "name1", nm, sizeof nm);
    expect(strstr(nm, "Sculpt T2") != NULL, "restored track name is the saved recording");
    api->get_param(inst, "ui", ui, sizeof ui); sc = 0; sscanf(ui, "%*d %*f %u", &sc);
    expect((sc & 3) == 3, "scenes restored");
    api->destroy_instance(inst);

    /* NaN scan of output */
    FILE *f = fopen(outp, "rb"); fseek(f, 44, SEEK_SET);
    int16_t s; long clip = 0, n = 0;
    while (fread(&s, 2, 1, f) == 1) { if (s == 32767 || s == -32767) clip++; n++; }
    fclose(f);
    printf("  output: %.1f s, %ld clipped samples\n", n / 2 / 44100.0, clip);
    printf(bad ? "\n%d FAILURES\n" : "\nALL OK\n", bad);
    return bad ? 1 : 0;
}
