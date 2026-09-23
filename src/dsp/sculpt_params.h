/*
 * Sculpt — parameter table.
 *
 * One flat table of per-track parameters. The UI pulls this table once
 * (get_param "meta") so the DSP is the single source of truth for names,
 * ranges and display formats.
 */
#ifndef SCULPT_PARAMS_H
#define SCULPT_PARAMS_H

/* Devices (UI pages) */
enum { DEV_MAT = 0, DEV_GRN, DEV_FLT, DEV_COL, DEV_SPC, DEV_MOD, DEV_MIX, DEV_COUNT };

/* Display formats (mirrored in ui.js) */
enum {
    FMT_PCT = 0,   /* 0..1 -> 0..100%            */
    FMT_BIP,       /* -1..1 -> -100..+100        */
    FMT_SEMI,      /* semitones, integer steps    */
    FMT_FREQ,      /* 0..1 -> lo*(hi/lo)^x Hz     */
    FMT_TIME,      /* 0..1 -> lo*(hi/lo)^x ms     */
    FMT_ENUM,      /* index into '|' list         */
    FMT_CENT,      /* cents                       */
    FMT_RATE,      /* mod rate (Hz or sync div)   */
    FMT_TARGET,    /* mod target index            */
    FMT_DTIME,     /* delay time (ms or sync div) */
    FMT_DENS,      /* grain density (/s or div)   */
    FMT_BITS,      /* 0..1 -> bits                */
    FMT_MORPH,     /* 0..1 -> LP .. BP .. HP      */
    FMT_LFO,       /* 0..1 -> lo*(hi/lo)^x Hz, fractional */
    FMT_SLOPE,     /* 0..1 -> 6..48 dB/oct        */
    FMT_DECAY      /* 0 = off, else lo*(hi/lo)^x ms */
};

typedef struct {
    const char *key;
    const char *name;   /* long name, <= 14 chars */
    const char *sname;  /* knob-cell label, <= 5 chars */
    int dev;
    int page;           /* page within device (mods: page = mod index) */
    int slot;           /* knob slot 0..7 */
    float min, max, def;
    int fmt;
    float lo, hi;       /* exp mapping for FREQ/TIME/DENS formats */
    const char *enums;  /* '|' separated labels for FMT_ENUM */
    int step;           /* 1 = integer steps */
} param_def_t;

/* ---- Indices ---- */
enum {
    /* Material page 1 */
    P_MODE = 0, P_START, P_LEN, P_PITCH, P_DIR, P_ATK, P_REL, P_LEVEL_M,
    /* Material page 2 */
    P_FINE, P_PADMODE, P_SRC, P_MON, P_ODUB, P_QUANT, P_INGAIN,
    /* Granular */
    P_GMIX, P_GSIZE, P_GDENS, P_GPITCH, P_GSPRAY, P_GCONT, P_GSPREAD, P_GFREEZE,
    P_GREV, P_GPRND, P_GFB, P_GSYNC,
    /* Filter: 48-band resonant filter bank */
    P_FCUT, P_FMORPH, P_FRES, P_FDECAY, P_FPITCH, P_FSCALE, P_FDRIVE, P_FMIX,
    P_FWAVE, P_FWRATE, P_FWSIZE, P_FNOISE, P_FNTEX, P_FSPREAD, P_FENV, P_FSLOPE,
    /* Color */
    P_CDRIVE, P_CSPLIT, P_CBAL, P_CBITS, P_CRATE, P_CCOMP, P_CNOISE, P_CMIX,
    /* Space */
    P_DTIME, P_DFB, P_DTYPE, P_DMIX, P_RSIZE, P_RDECAY, P_RDAMP, P_RMIX,
    P_DSYNC, P_FREEZE, P_DTONE,
    /* Mixer */
    P_LEVEL, P_PAN, P_DJF, P_MUTE,
    /* Modulators: 4 x 8 */
    P_MOD0,
    NP = P_MOD0 + 32
};

