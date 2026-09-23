/* Per-scenario CPU benchmark: how much of the 2.9 ms audio block Sculpt uses.
 * Build the plugin with the release flags (-O3 -ffast-math) for meaningful numbers. */
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
static int cmpd(const void *a, const void *b) { double x = *(const double *)a, y = *(const double *)b; return x < y ? -1 : x > y; }
static double tl[20000];
static void bench(const char *name, float secs) {
    int n = (int)(secs * 44100 / 128);
    for (int b = 0; b < 200; b++) { int16_t o[256]; api->render_block(inst, o, 128); }   /* settle */
    for (int b = 0; b < n; b++) {
        int16_t *ain = (int16_t *)(mb + MOVE_AUDIO_IN_OFFSET);
        for (int i = 0; i < 256; i++) { rs = rs * 1664525u + 1013904223u; ain[i] = (int16_t)((int)(rs >> 16) - 32768) / 8; }
        int16_t o[256]; struct timespec a, c;
        clock_gettime(CLOCK_MONOTONIC, &a); api->render_block(inst, o, 128); clock_gettime(CLOCK_MONOTONIC, &c);
        tl[b] = (c.tv_sec - a.tv_sec) * 1e6 + (c.tv_nsec - a.tv_nsec) / 1e3;
    }
    double sum = 0; for (int b = 0; b < n; b++) sum += tl[b];
    qsort(tl, n, sizeof(double), cmpd);
    printf("%-44s avg %6.1f  p99 %6.1f us\n", name, sum / n, tl[n * 99 / 100]);
}
static void all(const char *fmt) { for (int t = 0; t < 4; t++) { char c[1024]; snprintf(c, sizeof c, fmt, t, t, t, t, t, t, t, t, t, t, t, t, t, t, t, t); cmd(c); } }
int main(int argc, char **argv) {
    setenv("SCULPT_SAMPLE_ROOT", "/tmp/sculpt_bench_s", 1); setenv("SCULPT_DATA_DIR", "/tmp/sculpt_bench_d", 1);
    if (system("rm -rf /tmp/sculpt_bench_d /tmp/sculpt_bench_s; mkdir -p /tmp/sculpt_bench_s")) {}
    /* a 4 s stereo loop to load everywhere */
    FILE *f = fopen("/tmp/sculpt_bench_s/loop.wav", "wb"); int fr = 176400; uint32_t v; uint16_t w;
    fwrite("RIFF", 1, 4, f); v = 36 + fr * 4; fwrite(&v, 4, 1, f); fwrite("WAVEfmt ", 1, 8, f); v = 16; fwrite(&v, 4, 1, f);
    w = 1; fwrite(&w, 2, 1, f); w = 2; fwrite(&w, 2, 1, f); v = 44100; fwrite(&v, 4, 1, f); v = 176400; fwrite(&v, 4, 1, f);
    w = 4; fwrite(&w, 2, 1, f); w = 16; fwrite(&w, 2, 1, f); fwrite("data", 1, 4, f); v = fr * 4; fwrite(&v, 4, 1, f);
    for (int i = 0; i < fr * 2; i++) { int16_t s = (int16_t)(8000 * sin(i * 0.013) + (rand() % 4000 - 2000)); fwrite(&s, 2, 1, f); }
    fclose(f);
    void *h = dlopen(argc > 1 ? argv[1] : "build/sculpt_fast.so", RTLD_NOW); if (!h) { puts(dlerror()); return 1; }
    static host_api_v1_t host; host.sample_rate = 44100; host.frames_per_block = 128; host.mapped_memory = mb;
    host.audio_in_offset = MOVE_AUDIO_IN_OFFSET; host.get_bpm = bpm;
    api = ((move_plugin_init_v2_fn)dlsym(h, MOVE_PLUGIN_INIT_V2_SYMBOL))(&host);
    inst = api->create_instance("/tmp", NULL);
    for (int i = 0; i < 200; i++) { usleep(10000); char u[512]; api->get_param(inst, "ui", u, 512); if (u[0] == '1') break; }
    usleep(300000);
    bench("empty (nothing loaded)", 3);
    all("load %d 0"); usleep(600000); { int16_t o[256]; api->render_block(inst, o, 128); }
    cmd("g 0 0"); bench("4 tracks loaded, stopped", 3);
    cmd("playall 1"); bench("4 tape tracks playing, no FX", 3);
    cmd("g 0 0.3"); bench("  + master comp", 3);
    all("k %d gmix 0.5\nk %d gdens 0.5\nk %d gsize 0.5"); bench("  + granular (moderate) x4", 3);
    all("k %d gdens 1\nk %d gsize 1"); bench("  + granular maxed (20 grains) x4", 3);
    all("k %d fcut 0.6\nk %d fres 0.3\nk %d fdecay 0.4"); bench("  + 48-band filter bank x4", 3);
    all("k %d fwave 0.6\nk %d fnoise 0.3\nk %d fspread 0.5"); bench("  + bank waves/noise/spread x4", 3);
    all("k %d cdrive 0.5\nk %d cbits 0.4\nk %d crate 0.3\nk %d ccomp 0.5\nk %d cnoise 0.3"); bench("  + color everything x4", 3);
    all("k %d dmix 0.4\nk %d rmix 0.4\nk %d dtype 1"); bench("  + tape delay + reverb x4", 3);
    all("k %d m1type 0\nk %d m1tgt 17\nk %d m1depth 0.2\nk %d m2type 4\nk %d m2tgt 9\nk %d m2depth 0.2\nk %d m3type 5\nk %d m3tgt 32\nk %d m3depth 0.2\nk %d m4type 1\nk %d m4tgt 20\nk %d m4depth 0.2");
    bench("  + 4 mods per track (WORST CASE)", 3);
    cmd("punch 0 1"); bench("  + stutter held", 1); cmd("punch 0 0");
    all("k %d gmix 0\nk %d fcut 1\nk %d fres 0\nk %d fdecay 0\nk %d fwave 0\nk %d fnoise 0\nk %d fspread 0");
    bench("typical: tape+color+space+mods, no grains/bank", 3);
    api->destroy_instance(inst);
    return 0;
}
