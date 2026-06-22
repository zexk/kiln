#include "noise.h"
#include <math.h>

static unsigned char perm[512];

/* 8 unit gradient vectors in 2D */
static const float grad2[8][2] = {
    { 1.0f, 0.0f}, {-1.0f, 0.0f}, { 0.0f, 1.0f}, { 0.0f,-1.0f},
    { 1.0f, 1.0f}, {-1.0f, 1.0f}, { 1.0f,-1.0f}, {-1.0f,-1.0f},
};

void noise_init(unsigned int seed) {
    for (int i = 0; i < 256; i++) perm[i] = (unsigned char)i;
    for (int i = 255; i > 0; i--) {
        seed = seed * 1664525u + 1013904223u;
        int j = (int)((seed >> 16) % (unsigned)(i + 1));
        unsigned char tmp = perm[i]; perm[i] = perm[j]; perm[j] = tmp;
    }
    for (int i = 0; i < 256; i++) perm[i + 256] = perm[i];
}

static float fade(float t) {
    return t * t * t * (t * (t * 6.0f - 15.0f) + 10.0f);
}

static float lerpf(float a, float b, float t) { return a + t * (b - a); }

static float grad(int hash, float x, float y) {
    const float *g = grad2[hash & 7];
    return g[0] * x + g[1] * y;
}

static float noise_perlin2d(float x, float y) {
    int xi = (int)floorf(x) & 255;
    int yi = (int)floorf(y) & 255;
    float xf = x - floorf(x);
    float yf = y - floorf(y);
    float u = fade(xf);
    float v = fade(yf);

    int aa = perm[perm[xi    ] + yi    ];
    int ba = perm[perm[xi + 1] + yi    ];
    int ab = perm[perm[xi    ] + yi + 1];
    int bb = perm[perm[xi + 1] + yi + 1];

    float res = lerpf(
        lerpf(grad(aa, xf,        yf       ), grad(ba, xf - 1.0f, yf       ), u),
        lerpf(grad(ab, xf,        yf - 1.0f), grad(bb, xf - 1.0f, yf - 1.0f), u),
        v);

    return (res + 1.0f) * 0.5f;
}

static const float grad3[16][3] = {
    { 1, 1, 0},{-1, 1, 0},{ 1,-1, 0},{-1,-1, 0},
    { 1, 0, 1},{-1, 0, 1},{ 1, 0,-1},{-1, 0,-1},
    { 0, 1, 1},{ 0,-1, 1},{ 0, 1,-1},{ 0,-1,-1},
    { 1, 1, 0},{-1, 1, 0},{ 0,-1, 1},{ 0,-1,-1},
};

static float grad3d(int hash, float x, float y, float z) {
    const float *g = grad3[hash & 15];
    return g[0]*x + g[1]*y + g[2]*z;
}

static float noise_perlin3d(float x, float y, float z) {
    int xi = (int)floorf(x) & 255;
    int yi = (int)floorf(y) & 255;
    int zi = (int)floorf(z) & 255;
    float xf = x - floorf(x), yf = y - floorf(y), zf = z - floorf(z);
    float u = fade(xf), v = fade(yf), w = fade(zf);
    int aaa = perm[perm[perm[xi  ]+yi  ]+zi  ], baa = perm[perm[perm[xi+1]+yi  ]+zi  ];
    int aba = perm[perm[perm[xi  ]+yi+1]+zi  ], bba = perm[perm[perm[xi+1]+yi+1]+zi  ];
    int aab = perm[perm[perm[xi  ]+yi  ]+zi+1], bab = perm[perm[perm[xi+1]+yi  ]+zi+1];
    int abb = perm[perm[perm[xi  ]+yi+1]+zi+1], bbb = perm[perm[perm[xi+1]+yi+1]+zi+1];
    float res = lerpf(
        lerpf(lerpf(grad3d(aaa,xf,  yf,  zf  ),grad3d(baa,xf-1,yf,  zf  ),u),
              lerpf(grad3d(aba,xf,  yf-1,zf  ),grad3d(bba,xf-1,yf-1,zf  ),u),v),
        lerpf(lerpf(grad3d(aab,xf,  yf,  zf-1),grad3d(bab,xf-1,yf,  zf-1),u),
              lerpf(grad3d(abb,xf,  yf-1,zf-1),grad3d(bbb,xf-1,yf-1,zf-1),u),v),w);
    return (res + 1.0f) * 0.5f;
}

float noise_fbm2d(float x, float y, int octaves, float lacunarity, float gain) {
    float value     = 0.0f;
    float amplitude = 1.0f;
    float frequency = 1.0f;
    float weight    = 0.0f;
    for (int i = 0; i < octaves; i++) {
        value     += noise_perlin2d(x * frequency, y * frequency) * amplitude;
        weight    += amplitude;
        amplitude *= gain;
        frequency *= lacunarity;
    }
    return value / weight;
}

float noise_fbm3d(float x, float y, float z, int octaves, float lacunarity, float gain) {
    float value = 0.0f, amplitude = 1.0f, frequency = 1.0f, weight = 0.0f;
    for (int i = 0; i < octaves; i++) {
        value     += noise_perlin3d(x*frequency, y*frequency, z*frequency) * amplitude;
        weight    += amplitude;
        amplitude *= gain;
        frequency *= lacunarity;
    }
    return value / weight;
}