/* Per-mod offsets */
enum { M_TYPE = 0, M_RATE, M_DEPTH, M_TARGET, M_SYNC, M_SHAPE, M_RETRIG, M_POL, M_COUNT };

#define MOD_TYPES "Sine|Tri|Saw|Square|S&H|Drift|Env|Follow"

/* Global parameters */
enum { G_COMP = 0, G_MORPH, G_DRIVE, G_OUT, NG };

#define MAX_MOD_TARGETS 64

#define PD(k,n,s,d,pg,sl,mn,mx,df,f,lo,hi,en,st) \
    { k, n, s, d, pg, sl, mn, mx, df, f, lo, hi, en, st }

#define MODDEFS(i) \
    PD("m" #i "type",  "Mod" #i " Type",  "Type",  DEV_MOD, i-1, 0, 0, 7, 0, FMT_ENUM, 0,0, MOD_TYPES, 1), \
    PD("m" #i "rate",  "Mod" #i " Rate",  "Rate",  DEV_MOD, i-1, 1, 0, 1, 0.4f, FMT_RATE, 0.02f,20.0f, 0, 0), \
    PD("m" #i "depth", "Mod" #i " Depth", "Depth", DEV_MOD, i-1, 2, -1, 1, 0, FMT_BIP, 0,0, 0, 0), \
    PD("m" #i "tgt",   "Mod" #i " Target","Tgt",   DEV_MOD, i-1, 3, 0, MAX_MOD_TARGETS, 0, FMT_TARGET, 0,0, 0, 1), \
    PD("m" #i "sync",  "Mod" #i " Sync",  "Sync",  DEV_MOD, i-1, 4, 0, 1, 0, FMT_ENUM, 0,0, "Free|Sync", 1), \
    PD("m" #i "shape", "Mod" #i " Shape", "Shape", DEV_MOD, i-1, 5, 0, 1, 0.5f, FMT_PCT, 0,0, 0, 0), \
    PD("m" #i "retr",  "Mod" #i " Retrig","Retrg", DEV_MOD, i-1, 6, 0, 1, 0, FMT_ENUM, 0,0, "Off|On", 1), \
    PD("m" #i "pol",   "Mod" #i " Polarity","Pol", DEV_MOD, i-1, 7, 0, 1, 0, FMT_ENUM, 0,0, "Bi|Uni", 1)

static const param_def_t PARAMS[NP] = {
    /* ---------------- Material ---------------- */
    PD("mode",   "Mode",        "Mode",  DEV_MAT, 0, 0, 0, 1, 0,     FMT_ENUM, 0,0, "Tape|Poly", 1),
    PD("start",  "Start",       "Start", DEV_MAT, 0, 1, 0, 1, 0,     FMT_PCT,  0,0, 0, 0),
    PD("len",    "Length",      "Len",   DEV_MAT, 0, 2, 0, 1, 1,     FMT_PCT,  0,0, 0, 0),
    PD("pitch",  "Pitch",       "Pitch", DEV_MAT, 0, 3, -24, 24, 0,  FMT_SEMI, 0,0, 0, 1),
    PD("dir",    "Direction",   "Dir",   DEV_MAT, 0, 4, 0, 2, 0,     FMT_ENUM, 0,0, "Fwd|Rev|Pong", 1),
    PD("atk",    "Attack",      "Atk",   DEV_MAT, 0, 5, 0, 1, 0.05f, FMT_TIME, 1,2000, 0, 0),
    PD("rel",    "Release",     "Rel",   DEV_MAT, 0, 6, 0, 1, 0.3f,  FMT_TIME, 5,4000, 0, 0),
    PD("mlevel", "Mat Level",   "Level", DEV_MAT, 0, 7, 0, 1, 0.8f,  FMT_PCT,  0,0, 0, 0),
    PD("fine",   "Fine Tune",   "Fine",  DEV_MAT, 1, 0, -100, 100, 0, FMT_CENT, 0,0, 0, 1),
    PD("padmode","Pad Mode",    "Pads",  DEV_MAT, 1, 1, 0, 1, 0,     FMT_ENUM, 0,0, "Slice|Chroma", 1),
    PD("src",    "Rec Source",  "Src",   DEV_MAT, 1, 2, 0, 1, 0,     FMT_ENUM, 0,0, "Input|Master", 1),
    PD("mon",    "Live Input",  "Live",  DEV_MAT, 1, 3, 0, 1, 0,     FMT_ENUM, 0,0, "Off|On", 1),
    PD("odub",   "Overdub Keep","Keep",  DEV_MAT, 1, 4, 0, 1, 0.8f,  FMT_PCT,  0,0, 0, 0),
    PD("quant",  "Rec Quantize","Quant", DEV_MAT, 1, 5, 0, 2, 2,     FMT_ENUM, 0,0, "Free|Beat|Bar", 1),
    PD("ingain", "Input Gain",  "InGn",  DEV_MAT, 1, 6, 0, 1, 0.5f,  FMT_PCT,  0,0, 0, 0),

    /* ---------------- Granular ---------------- */
    PD("gmix",   "Grain Mix",   "Mix",   DEV_GRN, 0, 0, 0, 1, 0,     FMT_PCT,  0,0, 0, 0),
    PD("gsize",  "Grain Size",  "Size",  DEV_GRN, 0, 1, 0, 1, 0.5f,  FMT_TIME, 10,1000, 0, 0),
    PD("gdens",  "Density",     "Dens",  DEV_GRN, 0, 2, 0, 1, 0.5f,  FMT_DENS, 1,80, 0, 0),
    PD("gpitch", "Grain Pitch", "Pitch", DEV_GRN, 0, 3, -24, 24, 0,  FMT_SEMI, 0,0, 0, 1),
    PD("gspray", "Spray",       "Spray", DEV_GRN, 0, 4, 0, 1, 0.1f,  FMT_PCT,  0,0, 0, 0),
    PD("gcont",  "Contour",     "Contr", DEV_GRN, 0, 5, 0, 1, 0.5f,  FMT_PCT,  0,0, 0, 0),
    PD("gspread","Grain Spread","Sprd",  DEV_GRN, 0, 6, 0, 1, 0.5f,  FMT_PCT,  0,0, 0, 0),
    PD("gfreeze","Grain Freeze","Frz",   DEV_GRN, 0, 7, 0, 1, 0,     FMT_ENUM, 0,0, "Off|On", 1),
    PD("grev",   "Reverse Prob","Rev",   DEV_GRN, 1, 0, 0, 1, 0,     FMT_PCT,  0,0, 0, 0),
    PD("gprnd",  "Pitch Random","PRnd",  DEV_GRN, 1, 1, 0, 1, 0,     FMT_PCT,  0,0, 0, 0),
    PD("gfb",    "Grain Fdbk",  "Fdbk",  DEV_GRN, 1, 2, 0, 1, 0,     FMT_PCT,  0,0, 0, 0),
    PD("gsync",  "Grain Sync",  "Sync",  DEV_GRN, 1, 3, 0, 1, 0,     FMT_ENUM, 0,0, "Free|Sync", 1),

    /* ---------------- Filter: 48-band resonant bank ---------------- */
    PD("fcut",   "Cutoff",      "Cut",   DEV_FLT, 0, 0, 0, 1, 1,     FMT_FREQ, 20,20000, 0, 0),
    PD("fmorph", "Slope Morph", "Morph", DEV_FLT, 0, 1, 0, 1, 0,     FMT_MORPH, 0,0, 0, 0),
    PD("fres",   "Resonance",   "Res",   DEV_FLT, 0, 2, 0, 1, 0,     FMT_PCT,  0,0, 0, 0),
    PD("fdecay", "Band Decay",  "Decay", DEV_FLT, 0, 3, 0, 1, 0,     FMT_DECAY, 50,4000, 0, 0),
    PD("fpitch", "Bank Pitch",  "Pitch", DEV_FLT, 0, 4, -24, 24, 0,  FMT_SEMI, 0,0, 0, 1),
    PD("fscale", "Bank Scale",  "Scale", DEV_FLT, 0, 5, 0, 8, 0,     FMT_ENUM, 0,0, "Free|Chrom|Major|Minor|Dorian|Penta|MinPen|Whole|Notes", 1),
    PD("fdrive", "Filter Drive","Drive", DEV_FLT, 0, 6, 0, 1, 0,     FMT_PCT,  0,0, 0, 0),
    PD("fmix",   "Filter Mix",  "Mix",   DEV_FLT, 0, 7, 0, 1, 1,     FMT_PCT,  0,0, 0, 0),
    PD("fwave",  "Waves",       "Waves", DEV_FLT, 1, 0, 0, 1, 0,     FMT_PCT,  0,0, 0, 0),
    PD("fwrate", "Wave Rate",   "WRate", DEV_FLT, 1, 1, 0, 1, 0.35f, FMT_LFO,  0.02f,12.0f, 0, 0),
    PD("fwsize", "Wave Size",   "WSize", DEV_FLT, 1, 2, 0, 1, 0.3f,  FMT_PCT,  0,0, 0, 0),
    PD("fnoise", "Noise",       "Noise", DEV_FLT, 1, 3, 0, 1, 0,     FMT_PCT,  0,0, 0, 0),
    PD("fntex",  "Noise Texture","Textr",DEV_FLT, 1, 4, 0, 1, 0.5f,  FMT_PCT,  0,0, 0, 0),
    PD("fspread","Stereo Spread","Sprd", DEV_FLT, 1, 5, 0, 1, 0,     FMT_PCT,  0,0, 0, 0),
    PD("fenv",   "Env Amount",  "Env",   DEV_FLT, 1, 6, -1, 1, 0,    FMT_BIP,  0,0, 0, 0),
    PD("fslope", "Slope",       "Slope", DEV_FLT, 1, 7, 0, 1, 0.43f, FMT_SLOPE, 0,0, 0, 0),

    /* ---------------- Color ---------------- */
    PD("cdrive", "Drive",       "Drive", DEV_COL, 0, 0, 0, 1, 0,     FMT_PCT,  0,0, 0, 0),
    PD("csplit", "Band Split",  "Split", DEV_COL, 0, 1, 0, 1, 0.5f,  FMT_FREQ, 80,8000, 0, 0),
    PD("cbal",   "Low/High",    "L/H",   DEV_COL, 0, 2, -1, 1, 0,    FMT_BIP,  0,0, 0, 0),
    PD("cbits",  "Bit Crush",   "Bits",  DEV_COL, 0, 3, 0, 1, 0,     FMT_BITS, 0,0, 0, 0),
    PD("crate",  "Downsample",  "Rate",  DEV_COL, 0, 4, 0, 1, 0,     FMT_PCT,  0,0, 0, 0),
    PD("ccomp",  "Compress",    "Comp",  DEV_COL, 0, 5, 0, 1, 0,     FMT_PCT,  0,0, 0, 0),
    PD("cnoise", "Noise",       "Noise", DEV_COL, 0, 6, 0, 1, 0,     FMT_PCT,  0,0, 0, 0),
    PD("cmix",   "Color Mix",   "Mix",   DEV_COL, 0, 7, 0, 1, 1,     FMT_PCT,  0,0, 0, 0),

    /* ---------------- Space ---------------- */
    PD("dtime",  "Delay Time",  "Time",  DEV_SPC, 0, 0, 0, 1, 0.5f,  FMT_DTIME, 10,2000, 0, 0),
    PD("dfb",    "Delay Fdbk",  "Fdbk",  DEV_SPC, 0, 1, 0, 1, 0.4f,  FMT_PCT,  0,0, 0, 0),
    PD("dtype",  "Delay Type",  "Type",  DEV_SPC, 0, 2, 0, 2, 1,     FMT_ENUM, 0,0, "Clean|Tape|Pong", 1),
    PD("dmix",   "Delay Mix",   "DMix",  DEV_SPC, 0, 3, 0, 1, 0,     FMT_PCT,  0,0, 0, 0),
    PD("rsize",  "Verb Size",   "Size",  DEV_SPC, 0, 4, 0, 1, 0.6f,  FMT_PCT,  0,0, 0, 0),
    PD("rdecay", "Verb Decay",  "Decay", DEV_SPC, 0, 5, 0, 1, 0.5f,  FMT_PCT,  0,0, 0, 0),
    PD("rdamp",  "Verb Damp",   "Damp",  DEV_SPC, 0, 6, 0, 1, 0.4f,  FMT_PCT,  0,0, 0, 0),
    PD("rmix",   "Verb Mix",    "RMix",  DEV_SPC, 0, 7, 0, 1, 0,     FMT_PCT,  0,0, 0, 0),
    PD("dsync",  "Delay Sync",  "Sync",  DEV_SPC, 1, 0, 0, 1, 1,     FMT_ENUM, 0,0, "Free|Sync", 1),
    PD("freeze", "Freeze",      "Frz",   DEV_SPC, 1, 1, 0, 1, 0,     FMT_ENUM, 0,0, "Off|On", 1),
    PD("dtone",  "Delay Tone",  "Tone",  DEV_SPC, 1, 2, 0, 1, 0.7f,  FMT_PCT,  0,0, 0, 0),

    /* ---------------- Mixer ---------------- */
    PD("level",  "Track Level", "Lvl",   DEV_MIX, 0, 0, 0, 1, 0.7f,  FMT_PCT,  0,0, 0, 0),
    PD("pan",    "Pan",         "Pan",   DEV_MIX, 0, 1, -1, 1, 0,    FMT_BIP,  0,0, 0, 0),
    PD("djf",    "DJ Filter",   "DJF",   DEV_MIX, 0, 2, -1, 1, 0,    FMT_BIP,  0,0, 0, 0),
    PD("mute",   "Mute",        "Mute",  DEV_MIX, 0, 3, 0, 1, 0,     FMT_ENUM, 0,0, "Off|On", 1),

    /* ---------------- Modulators ---------------- */
    MODDEFS(1), MODDEFS(2), MODDEFS(3), MODDEFS(4)
};

static const param_def_t GPARAMS[NG] = {
    PD("gcomp",  "Master Comp", "Comp",  DEV_MIX, 1, 4, 0, 1, 0.3f,  FMT_PCT,  0,0, 0, 0),
    PD("gmorph", "Scene Morph", "Morph", DEV_MIX, 1, 5, 0, 1, 0,     FMT_TIME, 1,8000, 0, 0),
    PD("gdrive", "Master Drive","Drive", DEV_MIX, 1, 6, 0, 1, 0,     FMT_PCT,  0,0, 0, 0),
    PD("gout",   "Master Out",  "Out",   DEV_MIX, 1, 7, 0, 1, 0.8f,  FMT_PCT,  0,0, 0, 0),
};

/* Sync divisions (in beats) shared by delay, grains and mods */
static const float DELAY_DIVS[12] = {
    0.125f, 1.0f/6, 0.25f, 1.0f/3, 0.375f, 0.5f, 2.0f/3, 0.75f, 1.0f, 1.5f, 2.0f, 4.0f
};
static const char *DELAY_DIV_NAMES = "1/32|1/16T|1/16|1/8T|1/16D|1/8|1/4T|1/8D|1/4|1/4D|1/2|1bar";

static const float MOD_DIVS[10] = { 32, 16, 8, 4, 2, 1, 0.5f, 0.25f, 0.125f, 1.0f/16 };
static const char *MOD_DIV_NAMES = "8bar|4bar|2bar|1bar|1/2|1/4|1/8|1/16|1/32|1/64";

static const float GRAIN_DIVS[7] = { 1, 0.5f, 0.25f, 0.125f, 1.0f/16, 1.0f/32, 1.0f/64 };
static const char *GRAIN_DIV_NAMES = "1/4|1/8|1/16|1/32|1/64|1/128|1/256";

#endif
