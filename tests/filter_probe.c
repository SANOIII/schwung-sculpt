/* Drives the 48-band filter bank with noise/impulses through the live-input
 * path and writes one WAV per scenario for tests/filter_analyze.py. */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>
#include <unistd.h>
#include "../src/dsp/plugin_api_v1.h"
static uint8_t mb[8192];
static float bpm(void) { return 120.f; }
static plugin_api_v2_t *api; static void *inst;
static void cmd(const char *c) { api->set_param(inst, "cmd", c); }
static unsigned rs = 1;
static float wn(void) { rs = rs * 1664525u + 1013904223u; return ((rs >> 8) / 16777216.f) * 2 - 1; }
static double us_tot; static long blocks;
static void run(const char *path, float secs, int impulse) {
    FILE *f = path ? fopen(path, "wb") : NULL;
    int n = (int)(secs * 44100 / 128);
    if (f) { unsigned char h[44] = {0}; fwrite(h, 1, 44, f); }
    long fr = 0;
    for (int b = 0; b < n; b++) {
        int16_t *ain = (int16_t *)(mb + MOVE_AUDIO_IN_OFFSET);
        for (int i = 0; i < 128; i++) {
            float x = impulse ? ((fr + i) % 44100 == 0 ? 0.9f : 0.f) : wn() * 0.08f;
            ain[i * 2] = ain[i * 2 + 1] = (int16_t)(x * 32767);
        }
        int16_t o[256];
        struct timespec a, c; clock_gettime(CLOCK_MONOTONIC, &a);
        api->render_block(inst, o, 128);
        clock_gettime(CLOCK_MONOTONIC, &c);
        us_tot += (c.tv_sec - a.tv_sec) * 1e6 + (c.tv_nsec - a.tv_nsec) / 1e3; blocks++;
        if (f) fwrite(o, 2, 256, f);
        fr += 128;
    }
    if (f) {
        uint32_t dl = fr * 4, v; uint16_t w; fseek(f, 0, SEEK_SET);
        fwrite("RIFF", 1, 4, f); v = 36 + dl; fwrite(&v, 4, 1, f); fwrite("WAVEfmt ", 1, 8, f); v = 16; fwrite(&v, 4, 1, f);
        w = 1; fwrite(&w, 2, 1, f); w = 2; fwrite(&w, 2, 1, f); v = 44100; fwrite(&v, 4, 1, f); v = 176400; fwrite(&v, 4, 1, f);
        w = 4; fwrite(&w, 2, 1, f); w = 16; fwrite(&w, 2, 1, f); fwrite("data", 1, 4, f); fwrite(&dl, 4, 1, f); fclose(f);
    }
}
int main(int argc, char **argv) {
    const char *so = argc > 1 ? argv[1] : "build/sculpt_host.so";
    setenv("SCULPT_SAMPLE_ROOT", "/tmp/sculpt_probe_samples", 1); setenv("SCULPT_DATA_DIR", "/tmp/sculpt_probe_data", 1);
    system("rm -rf /tmp/sculpt_probe_data; mkdir -p /tmp/sculpt_probe_samples");
    void *h = dlopen(so, RTLD_NOW); if (!h) { puts(dlerror()); return 1; }
    static host_api_v1_t host; host.sample_rate = 44100; host.frames_per_block = 128; host.mapped_memory = mb;
    host.audio_in_offset = MOVE_AUDIO_IN_OFFSET; host.audio_out_offset = MOVE_AUDIO_OUT_OFFSET; host.get_bpm = bpm;
    api = ((move_plugin_init_v2_fn)dlsym(h, MOVE_PLUGIN_INIT_V2_SYMBOL))(&host);
    inst = api->create_instance("/tmp", NULL);
    for (int i = 0; i < 100; i++) { usleep(10000); char u[512]; api->get_param(inst, "ui", u, 512); if (u[0] == '1') break; }
    cmd("g 0 0\ng 3 0.8\nk 0 mon 1\nk 0 ingain 0.5\nk 0 level 0.714");   /* unity path, no master comp */
    system("mkdir -p build/fb");
    run("build/fb/dry.wav", 4, 0);
    cmd("k 0 fnoise 0.002"); run(NULL, 0.5, 0); run("build/fb/open.wav", 4, 0); cmd("k 0 fnoise 0");
    cmd("k 0 fcut 0.566"); run(NULL, 0.3, 0); run("build/fb/lp1k.wav", 4, 0);                       /* ~1 kHz */
    cmd("k 0 fmorph 0.5"); run(NULL, 0.3, 0); run("build/fb/bp1k.wav", 4, 0);
    cmd("k 0 fmorph 1"); run(NULL, 0.3, 0); run("build/fb/hp1k.wav", 4, 0);
    cmd("k 0 fmorph 0\nk 0 fres 0.8"); run(NULL, 0.3, 0); run("build/fb/lp1k_res.wav", 4, 0);
    cmd("k 0 fslope 1\nk 0 fres 0"); run(NULL, 0.3, 0); run("build/fb/lp1k_steep.wav", 4, 0);
    cmd("k 0 fslope 0.43\nk 0 fcut 1\nk 0 fmorph 0.5\nk 0 fcut 0.6\nk 0 fmorph 0.5\nk 0 fres 0\nk 0 fdecay 0.7\nk 0 fscale 3\nk 0 fmorph 0.5\nk 0 fslope 0");
    run(NULL, 0.5, 1); run("build/fb/ring_minor.wav", 4, 1);                                         /* impulses every 1 s */
    cmd("k 0 fdecay 0.3"); run(NULL, 1, 1); run("build/fb/ring_short.wav", 3, 1);
    cmd("k 0 fdecay 0.7\nk 0 fscale 0\nk 0 fwave 1\nk 0 fwsize 0.5\nk 0 fnoise 0.7\nk 0 fntex 0.8\nk 0 fspread 0.7");
    run("build/fb/waves_noise.wav", 6, 0);
    /* poly resonator: hold C-Eb-G on track 0 via MIDI notes, Notes scale */
    cmd("k 0 fwave 0\nk 0 fnoise 0\nk 0 fspread 0\nk 0 fscale 8\nk 0 fdecay 0.85\nk 0 fcut 0.6\nk 0 fmorph 0.5\nk 0 fslope 0");
    uint8_t n1[3] = {0x90, 60, 100}, n2[3] = {0x90, 63, 100}, n3[3] = {0x90, 67, 100};
    api->on_midi(inst, n1, 3, 2); api->on_midi(inst, n2, 3, 2); api->on_midi(inst, n3, 3, 2);
    run(NULL, 0.2, 1); run("build/fb/notes_cm.wav", 4, 0);
    /* CPU: all four tracks with the bank running */
    for (int t = 0; t < 4; t++) { char c[256]; snprintf(c, sizeof c, "k %d mon 1\nk %d fdecay 0.6\nk %d fscale 2\nk %d fcut 0.7\nk %d fwave 0.5\nk %d fnoise 0.3", t, t, t, t, t, t); cmd(c); }
    run(NULL, 0.5, 0); us_tot = 0; blocks = 0; run(NULL, 4, 0);
    printf("4-track bank: %.1f us/block\n", us_tot / blocks);
    for (int t = 0; t < 4; t++) { char c[64]; snprintf(c, sizeof c, "k %d fmix 0", t); cmd(c); }
    run(NULL, 0.2, 0); us_tot = 0; blocks = 0; run(NULL, 4, 0);
    printf("4-track bank bypassed: %.1f us/block\n", us_tot / blocks);
    api->destroy_instance(inst);
    return 0;
}
