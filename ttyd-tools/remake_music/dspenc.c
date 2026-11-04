// Minimal GameCube DSP-ADPCM encoder for the remake-music converter.
// Usage: dspenc <in.s16le mono> <out.bin> <loop_start_sample or -1>
// Output: 16 coefs (s16 BE), ps/yn1/yn2 at loop start (u16/s16/s16 BE),
// then the encoded ADPCM frames (8 bytes per 14 samples).
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FS 14
#define NPRED 8

static int clamp16(int v) { return v < -32768 ? -32768 : v > 32767 ? 32767 : v; }

// Least-squares 2nd-order predictor fit from accumulated sums.
static void solve(const double* acc, double* c1, double* c2) {
    // acc: r11, r12, r22, r01, r02 (x[n]~c1*x[n-1]+c2*x[n-2])
    double r11 = acc[0] + 1e-3, r12 = acc[1], r22 = acc[2] + 1e-3, r01 = acc[3], r02 = acc[4];
    double det = r11 * r22 - r12 * r12;
    if (fabs(det) < 1e-9) { *c1 = 0; *c2 = 0; return; }
    *c1 = (r01 * r22 - r02 * r12) / det;
    *c2 = (r02 * r11 - r01 * r12) / det;
    // Keep the predictor stable and within the 4.11 fixed-point range.
    if (*c2 > 0.999) *c2 = 0.999;
    if (*c2 < -0.999) *c2 = -0.999;
    if (*c1 > 1.999) *c1 = 1.999;
    if (*c1 < -1.999) *c1 = -1.999;
}

// Encodes one frame with the given predictor; returns squared error.
static double enc_frame(const int16_t* x, int n, int c1, int c2, int y1, int y2,
                        int scale, uint8_t* out, int* oy1, int* oy2) {
    double err = 0;
    int s = 1 << scale;
    for (int i = 0; i < FS; ++i) {
        int target = i < n ? x[i] : 0;
        int pred = (c1 * y1 + c2 * y2 + 1024) >> 11;
        int diff = target - pred;
        int q = (int)lrint((double)diff / s);
        if (q > 7) q = 7;
        if (q < -8) q = -8;
        int dec = clamp16((((q * s) << 11) + 1024 + c1 * y1 + c2 * y2) >> 11);
        // Correct q if rounding pushed the decoded value away (clamping).
        if (out) {
            if (i % 2 == 0) out[1 + i / 2] = (uint8_t)((q & 15) << 4);
            else out[1 + i / 2] |= (uint8_t)(q & 15);
        }
        double e = (double)(target - dec);
        if (i < n) err += e * e;
        y2 = y1; y1 = dec;
    }
    *oy1 = y1; *oy2 = y2;
    return err;
}

int main(int argc, char** argv) {
    if (argc < 4) { fprintf(stderr, "usage: dspenc in.raw out.bin loop_start\n"); return 1; }
    FILE* f = fopen(argv[1], "rb");
    if (!f) { perror("in"); return 1; }
    fseek(f, 0, SEEK_END); long bytes = ftell(f); fseek(f, 0, SEEK_SET);
    long ns = bytes / 2;
    int16_t* x = calloc(ns + 2 * FS, sizeof(int16_t));
    if (fread(x + 2, 2, ns, f) != (size_t)ns) { fprintf(stderr, "read fail\n"); return 1; }
    fclose(f);
    int16_t* s = x + 2;  // two leading zeros of history
    long loop_start = atol(argv[3]);
    long nframes = (ns + FS - 1) / FS;

    // --- Coefficient search: k-means over per-frame predictors. ---
    double pc1[NPRED], pc2[NPRED];
    // Seed with a spread of typical predictors.
    const double seed[NPRED][2] = {{0, 0}, {1, 0}, {2, -1}, {1.6, -0.7}, {1.2, -0.3},
                                   {1.9, -0.95}, {0.5, 0.2}, {1.75, -0.85}};
    for (int k = 0; k < NPRED; ++k) { pc1[k] = seed[k][0]; pc2[k] = seed[k][1]; }
    int* assign = calloc(nframes, sizeof(int));
    for (int it = 0; it < 12; ++it) {
        double acc[NPRED][5];
        memset(acc, 0, sizeof(acc));
        for (long fr = 0; fr < nframes; ++fr) {
            const int16_t* p = s + fr * FS;
            double best = 1e300; int bk = 0;
            for (int k = 0; k < NPRED; ++k) {
                double e = 0;
                for (int i = 0; i < FS; ++i) {
                    double r = p[i] - pc1[k] * p[i - 1] - pc2[k] * p[i - 2];
                    e += r * r;
                }
                if (e < best) { best = e; bk = k; }
            }
            assign[fr] = bk;
            for (int i = 0; i < FS; ++i) {
                double a = p[i], b = p[i - 1], c = p[i - 2];
                acc[bk][0] += b * b; acc[bk][1] += b * c; acc[bk][2] += c * c;
                acc[bk][3] += a * b; acc[bk][4] += a * c;
            }
        }
        for (int k = 0; k < NPRED; ++k)
            if (acc[k][0] > 0) solve(acc[k], &pc1[k], &pc2[k]);
    }
    int16_t co[16];
    for (int k = 0; k < NPRED; ++k) {
        co[k * 2] = (int16_t)lrint(pc1[k] * 2048);
        co[k * 2 + 1] = (int16_t)lrint(pc2[k] * 2048);
    }

    // --- Encode. ---
    uint8_t* outdata = calloc(nframes, 8);
    int y1 = 0, y2 = 0;
    uint16_t lps = 0; int16_t ly1 = 0, ly2 = 0;
    for (long fr = 0; fr < nframes; ++fr) {
        const int16_t* p = s + fr * FS;
        int n = (int)(ns - fr * FS < FS ? ns - fr * FS : FS);
        double best = 1e300; int bk = 0, bs = 0;
        for (int k = 0; k < NPRED; ++k) {
            for (int sc = 0; sc < 12; ++sc) {
                int a, b;
                double e = enc_frame(p, n, co[k * 2], co[k * 2 + 1], y1, y2, sc, NULL, &a, &b);
                if (e < best) { best = e; bk = k; bs = sc; }
            }
        }
        uint8_t* o = outdata + fr * 8;
        if (fr * FS == loop_start) { ly1 = (int16_t)y1; ly2 = (int16_t)y2; }
        o[0] = (uint8_t)((bk << 4) | bs);
        if (fr * FS == loop_start) lps = o[0];
        enc_frame(p, n, co[bk * 2], co[bk * 2 + 1], y1, y2, bs, o, &y1, &y2);
    }

    FILE* g = fopen(argv[2], "wb");
    uint8_t hdr[32 + 6];
    for (int i = 0; i < 16; ++i) { hdr[i * 2] = (uint8_t)(co[i] >> 8); hdr[i * 2 + 1] = (uint8_t)co[i]; }
    hdr[32] = (uint8_t)(lps >> 8); hdr[33] = (uint8_t)lps;
    hdr[34] = (uint8_t)((uint16_t)ly1 >> 8); hdr[35] = (uint8_t)ly1;
    hdr[36] = (uint8_t)((uint16_t)ly2 >> 8); hdr[37] = (uint8_t)ly2;
    fwrite(hdr, 1, sizeof(hdr), g);
    fwrite(outdata, 8, nframes, g);
    fclose(g);
    return 0;
}
