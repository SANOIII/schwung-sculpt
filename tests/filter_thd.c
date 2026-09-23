/* Low-end linearity check for the filter bank: sine / kick input through the
 * live-input path, reports THD and peak headroom for several settings. */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <unistd.h>
#include "../src/dsp/plugin_api_v1.h"
static uint8_t mb[8192];
static float bpm(void) { return 120.f; }
static plugin_api_v2_t *api; static void *inst;
static void cmd(const char *c) { api->set_param(inst, "cmd", c); }
static double ph; static long fr;
static int mode; static float freq, amp;
static float input(void) {
    if (mode == 0) { ph += 2 * M_PI * freq / 44100; return amp * sin(ph); }
    double t = fmod(fr / 44100.0, 0.5);   /* 808-ish kick every 0.5 s */
    return amp * sin(2 * M_PI * (45 * t + 110 * (1 - exp(-t * 25)) / 25)) * exp(-t * 5);
}
/* render secs, return output (left) in buf */
static int render(float secs, float *out) {
    int n = (int)(secs * 44100 / 128), k = 0;
    for (int b = 0; b < n; b++) {
        int16_t *ain = (int16_t *)(mb + MOVE_AUDIO_IN_OFFSET);
        for (int i = 0; i < 128; i++, fr++) { float x = input(); ain[i * 2] = ain[i * 2 + 1] = (int16_t)(x * 32767); }
        int16_t o[256]; api->render_block(inst, o, 128);
        if (out) for (int i = 0; i < 128; i++) out[k++] = o[i * 2] / 32768.f;
    }
    return k;
}
static double thd(const float *x, int n, float f0) {   /* Goertzel on harmonics 1..8 */
    double p[9];
    for (int h = 1; h <= 8; h++) {
        double w = 2 * M_PI * f0 * h / 44100, c = 2 * cos(w), s1 = 0, s2 = 0;
        for (int i = 0; i < n; i++) { double win = 0.5 - 0.5 * cos(2 * M_PI * i / (n - 1)); double s0 = x[i] * win + c * s1 - s2; s2 = s1; s1 = s0; }
        p[h] = s1 * s1 + s2 * s2 - c * s1 * s2;
    }
    double hs = 0; for (int h = 2; h <= 8; h++) hs += p[h];
    return 100 * sqrt(hs / p[1]);
}
int main(int argc, char **argv) {
    setenv("SCULPT_SAMPLE_ROOT", "/tmp/sculpt_thd_s", 1); setenv("SCULPT_DATA_DIR", "/tmp/sculpt_thd_d", 1);
    system("rm -rf /tmp/sculpt_thd_d; mkdir -p /tmp/sculpt_thd_s");
    void *h = dlopen(argc > 1 ? argv[1] : "build/sculpt_host.so", RTLD_NOW); if (!h) { puts(dlerror()); return 1; }
    static host_api_v1_t host; host.sample_rate = 44100; host.frames_per_block = 128; host.mapped_memory = mb;
    host.audio_in_offset = MOVE_AUDIO_IN_OFFSET; host.get_bpm = bpm;
    api = ((move_plugin_init_v2_fn)dlsym(h, MOVE_PLUGIN_INIT_V2_SYMBOL))(&host);
    inst = api->create_instance("/tmp", NULL);
    for (int i = 0; i < 100; i++) { usleep(10000); char u[512]; api->get_param(inst, "ui", u, 512); if (u[0] == '1') break; }
    cmd("g 0 0\ng 3 0.8\nk 0 mon 1\nk 0 ingain 0.5\nk 0 level 0.714");
    const char *setups[][2] = {
        { "bypass",               "k 0 fnoise 0" },
        { "open bank",            "k 0 fnoise 0.002" },
        { "LP 500",               "k 0 fcut 0.465" },
        { "LP 200 res .6",        "k 0 fcut 0.333\nk 0 fres 0.6" },
        { "BP 100 steep",         "k 0 fcut 0.233\nk 0 fres 0\nk 0 fmorph 0.5\nk 0 fslope 1" },
        { "LP 800 decay .6",      "k 0 fcut 0.534\nk 0 fmorph 0\nk 0 fslope 0.43\nk 0 fdecay 0.6" },
        { "minor res decay .8",   "k 0 fcut 0.6\nk 0 fmorph 0.5\nk 0 fscale 3\nk 0 fdecay 0.8\nk 0 fres 0.5" },
    };
    float *buf = malloc(sizeof(float) * 44100 * 4);
    int bad = 0;
    printf("%-22s %8s %8s %8s %8s | %s\n", "setting", "40Hz", "60Hz", "100Hz", "200Hz", "kick peak in/out");
    for (int s = 0; s < 7; s++) {
        cmd(setups[s][1]);
        printf("%-22s", setups[s][0]);
        double worst = 0;
        float fs[4] = { 40, 60, 100, 200 };
        for (int k = 0; k < 4; k++) {
            mode = 0; freq = fs[k]; amp = 0.7f; ph = 0;
            render(1.5f, NULL);
            int n = render(1.0f, buf);
            float opk = 0; for (int i = 0; i < n; i++) opk = fmaxf(opk, fabsf(buf[i]));
            double d = opk < 0.01f ? 0 : thd(buf, n, freq); if (d > worst) worst = d;
            if (opk < 0.01f) { printf(" %8s", "(cut)"); continue; }
            if (getenv("THD_VERBOSE")) { float p = 0; for (int i = 0; i < n; i++) p = fmaxf(p, fabsf(buf[i])); fprintf(stderr, "[%s %.0fHz peak %.3f] ", setups[s][0], freq, p); }
            printf(" %7.2f%%", d);
        }
        mode = 1; amp = 0.9f; fr = 0; render(1.0f, NULL);
        int n = render(2.0f, buf); float pk = 0; for (int i = 0; i < n; i++) if (fabsf(buf[i]) > pk) pk = fabsf(buf[i]);
        printf(" | 0.90 / %.2f\n", pk);
        if (s < 6 && worst > 1.0) bad++;
    }
    api->destroy_instance(inst);
    printf(bad ? "\n%d LOW-END FAILURES\n" : "\nLOW END OK\n", bad);
    return bad ? 1 : 0;
}
